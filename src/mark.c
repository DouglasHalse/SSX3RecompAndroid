/*
 * The player's mark (docs/LOGGING.md C1): "I saw it here".
 *
 * F12, or both thumbsticks clicked together on a controller, writes
 *   INFO [MARK] *** player mark N (F12) *** in game, Peak 1 Race Snow Jam; snapshot logs\mark_...png
 * to the session log -- the log pump writes its flight recorder (the detail
 * lines below the log level) just before it -- and saves what the window
 * shows next to the log. A host thread polls every 50 ms, so a mark works
 * while the game itself hangs.
 *
 * The same thread logs the host's input (C4), [INPUT]: controllers as they
 * come and go (the ones plugged in at start first), a battery running low,
 * and the first key pressed in the window (keyboard in use).
 */
#include "log.h"
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#else
#include "platform/win32_compat.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifndef VK_F12
#define VK_F12 0x7B
#endif

extern int xbox_FramebufferKeyPresses(int vk);
extern int xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferSnapshot(const char *path);
extern const char *ssx3_LogsDir(void);                /* main.c */
extern void ssx3_WhereNow(char *out, size_t n);       /* game_state.c */
extern const char *ssx3_ScreenNow(void);              /* ssx3_overrides.c */

extern int ssx3_LiveHdTexturesOn(void);              /* src/vk/vkr_backend.c */
extern int ssx3_LiveShadowsOn(void);
extern int ssx3_RenderInfo(uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes);
extern void ssx3_DrawDist(float *setting, float *in_use);   /* ssx3_overrides.c */
extern float ssx3_PerfLastFps(void);                 /* perf.c */
extern void ssx3_PerfEventTime(char *out, size_t n);
extern int ssx3_PlayerPos(float out[3]);             /* game_state.c */

/* The settings in effect at the mark (F4-F6 may have changed them since
 * the start), what the renderer is doing and where the rider is. */
static void mark_state(int n)
{
    char line[512], ev[96];
    size_t k = 0;
    float dd, dd_now, pos[3];
    uint32_t w = 0, h = 0, msaa = 0;
    uint64_t tex = 0;
#define ADD(...) do { k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, __VA_ARGS__); } while (0)
    ssx3_DrawDist(&dd, &dd_now);
    ADD("HD textures %s, draw distance %.1fx", ssx3_LiveHdTexturesOn() ? "on" : "off", dd);
    if (dd_now < dd - 0.005f || dd_now > dd + 0.005f)
        ADD(" (now %.2fx, adapted)", dd_now);
    ADD(", rider shadows %s", ssx3_LiveShadowsOn() ? "on" : "off");
    if (ssx3_RenderInfo(&w, &h, &msaa, &tex))
        ADD(", MSAA %ux, %ux%u", msaa, w, h);
    ADD(" | %.1f FPS last second", ssx3_PerfLastFps());
    ssx3_PerfEventTime(ev, sizeof ev);
    if (ev[0])
        ADD(" | %s", ev);
    if (ssx3_PlayerPos(pos))
        ADD("%s rider at %.0f, %.0f, %.0f", ev[0] ? "," : " |", pos[0], pos[1], pos[2]);
    if (msaa || tex)
        ADD(" | texture cache %.0f MB", tex / 1048576.0);
#undef ADD
    log_msg(LOG_INFO, "MARK", "mark %d state: %s", n, line);
}

static void mark(const char *how)
{
    static int n;
    char where[128], path[512];
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
    CreateDirectoryA(ssx3_LogsDir(), NULL);
#else
    localtime_r(&now, &tm);
    mkdir(ssx3_LogsDir(), 0755);
#endif
    n++;
    /* PNG on Windows (encoded on a thread of the presenter's own), BMP
     * elsewhere */
    snprintf(path, sizeof path, "%s%cmark_%04d%02d%02d-%02d%02d%02d_%d.%s", ssx3_LogsDir(),
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, n,
#ifdef _WIN32
             "png");
#else
             "bmp");
#endif
    ssx3_WhereNow(where, sizeof where);
    xbox_FramebufferSnapshot(path);
    {
        const char *scr = ssx3_ScreenNow();
        int same = !strcmp(scr, "in game");     /* WhereNow says that already */
        log_msg(LOG_INFO, "MARK", "*** player mark %d (%s) *** %s%s%s; snapshot %s", n, how, where,
                same ? "" : "; ", same ? "" : scr, path);
    }
    mark_state(n);
}

#ifdef _WIN32
/* controller i's battery, logged when it changes to low or empty (wired
 * pads report none) */
static void battery_check(DWORD i, int *last)
{
    XINPUT_BATTERY_INFORMATION b;
    int lv;
    if (XInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &b) != ERROR_SUCCESS
            || b.BatteryType == BATTERY_TYPE_WIRED || b.BatteryType == BATTERY_TYPE_DISCONNECTED)
        return;
    lv = b.BatteryLevel;
    if (lv != *last && *last >= 0 && lv <= BATTERY_LEVEL_LOW)
        log_msg(LOG_WARN, "INPUT", "controller %lu: battery %s", (unsigned long)i + 1,
                lv == BATTERY_LEVEL_EMPTY ? "empty" : "low");
    *last = lv;
}

static const char *battery_words(DWORD i)
{
    XINPUT_BATTERY_INFORMATION b;
    if (XInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &b) != ERROR_SUCCESS)
        return "";
    if (b.BatteryType == BATTERY_TYPE_WIRED)
        return ", wired";
    switch (b.BatteryLevel) {
    case BATTERY_LEVEL_EMPTY:  return ", battery empty";
    case BATTERY_LEVEL_LOW:    return ", battery low";
    case BATTERY_LEVEL_MEDIUM: return ", battery medium";
    default:                   return ", battery full";
    }
}
#endif

static DWORD WINAPI mark_thread(LPVOID unused)
{
    int pads_held = 0, keyboard_seen = 0;
    unsigned tick = 0;
#ifdef _WIN32
    const char *nh = getenv("RECOMP_NO_HOST_PAD");
    int pads = !(nh && *nh && *nh != '0'), connected[4] = {-1, -1, -1, -1},
        battery[4] = {-1, -1, -1, -1};
    if (!pads)
        log_msg(LOG_INFO, "INPUT", "the machine's controllers are ignored (RECOMP_NO_HOST_PAD)");
#endif
    (void)unused;
    for (;;) {
        int k = xbox_FramebufferKeyPresses(VK_F12), held = 0;
        while (k-- > 0)
            mark("F12");
        {
            extern void ssx3_LiveKeysPoll(void);    /* live.c: F4 / F5 / F6 */
            ssx3_LiveKeysPoll();
        }
        if (!keyboard_seen) {
            int vk;
            for (vk = 8; vk < 256 && !xbox_FramebufferKeyDown(vk); vk++)
                ;
            if (vk < 256) {
                keyboard_seen = 1;
                log_msg(LOG_INFO, "INPUT", "keyboard in use (first key 0x%02X)", vk);
            }
        }
#ifdef _WIN32
        if (pads) {
            DWORD i;
            for (i = 0; i < 4; i++) {
                XINPUT_STATE st;
                int on = XInputGetState(i, &st) == ERROR_SUCCESS;
                if (on != connected[i]) {
                    if (on) {
                        XINPUT_CAPABILITIES caps;
                        int sub = XInputGetCapabilities(i, 0, &caps) == ERROR_SUCCESS ? caps.SubType : -1;
                        log_msg(LOG_INFO, "INPUT", "controller %lu connected%s (%s%s)", (unsigned long)i + 1,
                                connected[i] < 0 ? " at start" : "",
                                sub == XINPUT_DEVSUBTYPE_GAMEPAD ? "gamepad" : sub < 0 ? "unknown type"
                                    : "not a gamepad", battery_words(i));
                    } else if (connected[i] > 0) {
                        log_msg(LOG_WARN, "INPUT", "controller %lu disconnected", (unsigned long)i + 1);
                    }
                    connected[i] = on;
                    battery[i] = -1;
                }
                if (on && tick % 200 == 0)        /* every 10 s */
                    battery_check(i, &battery[i]);
                if (on && (st.Gamepad.wButtons & (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB))
                              == (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB))
                    held = 1;
            }
            if (tick == 0 && !connected[0] && !connected[1] && !connected[2] && !connected[3])
                log_msg(LOG_INFO, "INPUT", "no controller connected at start");
        }
#endif
        tick++;
        if (held && !pads_held)
            mark("both sticks clicked");
        pads_held = held;
        Sleep(50);
    }
    return 0;
}

void ssx3_MarkStart(void)
{
    const char *e = getenv("RECOMP_MARK");
    if (e && e[0] == '0')
        return;
    CloseHandle(CreateThread(NULL, 0, mark_thread, NULL, 0, NULL));
}
