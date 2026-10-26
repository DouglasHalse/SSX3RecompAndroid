/* Levelled logging: see log.h and docs/LOGGING.md. */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

static int s_level = -1;
static int s_pump;                   /* a pump filters: send down to DEBUG */

static const char *const k_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRIT"};

const char *log_level_name(int level)
{
    return level >= LOG_TRACE && level <= LOG_CRITICAL ? k_names[level] : "?";
}

int log_parse_level(const char *s, int dflt)
{
    static const struct { const char *name; int level; } k[] = {
        {"trace", LOG_TRACE}, {"everything", LOG_TRACE}, {"debug", LOG_DEBUG},
        {"detailed", LOG_DEBUG}, {"info", LOG_INFO}, {"normal", LOG_INFO},
        {"warn", LOG_WARN}, {"warning", LOG_WARN}, {"warnings", LOG_WARN},
        {"error", LOG_ERROR}, {"errors", LOG_ERROR}, {"critical", LOG_CRITICAL},
        {"crit", LOG_CRITICAL},
    };
    char low[16];
    unsigned i;
    if (!s || !*s)
        return dflt;
    if (s[0] >= '0' && s[0] <= '5' && !s[1])
        return s[0] - '0';
    for (i = 0; i + 1 < sizeof low && s[i]; i++)
        low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(low, k[i].name))
            return k[i].level;
    return dflt;
}

/* a RECOMP_*_TRACE / *_STATS / *_DIAG diagnostic switched on: its lines are
 * DEBUG, so without RECOMP_LOG_LEVEL the default becomes debug (asking for
 * a trace and not seeing it helps no one) */
static int name_has(const char *var, const char *eq, const char *word)
{
    const char *p = strstr(var, word);
    return p && p < eq;
}

static int trace_asked(void)
{
#ifdef _WIN32
    char **e = _environ;
#else
    extern char **environ;
    char **e = environ;
#endif
    for (; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (strncmp(*e, "RECOMP_", 7) || !eq || !eq[1] || (eq[1] == '0' && !eq[2]))
            continue;
        if (name_has(*e, eq, "_TRACE") || name_has(*e, eq, "_STATS") || name_has(*e, eq, "_DIAG"))
            return 1;
    }
    return 0;
}

int log_level(void)
{
    if (s_level < 0)
        s_level = log_parse_level(getenv("RECOMP_LOG_LEVEL"), trace_asked() ? LOG_DEBUG : LOG_INFO);
    return s_level;
}

int log_enabled(int level)
{
    return level >= log_level();
}

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

double log_uptime(void)
{
    static double t0 = -1.0;
    double t = now_s();
    if (t0 < 0.0)
        t0 = t;
    return t - t0;
}

void log_stamp(char *out, unsigned n)
{
    int h, m, s, ms;
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    h = st.wHour; m = st.wMinute; s = st.wSecond; ms = st.wMilliseconds;
#else
    struct timeval tv;
    struct tm tmv;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tmv);
    h = tmv.tm_hour; m = tmv.tm_min; s = tmv.tm_sec; ms = (int)(tv.tv_usec / 1000);
#endif
    snprintf(out, n, "%02d:%02d:%02d.%03d +%6.1f ", h, m, s, ms, log_uptime());
}

void log_msg(int level, const char *cat, const char *fmt, ...)
{
    char line[2048], stamp[40];
    va_list ap;
    int n;
    /* [SESSION] (the build at the start, the summary at the end) is written
     * at any level: even an "errors only" log says what ran and how it ended */
    if (level < log_level() && !(s_pump && level >= LOG_DEBUG) && !(cat && !strcmp(cat, "SESSION")))
        return;
    log_stamp(stamp, sizeof stamp);
    n = snprintf(line, sizeof line, "%s%-5s [%s] ", stamp, log_level_name(level), cat ? cat : "-");
    if (n < 0 || n >= (int)sizeof line)
        return;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    n = (int)strlen(line);
    if (n && line[n - 1] != '\n' && n + 1 < (int)sizeof line) {
        line[n++] = '\n';
        line[n] = 0;
    }
    fputs(line, stderr);             /* one call: lines from threads stay whole */
    if (level >= LOG_ERROR)
        fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * The log pump (RECOMP_LOG_ASYNC=0 off). When stderr is a file (the
 * launcher's logs\, play.bat and the scripts), every fprintf was a write on
 * the thread that printed -- the GPU executor, the game's main thread or a
 * guest thread holding the guest CPU -- and such a write now and then
 * blocks for 100 ms and more: a dropped frame for a log line. stderr writes
 * into a 4 MB pipe (a memory copy) and one thread moves it into the file.
 *
 * On the way it grades every line (docs/LOGGING.md): lines from log_msg
 * carry their level after the timestamp; the older "[TAG] ..." lines get
 * one from k_tags (first match on the line's start, leading blanks
 * skipped); indented untagged lines -- a crash's stack, a block's details --
 * the level of the line before; anything else DEBUG. Lines below
 * RECOMP_LOG_LEVEL go to a ring instead of the file (the flight recorder),
 * written out ahead of the next ERROR or CRITICAL line. The same message
 * again is counted and written once as "(repeated N more times)".
 * log_drain waits until the file has everything: the crash handler and the
 * toolkit's exits (xbox_before_exit) call it. */
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
static HANDLE s_pump_rd, s_pump_out;
static volatile LONG s_pump_waiting, s_pump_reads;
#else
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
static int s_pump_rd = -1, s_pump_out = -1;
static volatile int s_pump_waiting, s_pump_reads;
#endif

static const struct { const char *tag; int level; } k_tags[] = {
    /* C9: the always-on diagnostics; opt-in traces stay DEBUG (the default
     * for any [TAG] line) */
    {"[NV2A-WARN]", LOG_WARN}, {"[APU-WARN]", LOG_WARN}, {"[ABI]", LOG_WARN},
    {"[ALLOC] FAILED", LOG_ERROR}, {"[CONTIG] arena exhausted", LOG_ERROR},
    {"[IRQLBUG]", LOG_WARN}, {"[GPU] REFUSING", LOG_WARN},
    {"[BOOT] hardware TSO unavailable", LOG_WARN}, {"[BOOT] flat dispatch unavailable", LOG_WARN},
    {"[BOOT] widescreen", LOG_INFO}, {"[BOOT]", LOG_DEBUG},
    {"[VIDEO] File not found", LOG_ERROR}, {"[VIDEO] could not open", LOG_ERROR},
    {"[VIDEO] Failed", LOG_ERROR}, {"[VIDEO] No video stream", LOG_ERROR},
    {"[VIDEO] CreateDevice failed", LOG_ERROR}, {"[VIDEO] Direct3DCreate8 failed", LOG_ERROR},
    {"[VIDEO] MFStartup failed", LOG_ERROR}, {"[VIDEO] window creation failed", LOG_ERROR},
    {"[RECORD]", LOG_INFO}, {"[DEBUGMENU]", LOG_INFO},
    {"[CRASH]", LOG_CRITICAL}, {"[EXIT]", LOG_INFO}, {"[FBWIN] window closed", LOG_INFO},
    {"[FBWIN] fullscreen", LOG_INFO}, {"[FBWIN] focus", LOG_INFO},
    {"[VKR] internal resolution", LOG_INFO},
    {"[XA2] XAudio2 initialized", LOG_INFO}, {"[XA2] output device", LOG_INFO},
    {"[XA2] audio output:", LOG_WARN}, {"[XA2] SDL audio initialized", LOG_INFO},
    {"[XA2] web audio", LOG_INFO}, {"[XA2] Shut down", LOG_DEBUG},
    {"[XA2]", LOG_ERROR},               /* the rest: failures */
    {"[APU] XAudio2 unavailable", LOG_WARN}, {"[APU] waveOut audio output initialized", LOG_INFO},
    {"[APU] waveOutOpen failed", LOG_ERROR},
    {"[VKR] device:", LOG_INFO}, {"[VKR] MSAA", LOG_INFO}, {"[VKR] window", LOG_INFO},
    {"[ICALL] Failed", LOG_ERROR}, {"[VKR] upload buffer full", LOG_ERROR},
    {"[VKR] pipeline creation failed", LOG_ERROR}, {"[VKR] init failed", LOG_CRITICAL},
    {"[VKR] shader module failed", LOG_ERROR}, {"[VKR] CRT shader module failed", LOG_ERROR},
    {"[VKR] scene shader modules failed", LOG_ERROR}, {"[VKR] no host-visible memory", LOG_ERROR},
    {"[VKR] pipeline cache full", LOG_WARN}, {"[VKR] this GPU cannot sample BC7", LOG_WARN},
    {"[VKR] no depth-stencil format", LOG_WARN}, {"[VKR] CRT needs", LOG_WARN},
    {"[HDTEX] cannot open", LOG_WARN}, {"[HDTEX] no HD texture pack", LOG_INFO},
    {"[HDTEX] dumping", LOG_INFO},
    {"[GATHER]", LOG_WARN}, {"[BLOCK-STUCK]", LOG_WARN}, {"[DRAWDIST]", LOG_DEBUG}, {"[BLOCK-RESCUE]", LOG_DEBUG},  /* a lost GPU wake recovered: routine */
    {"[STATE]", LOG_DEBUG},
    {"[KERNEL]", LOG_TRACE}, {"[CS]", LOG_TRACE}, {"[PATH]", LOG_TRACE}, {"[FILE]", LOG_TRACE},
    {"[HEAP]", LOG_TRACE}, {"[BRIDGE]", LOG_TRACE}, {"[OHCI", LOG_TRACE}, {"[USB", LOG_TRACE},
    {"[DPC]", LOG_TRACE}, {"[D3D] frame size", LOG_TRACE},
};

#define LOG_RING 2000
static char *s_ring[LOG_RING];
static int s_ring_head, s_ring_n;
static int s_prev_level = LOG_DEBUG;
static char s_partial[65536];        /* a line not yet ended (pump thread) */
static size_t s_have;

static void out(const char *p, size_t n)
{
#ifdef _WIN32
    DWORD w;
    WriteFile(s_pump_out, p, (DWORD)n, &w, NULL);
#else
    while (n) {
        ssize_t w = write(s_pump_out, p, n);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
#endif
}

#ifdef _WIN32
#define EOL "\r\n"
#else
#define EOL "\n"
#endif

static void ring_flush(void)
{
    static const char hd[] = "----- flight recorder: the lines below the log level before this -----" EOL;
    static const char tl[] = "----- end of the flight recorder -----" EOL;
    int i;
    if (!s_ring_n)
        return;
    out(hd, sizeof hd - 1);
    for (i = 0; i < s_ring_n; i++) {
        char *l = s_ring[(s_ring_head + LOG_RING - s_ring_n + i) % LOG_RING];
        out(l, strlen(l));
        out(EOL, sizeof EOL - 1);
    }
    out(tl, sizeof tl - 1);
    for (i = 0; i < LOG_RING; i++) { free(s_ring[i]); s_ring[i] = NULL; }
    s_ring_head = s_ring_n = 0;
}

static int grade(const char *l, size_t n, int *stamped)
{
    size_t i = 0, k;
    int lv;
    *stamped = n > 20 && l[2] == ':' && l[5] == ':' && l[8] == '.' && l[12] == ' ' && l[13] == '+';
    if (*stamped) {
        const char *w = l + 14, *e = l + n;  /* after '+': the uptime, then the level */
        while (w < e && *w == ' ') w++;
        while (w < e && ((*w >= '0' && *w <= '9') || *w == '.')) w++;
        while (w < e && *w == ' ') w++;
        for (lv = LOG_TRACE; lv <= LOG_CRITICAL; lv++) {
            const char *nm = log_level_name(lv);
            if ((size_t)(e - w) >= strlen(nm) && !strncmp(w, nm, strlen(nm)))
                return lv;
        }
        return LOG_INFO;
    }
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    for (k = 0; k < sizeof k_tags / sizeof k_tags[0]; k++) {
        size_t tl = strlen(k_tags[k].tag);
        if (n - i >= tl && !memcmp(l + i, k_tags[k].tag, tl))
            return k_tags[k].level;
    }
    if (i < n && l[i] == '[')
        return LOG_DEBUG;
    return i > 0 ? s_prev_level : LOG_DEBUG;
}

/* for the session summary (log_session_counts) */
static unsigned s_counts[LOG_CRITICAL + 1];
static char s_exit_how[96];

static void note_exit(const char *l, size_t n)
{
    const char *e;
    char line[512];
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, l, n);
    line[n] = 0;
    if ((e = strstr(line, "[EXIT] ")) != NULL)
        snprintf(s_exit_how, sizeof s_exit_how, "the game ended itself (%s)", e + 7);
    else if (strstr(line, "[FBWIN] window closed"))
        snprintf(s_exit_how, sizeof s_exit_how, "window closed");
    else if (strstr(line, "[CRASH]"))
        snprintf(s_exit_how, sizeof s_exit_how, "crash");
}

/* the last [PLAY] lines (what the player did), repeated in a crash report */
#define RECENT_PLAY 8
static char s_recent[RECENT_PLAY][200];
static unsigned s_recent_n;

static void note_play(const char *l, size_t n)
{
    const char *p = memchr(l, '[', n < 40 ? n : 40);
    size_t k;
    if (!p || n - (size_t)(p - l) < 7 || memcmp(p, "[PLAY]", 6))
        return;
    p += 7;
    k = n - (size_t)(p - l);
    if (k >= sizeof s_recent[0])
        k = sizeof s_recent[0] - 1;
    memcpy(s_recent[s_recent_n % RECENT_PLAY], l, 13);      /* the clock time */
    memcpy(s_recent[s_recent_n % RECENT_PLAY] + 13, p, k);
    s_recent[s_recent_n % RECENT_PLAY][13 + k < sizeof s_recent[0] ? 13 + k : sizeof s_recent[0] - 1] = 0;
    s_recent_n++;
}

void log_recent_play(void)
{
    unsigned i = s_recent_n > RECENT_PLAY ? s_recent_n - RECENT_PLAY : 0;
    for (; i < s_recent_n; i++)
        fprintf(stderr, "  recent: %s\n", s_recent[i % RECENT_PLAY]);
}

void log_session_counts(unsigned *warn, unsigned *err, unsigned *crit, const char **how)
{
    *warn = s_counts[LOG_WARN];
    *err = s_counts[LOG_ERROR];
    *crit = s_counts[LOG_CRITICAL];
    *how = s_exit_how[0] ? s_exit_how : "unknown";
}

static int memmem_s(const char *h, size_t hn, const char *needle)
{
    size_t nn = strlen(needle), i;
    for (i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, needle, nn))
            return 1;
    return 0;
}

static void pump_line(const char *l, size_t n)
{
    static char last[512];
    static size_t last_n;
    static unsigned repeats;
    static int last_lv;
    int stamped, lv;
    char stamp[48], *copy;
    size_t sl = 0;
    const char *msg;
    size_t mn;
    while (n && (l[n - 1] == '\r' || l[n - 1] == '\n')) n--;
    lv = grade(l, n, &stamped);
    if (n && l[0] != ' ' && l[0] != '\t')     /* continuation lines are part of one */
        s_counts[lv]++;
    if (!stamped && lv >= LOG_INFO)
        note_exit(l, n);
    if (stamped && lv == LOG_INFO && n > 40)
        note_play(l, n);
    s_prev_level = lv;
    if (!stamped) {
        log_stamp(stamp, sizeof stamp);
        sl = strlen(stamp);
        snprintf(stamp + sl, sizeof stamp - sl, "%-5s ", log_level_name(lv));
        sl = strlen(stamp);
    }
    if (lv < log_level() && !(stamped && n > 50 && memmem_s(l + 20, 30, "[SESSION]"))) {
        if (!(copy = (char *)malloc(sl + n + 1)))
            return;
        memcpy(copy, stamp, sl);
        memcpy(copy + sl, l, n);
        copy[sl + n] = 0;
        free(s_ring[s_ring_head]);
        s_ring[s_ring_head] = copy;
        s_ring_head = (s_ring_head + 1) % LOG_RING;
        if (s_ring_n < LOG_RING) s_ring_n++;
        return;
    }
    /* the same message again (the timestamp aside): counted ("upload buffer
     * full" ran 900 times in a race load) */
    msg = l;
    mn = n;
    if (stamped) {
        const char *b = memchr(l + 14, ' ', n - 14);
        while (b && b < l + n && *b == ' ') b++;
        while (b && b < l + n && ((*b >= '0' && *b <= '9') || *b == '.')) b++;
        if (b) { msg = b; mn = n - (size_t)(b - l); }
    }
    if (mn && mn == last_n && !memcmp(msg, last, mn)) {
        repeats++;
        return;
    }
    if (repeats) {
        char rep[96];
        int rn;
        log_stamp(rep, sizeof rep);
        rn = (int)strlen(rep);
        rn += snprintf(rep + rn, sizeof rep - (size_t)rn, "%-5s (repeated %u more times)" EOL,
                       log_level_name(last_lv), repeats);
        out(rep, (size_t)rn);
        repeats = 0;
    }
    last_n = mn <= sizeof last ? mn : 0;
    last_lv = lv;
    if (last_n) memcpy(last, msg, mn);
    /* an error, or the player's mark (mark.c): the detail before it */
    if (lv >= LOG_ERROR || (stamped && n > 50 && memmem_s(l + 20, 30, "[MARK]")))
        ring_flush();
    if (sl) out(stamp, sl);
    out(l, n);
    out(EOL, sizeof EOL - 1);
}

static void pump_feed(const char *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n' || s_have == sizeof s_partial) {
            pump_line(s_partial, s_have);
            s_have = 0;
            if (buf[i] == '\n')
                continue;
        }
        s_partial[s_have++] = buf[i];
    }
}

#ifdef _WIN32
static DWORD WINAPI pump_thread(LPVOID unused)
{
    static char buf[65536];
    DWORD n;
    (void)unused;
    for (;;) {
        InterlockedExchange(&s_pump_waiting, 1);
        if (!ReadFile(s_pump_rd, buf, sizeof buf, &n, NULL) || !n)
            return 0;
        InterlockedExchange(&s_pump_waiting, 0);
        pump_feed(buf, n);
        InterlockedIncrement(&s_pump_reads);
    }
}
#else
static void *pump_thread(void *unused)
{
    static char buf[65536];
    (void)unused;
    for (;;) {
        ssize_t n;
        __atomic_store_n(&s_pump_waiting, 1, __ATOMIC_SEQ_CST);
        n = read(s_pump_rd, buf, sizeof buf);
        if (n <= 0)
            return NULL;
        __atomic_store_n(&s_pump_waiting, 0, __ATOMIC_SEQ_CST);
        pump_feed(buf, (size_t)n);
        __atomic_add_fetch(&s_pump_reads, 1, __ATOMIC_SEQ_CST);
    }
}
#endif

/* Everything written so far is in the file once the pump has sat in its
 * read (pipe empty) for 20 ms. Not PeekNamedPipe on Windows: on this
 * synchronous handle it waits behind the pump's pending ReadFile -- forever
 * when nothing more is written. */
void log_drain(void)
{
    int i;
    int last = -1;
#ifdef _WIN32
    if (!s_pump_rd)
        return;
#else
    if (s_pump_rd < 0)
        return;
#endif
    fflush(stderr);
    for (i = 0; i < 100; i++) {         /* <= 2 s */
        int r = (int)s_pump_reads;
        if (r == last && s_pump_waiting) {
            if (s_have) {               /* the pump sits in its read: safe */
                pump_line(s_partial, s_have);
                s_have = 0;
            }
            return;
        }
        last = r;
#ifdef _WIN32
        Sleep(20);
#else
        usleep(20000);
#endif
    }
}

int log_pump_start(void)
{
    const char *e = getenv("RECOMP_LOG_ASYNC");
    if (e && e[0] == '0')
        return 0;
#ifdef _WIN32
    {
        /* the C runtime's stderr, not GetStdHandle: started from the
         * launcher (a GUI program, no console) the session file is opened
         * with freopen and the process's standard handle stays empty -- the
         * pump never ran in played sessions, only in scripted ones */
        HANDLE outh = (HANDLE)_get_osfhandle(_fileno(stderr)), dup, rd, wr, th;
        int fd;
        if (!outh || outh == INVALID_HANDLE_VALUE || GetFileType(outh) != FILE_TYPE_DISK)
            return 0;
        /* _dup2 below closes the fd's old handle: keep a copy for the pump. */
        if (!DuplicateHandle(GetCurrentProcess(), outh, GetCurrentProcess(), &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return 0;
        if (!CreatePipe(&rd, &wr, NULL, 4u << 20)) {
            CloseHandle(dup);
            return 0;
        }
        fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
        if (fd < 0) {
            CloseHandle(rd); CloseHandle(wr); CloseHandle(dup);
            return 0;
        }
        fflush(stderr);
        s_pump_rd = rd;
        s_pump_out = dup;
        th = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
        if (!th) {
            s_pump_rd = NULL; s_pump_out = NULL;
            _close(fd); CloseHandle(rd); CloseHandle(dup);
            return 0;
        }
        SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(th);
        _dup2(fd, _fileno(stderr));
        _close(fd);
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
        setvbuf(stderr, NULL, _IONBF, 0);
    }
#else
    {
        int p[2], dupfd;
        pthread_t th;
        /* Not isatty(): the generated guest code defines the Xbox CRT's
         * isatty (0x0021A60D) as a global symbol, which shadows libc's inside
         * this library -- calling it ran Xbox bytecode, from the log thread,
         * and faulted. tcgetpgrp answers the same question and nothing in the
         * guest image is called that. */
        if (tcgetpgrp(2) != -1)
            return 0;                   /* a terminal: everything, as it comes */
        if ((dupfd = dup(2)) < 0)
            return 0;
        if (pipe(p) < 0) {
            close(dupfd);
            return 0;
        }
#ifdef F_SETPIPE_SZ
        fcntl(p[1], F_SETPIPE_SZ, 1 << 20);
#endif
        fflush(stderr);
        s_pump_rd = p[0];
        s_pump_out = dupfd;
        if (pthread_create(&th, NULL, pump_thread, NULL)) {
            s_pump_rd = -1; s_pump_out = -1;
            close(p[0]); close(p[1]); close(dupfd);
            return 0;
        }
        pthread_detach(th);
        dup2(p[1], 2);
        close(p[1]);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
#endif
    s_pump = 1;
    return 1;
}
