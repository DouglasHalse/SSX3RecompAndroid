/*
 * Android entry point.
 *
 * SDL2's Java activity calls SDL_main on a native thread it owns; SDL video
 * must be initialised and used from that thread. The game's own presenter
 * (toolkit src/video/fb_present_sdl.c) would create the window from a thread
 * SDL does not own, so on Android the two are split here:
 *
 *   SDL thread   -> SDL_Init(VIDEO), window, event pump, frame blit
 *                   (xbox_FramebufferWindowRunHere, the toolkit's loop)
 *   game thread  -> ssx3_main(), i.e. WinMain and the whole guest
 *
 * The process is put into <external files>/ssx3 before anything else, with
 * game/ (default.xbe + data/) below it and HOME pointing at home/, so saves
 * (game/UDATA) and the toolkit's system images stay inside the app's dirs.
 */
#include <SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>

extern int ssx3_main(int argc, char **argv);        /* src/main.c, -Dmain=ssx3_main */
extern void xbox_FramebufferWindowRunHere(void);    /* toolkit: fb_present_sdl.c */

static int    s_argc;
static char **s_argv;

/* The port talks on stdout/stderr ([BOOT], [VKR], [CRASH], [PATH] ...). On
 * Android neither goes anywhere, so both are pointed at a pipe that a thread
 * copies into logcat: `adb logcat -s SSX3`. */
static int s_log_fd[2];

static void *log_reader(void *unused)
{
    static char line[1024];
    char buf[512];
    size_t n = 0;
    ssize_t r;

    (void)unused;
    while ((r = read(s_log_fd[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || n >= sizeof line - 1) {
                line[n] = 0;
                __android_log_write(ANDROID_LOG_INFO, "SSX3", line);
                n = 0;
            } else if (buf[i] != '\r') {
                line[n++] = buf[i];
            }
        }
    }
    return NULL;
}

static void log_to_logcat(void)
{
    pthread_t t;

    if (pipe(s_log_fd) != 0)
        return;
    dup2(s_log_fd[1], STDOUT_FILENO);
    dup2(s_log_fd[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pthread_create(&t, NULL, log_reader, NULL) == 0)
        pthread_detach(t);
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path);

    if (n == 0 || n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void setenv_def(const char *k, const char *v)
{
    if (!getenv(k))
        setenv(k, v, 1);
}

/* Optional knobs without a rebuild: <root>/recomp.cfg, one KEY=VALUE per line,
 * '#' starts a comment. Real environment variables win, and the values are
 * applied before the port reads them (they are read from ssx3_main onwards). */
static void load_env_file(const char *root)
{
    char path[1200], line[512];
    FILE *f;
    int n = 0;

    snprintf(path, sizeof path, "%s/recomp.cfg", root);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *val, *end;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == 0)
            continue;
        if ((eq = strchr(p, '=')) == NULL)
            continue;
        *eq = 0;
        val = eq + 1;
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '
                             || end[-1] == '\t'))
            *--end = 0;
        end = eq;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        if (!*p)
            continue;
        setenv(p, val, 0);
        n++;
    }
    fclose(f);
    if (n)
        fprintf(stderr, "[ANDROID] %d setting(s) from %s\n", n, path);
}

/* stdout/stderr go through a pipe that a thread copies into logcat, so a
 * process that exits immediately can lose the very lines that explain why it
 * exited. Give the reader a moment to drain. */
static void log_drain(void)
{
    fflush(NULL);
    usleep(200 * 1000);
}

static void *game_thread(void *arg)
{
    (void)arg;
    int rc = ssx3_main(s_argc, s_argv);
    fprintf(stderr, "[ANDROID] game returned %d\n", rc);
    log_drain();
    /* The presenter loop owns the SDL thread; stop it once the guest is done. */
    _exit(rc);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *ext = SDL_AndroidGetExternalStoragePath();
    const char *in  = SDL_AndroidGetInternalStoragePath();
    char root[1024], home[1024];
    pthread_t th;

    s_argc = argc;
    s_argv = argv;

    log_to_logcat();

    if (ext && *ext)
        snprintf(root, sizeof root, "%s/ssx3", ext);
    else if (in && *in)
        snprintf(root, sizeof root, "%s/ssx3", in);
    else
        snprintf(root, sizeof root, ".");
    mkdir_p(root);

    /* Android/data/<pkg> cannot be written by adb or a file manager on a
     * retail phone (scoped storage), so the rip may also be dropped where
     * anything can write it: /sdcard/Download/ssx3. Reading it needs the
     * "All files access" grant (MANAGE_EXTERNAL_STORAGE), so the fallback is
     * only taken when the XBE really opens; otherwise the app folder is used. */
    {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/game/default.xbe", root);
        if (access(probe, F_OK) != 0) {
            FILE *f = fopen("/sdcard/Download/ssx3/game/default.xbe", "rb");
            if (f) {
                fclose(f);
                snprintf(root, sizeof root, "/sdcard/Download/ssx3");
                fprintf(stderr, "[ANDROID] game files found in %s\n", root);
            }
        }
    }

    snprintf(home, sizeof home, "%s/home", root);
    mkdir_p(home);

    if (chdir(root) != 0)
        fprintf(stderr, "[ANDROID] chdir %s failed\n", root);

    /* $HOME / $XDG_DATA_HOME keep the toolkit's system partition images and
     * the guest's save dir inside the app sandbox on Android. */
    setenv("HOME", home, 1);
    {
        char xdg[1100];
        snprintf(xdg, sizeof xdg, "%s/.local/share", home);
        mkdir_p(xdg);
        setenv("XDG_DATA_HOME", xdg, 1);
    }
    setenv("TMPDIR", root, 1);

    load_env_file(root);

    /* Display: the window is the phone's screen; render internally at a
     * height the GPU holds 60 FPS at, then scale. RECOMP_RES can override. */
    setenv_def("RECOMP_RES", "720");
    setenv_def("RECOMP_WIDESCREEN", "auto");
    setenv_def("RECOMP_SKIP_INTRO", "1");
    setenv_def("RECOMP_FAST_BOOT", "4");
    setenv_def("RECOMP_KEYBOARD", "1");     /* pad still works (SDL controller) */
    setenv_def("RECOMP_FB_WINDOW", "1");
    setenv_def("RECOMP_HUD_EDGES", "1");
    setenv_def("RECOMP_HDTEX", "hd-textures");

    fprintf(stderr, "[ANDROID] root=%s\n", root);

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[ANDROID] SDL video init failed: %s\n", SDL_GetError());
        log_drain();
        return 1;
    }

    if (pthread_create(&th, NULL, game_thread, NULL) != 0) {
        fprintf(stderr, "[ANDROID] cannot start the game thread\n");
        log_drain();
        return 1;
    }
    pthread_detach(th);

    /* Blocks for the life of the app: SDL events, frame blit, FPS title. */
    xbox_FramebufferWindowRunHere();
    fprintf(stderr, "[ANDROID] presenter returned, exiting\n");
    return 0;
}
