/*
 * settings.c -- the options table, their file, and the live ones.
 *
 * Every row is an environment variable the port already reads, so applying a
 * choice is setting the variable and making sure whoever reads it looks
 * again: a consumer of a SSX3_LIVE row re-reads when the generation moves.
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "video/ui_overlay.h"       /* xbox_FramebufferSetFpsOverlay */

#if defined(__ANDROID__)
#define PLATFORM_ONLY SSX3_AND_ONLY
#else
#define PLATFORM_ONLY SSX3_WIN_ONLY
#endif

/* ---- the choices ------------------------------------------------------- */

static const char *const v_res[]    = {"", "480", "720", "1080", "1440", "2160"};
static const char *const n_res[]    = {"AUTO", "480P", "720P", "1080P", "1440P", "2160P"};
static const char *const v_aspect[] = {"auto", "16:9", "21:9", "32:9", "0"};
static const char *const n_aspect[] = {"AUTO", "16:9", "21:9", "32:9", "4:3"};
static const char *const v_msaa[]   = {"0", "2", "4", "8"};
static const char *const n_msaa[]   = {"OFF", "2X", "4X", "8X"};
static const char *const v_aniso[]  = {"0", "4", "8", "16"};
static const char *const n_aniso[]  = {"OFF", "4X", "8X", "16X"};
static const char *const v_dd[]     = {"1", "1.5", "2"};
static const char *const n_dd[]     = {"ORIGINAL", "FAR", "MAXIMUM"};
static const char *const v_shadow[] = {"1", "4", "8"};
static const char *const n_shadow[] = {"128 X 128", "512 X 512", "1024 X 1024"};
static const char *const v_audio[]  = {"2", "6"};
static const char *const n_audio[]  = {"STEREO", "5.1 SURROUND"};
static const char *const v_onoff[]  = {"0", "1"};
static const char *const n_onoff[]  = {"OFF", "ON"};
static const char *const v_hdtex[]  = {"0", "hd-textures"};
static const char *const n_hdtex[]  = {"ORIGINAL", "HD PACK"};
static const char *const v_fast[]   = {"0", "4"};
static const char *const n_fast[]   = {"OFF", "ON"};
static const char *const v_log[]    = {"error", "warn", "info", "debug", "trace"};
static const char *const n_log[]    = {"ERRORS", "WARNINGS", "NORMAL", "DETAILED", "EVERYTHING"};

#define N(a) ((int)(sizeof (a) / sizeof (a)[0]))

const Ssx3Setting ssx3_settings[] = {
    {"RECOMP_RES", "RESOLUTION", "Height the game renders at, before scaling to the window.",
     v_res, n_res, N(v_res), 0, 2, 0},
    {"RECOMP_WIDESCREEN", "SCREEN ASPECT", "AUTO follows the window.",
     v_aspect, n_aspect, N(v_aspect), 0, 0, 0},
    {"RECOMP_MSAA", "ANTIALIASING", "Multisampling: smooths polygon edges, costs fill rate.",
     v_msaa, n_msaa, N(v_msaa), 2, 0, 0},
    {"RECOMP_ANISO", "ANISOTROPIC FILTERING", "Sharpens ground textures seen at a shallow angle.",
     v_aniso, n_aniso, N(v_aniso), 3, 3, 0},
    {"RECOMP_DRAW_DIST", "DRAW DISTANCE", "How far the end of the view is pushed out.",
     v_dd, n_dd, N(v_dd), 1, 1, SSX3_LIVE},
    {"RECOMP_SHADOW_RES", "SHADOW QUALITY", "Rider shadow map size.",
     v_shadow, n_shadow, N(v_shadow), 1, 1, 0},
    {"RECOMP_HDTEX", "TEXTURES", "The high-resolution texture pack, when one is installed.",
     v_hdtex, n_hdtex, N(v_hdtex), 1, 1, 0},
    {"RECOMP_CRT", "CRT FILTER", "Scanlines and glow, like a television of the time.",
     v_onoff, n_onoff, N(v_onoff), 0, 0, 0},
    {"RECOMP_MBLUR", "MOTION BLUR", "Frame-to-frame smear on the camera's motion.",
     v_onoff, n_onoff, N(v_onoff), 0, 0, 0},
    {"RECOMP_HUD_EDGES", "HUD AT SCREEN EDGES", "Puts the widescreen HUD at the edges instead of the middle.",
     v_onoff, n_onoff, N(v_onoff), 1, 1, SSX3_LIVE},
    {"RECOMP_FPS_OVERLAY", "FRAME COUNTER", "Frames per second, top left.",
     v_onoff, n_onoff, N(v_onoff), 0, 0, SSX3_LIVE},
    {"RECOMP_AUDIO_CHANNELS", "AUDIO", "Stereo, or 5.1 where the audio device takes it.",
     v_audio, n_audio, N(v_audio), 0, 0, 0},
    {"RECOMP_SKIP_INTRO", "SKIP INTRO", "Straight to the front end, past the opening movies.",
     v_onoff, n_onoff, N(v_onoff), 1, 1, 0},
    {"RECOMP_FAST_BOOT", "FAST LOADING", "Shortens the front end's fixed pauses.",
     v_fast, n_fast, N(v_fast), 1, 1, 0},
    {"RECOMP_DEBUG_MENU", "DEBUG MENU", "EA's developer quick-start screen at the title.",
     v_onoff, n_onoff, N(v_onoff), 0, 0, 0},
    {"RECOMP_MONEY_CHEAT", "MONEY CHEAT", "Lots of money in the lodge from the start.",
     v_onoff, n_onoff, N(v_onoff), 0, 0, 0},
    {"RECOMP_LOG_LEVEL", "LOG DETAIL", "What the port writes to logs\\. For bug reports.",
     v_log, n_log, N(v_log), 2, 2, 0},
};

const int ssx3_settings_count = N(ssx3_settings);

/* ---- state ------------------------------------------------------------- */

static int      s_choice[N(ssx3_settings)];
static unsigned char s_from_file[N(ssx3_settings)];
static unsigned s_gen = 1;
static int      s_loaded;
static char     s_path[1024];

static int row_offered(int i)
{
    const unsigned f = ssx3_settings[i].flags;
#if defined(__ANDROID__)
    return !(f & SSX3_WIN_ONLY);
#else
    return !(f & SSX3_AND_ONLY);
#endif
}

int ssx3_SettingsRowCount(void)
{
    int i, n = 0;
    for (i = 0; i < ssx3_settings_count; i++)
        n += row_offered(i);
    return n;
}

int ssx3_SettingsRowAt(int index)
{
    int i, n = 0;
    for (i = 0; i < ssx3_settings_count; i++) {
        if (!row_offered(i))
            continue;
        if (n == index)
            return i;
        n++;
    }
    return 0;
}

static void env_set(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v)
        setenv(k, v, 1);
    else
        unsetenv(k);
#endif
}

const char *ssx3_SettingsFile(void)
{
    if (s_path[0])
        return s_path;
#ifdef _WIN32
    {
        char exe[1024];
        char *slash;
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (n == 0 || n >= sizeof exe)
            return "ssx3.cfg";
        slash = strrchr(exe, '\\');
        if (!slash)
            return "ssx3.cfg";
        *slash = 0;
        snprintf(s_path, sizeof s_path, "%s\\ssx3.cfg", exe);
    }
#else
    {
        char cwd[900];
        if (!getcwd(cwd, sizeof cwd))
            return "ssx3.cfg";
        snprintf(s_path, sizeof s_path, "%s/ssx3.cfg", cwd);
    }
#endif
    return s_path;
}

/* The choice whose env string is `v`, or -1. */
static int choice_of(const Ssx3Setting *s, const char *v)
{
    int i;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->values[i], v))
            return i;
    return -1;
}

static int default_of(const Ssx3Setting *s)
{
#if defined(__ANDROID__)
    return s->def_android >= 0 ? s->def_android : s->def;
#else
    return s->def;
#endif
}

void ssx3_SettingsLoad(void)
{
    FILE *f;
    char line[512];
    int i;

    if (s_loaded)
        return;
    s_loaded = 1;

    for (i = 0; i < ssx3_settings_count; i++) {
        const Ssx3Setting *s = &ssx3_settings[i];
        const char *e = getenv(s->key);
        s_choice[i] = default_of(s);
        if (e && *e) {
            int c = choice_of(s, e);
            if (c >= 0)
                s_choice[i] = c;
        }
    }

    f = fopen(ssx3_SettingsFile(), "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *end;
        int c;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == 0)
            continue;
        if ((eq = strchr(p, '=')) == NULL)
            continue;
        *eq = 0;
        end = eq;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        for (i = 0; i < ssx3_settings_count; i++) {
            if (strcmp(ssx3_settings[i].key, p))
                continue;
            end = eq + 1;
            while (*end == ' ' || *end == '\t')
                end++;
            {
                char *tail = end + strlen(end);
                while (tail > end && (tail[-1] == '\n' || tail[-1] == '\r'
                                      || tail[-1] == ' ' || tail[-1] == '\t'))
                    *--tail = 0;
            }
            c = choice_of(&ssx3_settings[i], end);
            if (c >= 0) {
                s_choice[i] = c;
                s_from_file[i] = 1;
                env_set(ssx3_settings[i].key, ssx3_settings[i].values[c]);
            }
            break;
        }
    }
    fclose(f);
    fprintf(stderr, "[SETTINGS] %s loaded\n", ssx3_SettingsFile());
}

int ssx3_SettingsGet(int i)
{
    if (i < 0 || i >= ssx3_settings_count)
        return 0;
    return s_choice[i];
}

int ssx3_SettingsFromFile(int i)
{
    if (i < 0 || i >= ssx3_settings_count)
        return 0;
    return s_from_file[i];
}

unsigned ssx3_SettingsGeneration(void)
{
    return s_gen;
}

/* What a choice means outside the environment, for the rows where the port
 * caches the value in a place only the presenter can reach. */
static void apply_now(int i, int c)
{
    const Ssx3Setting *s = &ssx3_settings[i];

    if (!strcmp(s->key, "RECOMP_FPS_OVERLAY"))
        xbox_FramebufferSetFpsOverlay(c > 0);
}

void ssx3_SettingsSet(int i, int c)
{
    const Ssx3Setting *s;
    FILE *f;
    int k;

    if (i < 0 || i >= ssx3_settings_count)
        return;
    s = &ssx3_settings[i];
    if (c < 0)
        c = 0;
    if (c >= s->count)
        c = s->count - 1;
    s_choice[i] = c;
    s_from_file[i] = 1;
    env_set(s->key, s->values[c]);
    apply_now(i, c);
    s_gen++;

    f = fopen(ssx3_SettingsFile(), "w");
    if (!f) {
        fprintf(stderr, "[SETTINGS] cannot write %s\n", ssx3_SettingsFile());
        return;
    }
    fprintf(f, "# SSX 3 options, as set in game. One RECOMP_* variable per line;\n"
               "# the game reads this before anything else, so a line here beats a\n"
               "# value from the launcher or the environment.\n");
    for (k = 0; k < ssx3_settings_count; k++) {
        if (!row_offered(k))
            continue;
        if (!s_from_file[k])
            continue;
        fprintf(f, "%s=%s\n", ssx3_settings[k].key,
                ssx3_settings[k].values[s_choice[k]]);
    }
    fclose(f);
}
