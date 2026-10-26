/*
 * Game state from the game itself, for the test scripts.
 *
 * SSX 3's app object ([0x003A930C] = 0x00369550) runs a state machine
 * (sub_001AD880): +4 points at the current state object, +8 at a pending next
 * state (0 when none), +0x5C is a phase. A state object's first dword is its
 * vtable, which says what kind of screen it is -- so a screen is identified
 * by that vtable, not by guessing from draw counts.
 *
 * A host thread samples the fields every 100 ms (reads only; a torn read is
 * corrected on the next sample) and
 *   - logs every change to stderr:  [STATE] t=12345 vt=002DE1A0 race next=0 phase=0
 *   - keeps game_state.txt in the working directory current, one line:
 *         <vtable hex> <name> <ms since process start> <seconds in this state>
 *         <vtable hex> <name> <ms since start> <seconds in state> <sub>
 *     which tools\*.ps1 read (tools\game_state.ps1). <sub> is the dword at
 *     state+0xA8: inside "ingame" it changes with the phase (5 at the
 *     briefing, then 8 / 0xC during the race; meaning still being mapped).
 *     <draws> is the GPU executor's total draw count: a frame that never
 *     ends shows as a count that stops moving (tools\to_race.ps1 [freeze]).
 * Names come from the table below; an unknown vtable is reported as "?".
 * RECOMP_GAME_STATE=0 disables it.
 */
#include "log.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include "recomp/gen/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define APP_PTR_VA 0x003A930Cu
#define SSX3_RIDERS 8               /* rider slots looked at (a race has 6) */

static const struct { uint32_t vt; const char *name; } k_states[] = {
    /* Learned by watching a run through every screen (tools\to_race.ps1
     * prints the state for each new screen; CLAUDE.md has the table). */
    { 0x0029AD1Cu, "boot" },
    { 0x0029ACD8u, "intro" },        /* EA / intro movies */
    { 0x00289618u, "frontend" },     /* every menu, title to rules */
    { 0x0029AD48u, "event_enter" },  /* brief transition into an event */
    { 0x0029B0DCu, "loading" },      /* race loading screens */
    { 0x00299D7Cu, "ingame" },       /* briefing, race, results, replay */
    { 0x0028825Cu, "debugmenu" },    /* EA's quick-start menu (RECOMP_DEBUG_MENU) */
    { 0x0029B0F8u, "to_frontend" },  /* leaving an event for the menus / a lodge */
    { 0x0029AD64u, "event_exit" },   /* after Quit -> Yes in the pause menu */
    { 0, NULL }
};

/* B5: the event being played. The game keeps its number at 0x346440 (set
 * when an event is chosen; the word at 0x346448 holds its kind): 0-4 the
 * races, 5-7 slopestyle, 8-10 big air, 11-13 super pipe (one per peak),
 * 14-16 the peak runs (Happiness, Ruthless, The Throne). Found by dumping
 * .data at each event's briefing (docs/LOGGING.md); names from
 * data/be/rwrdxbx.dat and the briefing panels. */
#define EVENT_ID_VA   0x00346440u
static const struct { int peak; const char *kind, *name; } k_events[17] = {
    {1, "Race", "Snow Jam"}, {1, "Race", "Metro City"},
    {2, "Race", "Ruthless Ridge"}, {2, "Race", "Intimidator"}, {3, "Race", "Gravitude"},
    {1, "Slopestyle", "R&B"}, {2, "Slopestyle", "The Style Mile"}, {3, "Slopestyle", "Kick Doubt"},
    {1, "Big Air", "Crows Nest"}, {2, "Big Air", "Launch Time"}, {3, "Big Air", "Much 2 Much"},
    {1, "Super Pipe", "The Junction"}, {2, "Super Pipe", "Schizophrenia"},
    {3, "Super Pipe", "Perpendiculous"},
    {1, "Peak run", "Happiness"}, {2, "Peak run", "Ruthless"}, {3, "Peak run", "The Throne"},
};

/* entering an event -> its briefing screen: the load time (C7) */
static volatile DWORD s_enter_ms;

void ssx3_EventReady(void)
{
    DWORD t = s_enter_ms;
    if (t) {
        s_enter_ms = 0;
        log_msg(LOG_INFO, "PLAY", "event loaded in %.1f s", (GetTickCount() - t) / 1000.0);
    }
}

/* set by the event results screen (ssx3_overrides.c event_result_note) */
volatile int g_ssx3_event_result;
/* for the session summary (journal_session_end) */
volatile int g_ssx3_events_started, g_ssx3_events_finished;
static uint32_t s_left_id = 0xFFFFFFFFu;   /* an event left without a result */

/* leaving in game: an event without a result was quit or restarted */
static void event_left(void)
{
    uint32_t id = MEM32(EVENT_ID_VA);
    s_left_id = 0xFFFFFFFFu;
    if (id < 17 && !g_ssx3_event_result) {
        log_msg(LOG_INFO, "PLAY", "left %s without a result", k_events[id].name);
        s_left_id = id;
    }
}

static void event_note(void)
{
    uint32_t id = MEM32(EVENT_ID_VA);
    int again = id == s_left_id;
    g_ssx3_event_result = 0;
    s_left_id = 0xFFFFFFFFu;
    if (id < 17)
        g_ssx3_events_started++;
    {
        extern void ssx3_PerfEvent(const char *name);
        ssx3_PerfEvent(id < 17 ? k_events[id].name : "free ride");
    }
    if (again)
        log_msg(LOG_INFO, "PLAY", "event restarted: %s", k_events[id].name);
    else if (id < 17)
        log_msg(LOG_INFO, "PLAY", "event: Peak %d, %s, %s", k_events[id].peak, k_events[id].kind,
                k_events[id].name);
    else if (id == 17)
        log_msg(LOG_INFO, "PLAY", "free ride (no event)");
    else
        log_msg(LOG_INFO, "PLAY", "in game without a known event (number %u)", id);
}

/* The session story's words for them (journal, INFO [PLAY]). */
static const char *state_words(uint32_t vt)
{
    switch (vt) {
    case 0x0029AD1Cu: return "starting up";
    case 0x0029ACD8u: return "intro movies";
    case 0x00289618u: return "menus";
    case 0x0029AD48u: return "entering an event";
    case 0x0029B0DCu: return "loading";
    case 0x00299D7Cu: return "in game";
    case 0x0028825Cu: return "debug quick-start menu";
    case 0x0029B0F8u: return "leaving the event";
    case 0x0029AD64u: return "quitting the event";
    case 0:           return "nothing yet";
    default:          return "an unknown state";
    }
}

static const char *state_name(uint32_t vt)
{
    int i;
    for (i = 0; k_states[i].name; i++)
        if (k_states[i].vt == vt)
            return k_states[i].name;
    return "?";
}

static int guest_ok(uint32_t va)
{
    /* Guest heap and image: anything below 64 MB, or the contiguous window. */
    return va >= 0x10000u && (va < 0x04000000u || (va >= 0x80000000u && va < 0x84000000u));
}

/* RECOMP_FE_PROBE=1: find the frontend's "current screen" field. While the
 * state is frontend, the first 0x1000 bytes of the state object are compared
 * with the previous sample (every 300 ms) and each dword that changed between
 * two small values (< 0x400) is logged as [FEPROBE] +off old->new, next to
 * the pad layer's own "PAD: live" press lines -- the field that moves on
 * every screen change and nowhere else is the page. */
#define FE_PROBE_BYTES 0x1000
static void fe_probe(uint32_t cur, uint32_t vt, DWORD t)
{
    static int on = -1;
    static uint32_t prev[FE_PROBE_BYTES / 4], prev_cur;
    uint32_t i;
    if (on < 0) {
        const char *e = getenv("RECOMP_FE_PROBE");
        on = e && e[0] == '1';
    }
    if (!on || vt != 0x00289618u || !guest_ok(cur) || !guest_ok(cur + FE_PROBE_BYTES))
        return;
    static uint32_t prev_vt[FE_PROBE_BYTES / 4];
    for (i = 0; i < FE_PROBE_BYTES / 4; i++) {
        uint32_t v = MEM32(cur + i * 4), pvt = 0;
        if (cur == prev_cur && v != prev[i] && v < 0x400u && prev[i] < 0x400u) {
            fprintf(stderr, "[FEPROBE] t=%lu +%03X %u->%u\n", (unsigned long)t, i * 4, prev[i], v);
        }
        /* Pointer fields: an object whose first dword looks like a vtable
         * (inside the image's data, 0x00280000-0x00340000). A screen object
         * swapped for another shows as its vtable changing. */
        if (guest_ok(v) && v >= 0x80000000u) {
            uint32_t w = MEM32(v);
            if (w >= 0x00280000u && w < 0x00340000u)
                pvt = w;
        }
        if (cur == prev_cur && pvt != prev_vt[i] && (pvt || prev_vt[i]))
            fprintf(stderr, "[FEPROBE] t=%lu +%03X -> obj vt %08X->%08X\n", (unsigned long)t, i * 4,
                    prev_vt[i], pvt);
        prev_vt[i] = pvt;
        prev[i] = v;
    }
    prev_cur = cur;
    fflush(stderr);
}

static DWORD WINAPI game_state_thread(LPVOID unused)
{
    uint32_t last_vt = 0xFFFFFFFFu, last_next = 0xFFFFFFFFu, last_sub = 0xFFFFFFFFu;
    DWORD t0 = GetTickCount(), since = t0;
    char line[160];
    (void)unused;

    for (;;) {
        uint32_t app, cur = 0, next = 0, phase = 0, vt = 0, sub = 0;
        DWORD now;

        Sleep(100);
        now = GetTickCount();
        app = MEM32(APP_PTR_VA);
        if (guest_ok(app)) {
            cur   = MEM32(app + 4);
            next  = MEM32(app + 8);
            phase = MEM32(app + 0x5C);
            if (guest_ok(cur)) {
                vt = MEM32(cur);
                sub = MEM32(cur + 0xA8);   /* in-game sub-state, see below */
            }
        }
        {
            static DWORD last_probe;
            if (now - last_probe >= 300) {
                last_probe = now;
                fe_probe(cur, vt, now - t0);
            }
        }
        if (vt != last_vt && vt) {         /* 0: no app object yet */
            static DWORD vt_since;
            static uint32_t vt_prev;
            if (vt_since)
                log_msg(LOG_INFO, "PLAY", "%s (after %.1f s in %s)", state_words(vt),
                        (now - vt_since) / 1000.0, state_words(vt_prev));
            else
                log_msg(LOG_INFO, "PLAY", "%s", state_words(vt));
            vt_since = now;
            vt_prev = vt;
            if (last_vt == 0x00299D7Cu) {
                extern void ssx3_PerfEvent(const char *name);
                event_left();
                ssx3_PerfEvent(NULL);     /* this event's frame times */
            }
            if (vt == 0x00289618u || vt == 0x0028825Cu)   /* back in the menus: not a restart */
                s_left_id = 0xFFFFFFFFu;
            /* entering an event / loading (the sampler can miss the short
             * first): its load starts */
            if ((vt == 0x0029AD48u || vt == 0x0029B0DCu) && !s_enter_ms)
                s_enter_ms = now ? now : 1;
            if (vt == 0x00299D7Cu)        /* in game: which event */
                event_note();
        }
        if (vt != last_vt || next != last_next || sub != last_sub) {
            if (vt != last_vt || sub != last_sub)
                since = now;
            fprintf(stderr, "[STATE] t=%lu vt=%08X %s sub=%u next=%08X phase=%u\n",
                    (unsigned long)(now - t0), vt, state_name(vt), sub, next, phase);
            fflush(stderr);
            last_vt = vt;
            last_next = next;
            last_sub = sub;
        }
        /* Rewritten every sample so a reader sees a fresh "seconds in state". */
        {
            FILE *f;
            extern uint32_t xbox_Nv2aDrawCount(void);
            /* 7th/8th fields: the active frontend screen (vtable and id,
             * ssx3_overrides.c sub_00204400) -- which menu page is up. */
            /* 9th: that screen's live state (5 = active; it moves on when the
             * screen closes) -- the in-game HUD is left out of fields 7/8,
             * so a pause menu over the race shows as the pause menu. */
            extern volatile uint32_t g_ssx3_ui_screen_vt, g_ssx3_ui_screen_id, g_ssx3_ui_screen_ptr;
            uint32_t sp = g_ssx3_ui_screen_ptr;
            unsigned sst = sp ? (unsigned)((MEM16(sp + 0x1C) >> 8) & 0x3F) : 0;
            snprintf(line, sizeof line, "%08X %s %lu %lu %u %u %08X %d %u\n", vt, state_name(vt),
                     (unsigned long)(now - t0), (unsigned long)((now - since) / 1000), sub,
                     xbox_Nv2aDrawCount(), g_ssx3_ui_screen_vt, (int)g_ssx3_ui_screen_id, sst);
            f = fopen("game_state.tmp", "w");
            if (f) {
                fputs(line, f);
                fclose(f);
                #ifdef _WIN32
                MoveFileExA("game_state.tmp", "game_state.txt", MOVEFILE_REPLACE_EXISTING);
#else
                rename("game_state.tmp", "game_state.txt");
#endif
            }
        }
    }
}

int ssx3_InGameNow(void)
{
    uint32_t app = MEM32(APP_PTR_VA), cur;
    return guest_ok(app) && guest_ok(cur = MEM32(app + 4)) && MEM32(cur) == 0x00299D7Cu;
}

/* Where the game is, in words, for the player mark (mark.c): the top-level
 * state, and the event when in game. */
void ssx3_WhereNow(char *out, size_t n)
{
    uint32_t app = MEM32(APP_PTR_VA), cur, vt = 0, id;
    if (guest_ok(app) && guest_ok(cur = MEM32(app + 4)))
        vt = MEM32(cur);
    id = MEM32(EVENT_ID_VA);
    if (vt == 0x00299D7Cu && id < 17)
        snprintf(out, n, "in game, Peak %d %s %s", k_events[id].peak, k_events[id].kind, k_events[id].name);
    else if (vt == 0x00299D7Cu && id == 17)
        snprintf(out, n, "in game, free ride");
    else
        snprintf(out, n, "%s", state_words(vt));
}

void ssx3_GameStateStart(void)
{
    const char *e = getenv("RECOMP_GAME_STATE");
    if (e && e[0] == '0')
        return;
    CloseHandle(CreateThread(NULL, 0, game_state_thread, NULL, 0, NULL));
}

/* Rider i's position (world, Z-up), for the motion blur (vkr_backend.c keeps
 * player 1 sharp and blurs the others by their own motion). The rider manager
 * [[[0x369534]+0x74]+0xC] (vtable 0x0028216C) holds the riders at
 * +0x28 + 4*i, slot 0 = player 1 (vtable 0x00282B08 in a race), then the
 * computer riders (0x00282520); a rider's position is at +0x180. A slot
 * counts when its object has one of those two vtables. 0 outside a race. */
int ssx3_RiderPos(int i, float out[3])
{
    uint32_t g = MEM32(0x00369534u), a, m, b, b0;
    if (i < 0 || i >= SSX3_RIDERS || !guest_ok(g) || !guest_ok(a = MEM32(g + 0x74))
            || !guest_ok(m = MEM32(a + 0xC)) || MEM32(m) != 0x0028216Cu
            || !guest_ok(b0 = MEM32(m + 0x28)) || !guest_ok(b = MEM32(m + 0x28 + 4u * (uint32_t)i))
            || (MEM32(b) != MEM32(b0) && MEM32(b) != 0x00282520u))
        return 0;
    out[0] = MEMF(b + 0x180);
    out[1] = MEMF(b + 0x184);
    out[2] = MEMF(b + 0x188);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

int ssx3_PlayerPos(float out[3])
{
    return ssx3_RiderPos(0, out);
}
