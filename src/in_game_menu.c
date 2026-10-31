/*
 * in_game_menu.c -- the options screen, drawn by the presenter over the game.
 *
 * The launcher used to hold every setting; this puts the same list inside the
 * game, where a player looks for it, and it is the only settings UI the port
 * needs. It draws into the finished frame (window pixels never enter into it,
 * so it looks the same at 480p and 4K), and the presenter hands it the keys
 * and fingers before the title sees them (toolkit src/video/ui_overlay.h).
 *
 * Opened with F10 or a pad's Back button on a desktop, or from the OPTIONS
 * pill the on-screen pad's devices get -- an in-game button, like the pause
 * menu it sits beside.
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#else
#include <time.h>
#endif

#include "font5x7.h"
#include "ui_font.h"
#include "video/ui_overlay.h"

/* SSX 3's own front end is white on near-black with one hot orange; the
 * screen keeps to those three and nothing else. */
#define C_PANEL   0x14161C
#define C_PANEL2  0x1C2029
#define C_RULE    0x2E3440
#define C_TEXT    0xF4F4F4
#define C_DIM     0x9AA2AE
#define C_ACCENT  0xFF8A1E
#define C_ACCENT2 0xFFC061
#define C_INK     0x0E0F12

#define CARD_ALPHA 246
#define SCRIM_ALPHA 200

static int      s_open;
static int      s_sel;                 /* index among the offered rows */
static int      s_scroll;
static unsigned char s_changed[32];    /* per settings row */
static int      s_fw = 640, s_fh = 480; /* the frame last drawn */
static int      s_row_y[16], s_row_n;  /* hit boxes from the last draw */
static int      s_panel[4];            /* x0, y0, x1, y1 from the last draw */
static int      s_close_tap[4];

#ifdef _WIN32
static unsigned s_pad_prev;
static unsigned long s_pad_nav_at;
static void poll_pad(void);
#endif

static unsigned long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
#endif
}

/* ---- drawing ----------------------------------------------------------- */

static void px_blend(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1,
                     uint32_t c, int a)
{
    int x, y;
    uint32_t cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (a <= 0)
        return;
    for (y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * (size_t)w;
        for (x = x0; x < x1; x++) {
            uint32_t p = row[x];
            uint32_t r = (((p >> 16) & 0xFF) * (255 - a) + cr * a) / 255;
            uint32_t g = (((p >> 8) & 0xFF) * (255 - a) + cg * a) / 255;
            uint32_t b = ((p & 0xFF) * (255 - a) + cb * a) / 255;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

static void px_fill(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * (size_t)w;
        for (x = x0; x < x1; x++)
            row[x] = c;
    }
}

static int text_w(int sc, const char *s)
{
    return ui_text_w(6 * sc, s);
}

/* Proportional type from ui_font.h: the shapes a PC game's menus use, which a
 * 5x7 bitmap never resembled. The italic flag is kept for the title's lean and
 * is spent as a per-row shear rather than a transform. */
static void text_draw(uint32_t *px, int w, int h, int x, int y, int sc,
                      const char *s, uint32_t c, int italic)
{
    int size = 6 * sc;
    if (size < 8)
        size = 8;
    (void)italic;
    ui_text(px, w, h, x, y, size, s, c, 255);
}

/* A bar whose ends lean right, the shape the title's own highlights use.
 * `skew` is how far the bottom edge is offset from the top one, in pixels --
 * independent of the bar's height, so a tall row and a short header lean by
 * the same amount and text inset by `skew` always stays inside. */
static void bar_skew(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1,
                     uint32_t c, int skew)
{
    int y, ymax = y1 < h ? y1 : h;
    int height = y1 - y0;
    if (height <= 0)
        return;
    for (y = y0; y < ymax; y++) {
        int dx = (y - y0) * skew / height;
        px_fill(px, w, h, x0 + dx, y, x1 + dx, y + 1, c);
    }
}

typedef struct {
    int ts, hs, row_h, pad, x0, y0, x1, y1, list_top, list_bot, vis, help_y, hint_y, skew;
} Layout;

static Layout layout_of(int w, int h)
{
    Layout L;
    int ts = h / 180;
    if (ts < 2) ts = 2;
    L.ts = ts;
    L.hs = ts > 3 ? ts / 2 : 2;
    L.row_h = (7 * ts) * 2;
    L.pad = 3 * ts;
    L.skew = ts * 3;
    L.x0 = (int)(w * 0.055f);
    L.x1 = w - L.x0;
    L.y0 = (int)(h * 0.075f);
    L.y1 = h - L.y0;
    L.list_top = L.y0 + (7 * ts) * 2 + L.pad * 2;
    L.hint_y = L.y1 - L.pad - 7 * L.hs;
    L.help_y = L.hint_y - L.pad - 7 * L.hs;
    L.list_bot = L.help_y - L.pad;
    L.vis = (L.list_bot - L.list_top) / L.row_h;
    if (L.vis < 3) L.vis = 3;
    if (L.vis > 9) L.vis = 9;
    return L;
}

static int row_of_slot(int slot)
{
    return ssx3_SettingsRowAt(slot);
}

static int slot_count(void)
{
    return ssx3_SettingsRowCount();
}

static void scroll_to_show(int slot, int vis)
{
    if (slot < s_scroll)
        s_scroll = slot;
    if (slot >= s_scroll + vis)
        s_scroll = slot - vis + 1;
    if (s_scroll < 0)
        s_scroll = 0;
    if (s_scroll > slot_count() - vis)
        s_scroll = slot_count() - vis;
    if (s_scroll < 0)
        s_scroll = 0;
}

/* "OPTIONS" pill, shown while the screen is closed on a touch device. Sized to
 * a finger rather than to the text: it is the only way in on a phone. */
static int pill_scale(int w, int h)
{
    int ts = h / 180;
    if (ts < 2) ts = 2;
    return ts + ts / 2;
}

static void pill_rect(int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    int ts = h / 180;
    int pts = pill_scale(w, h);
    int pw, ph;
    if (ts < 2) ts = 2;
    pw = text_w(pts, "OPTIONS") + ts * 6;
    ph = 7 * pts + ts * 6;
    *x0 = w / 2 - pw / 2;
    *y0 = h / 32;
    *x1 = *x0 + pw;
    *y1 = *y0 + ph;
}

static void draw_closed(uint32_t *px, int w, int h)
{
#if defined(__ANDROID__)
    int x0, y0, x1, y1, tw, pts = pill_scale(w, h);
    pill_rect(w, h, &x0, &y0, &x1, &y1);
    px_blend(px, w, h, x0, y0, x1, y1, C_PANEL, 170);
    px_fill(px, w, h, x0, y1 - 3, x1, y1, C_ACCENT);
    tw = text_w(pts, "OPTIONS");
    text_draw(px, w, h, (x0 + x1 - tw) / 2, y0 + (y1 - y0 - 7 * pts) / 2,
              pts, "OPTIONS", C_TEXT, 1);
#else
    (void)px; (void)w; (void)h;
#endif
}

static void draw_open(uint32_t *px, int w, int h)
{
    Layout L = layout_of(w, h);
    int i, n = slot_count(), y, big = L.ts + L.ts / 2;
    int changed_restart = 0;

    px_blend(px, w, h, 0, 0, w, h, 0x000000, SCRIM_ALPHA);          /* the game, held back */
    px_blend(px, w, h, L.x0, L.y0, L.x1, L.y1, C_PANEL, CARD_ALPHA);
    px_fill(px, w, h, L.x0, L.y0, L.x1, L.y0 + 2, C_RULE);
    px_fill(px, w, h, L.x0, L.y1 - 2, L.x1, L.y1, C_RULE);

    /* Header: the accent block behind the word, the title beside it. */
    {
        int bx0 = L.x0 + L.pad, by0 = L.y0 + L.pad;
        int bh = 7 * big + L.pad;
        int tx = bx0 + L.skew + L.pad / 2;
        int bw = (tx - bx0) + text_w(big, "OPTIONS") + L.pad + L.skew;
        bar_skew(px, w, h, bx0, by0, bx0 + bw, by0 + bh, C_ACCENT, L.skew);
        text_draw(px, w, h, tx, by0 + L.pad / 2, big, "OPTIONS", C_INK, 1);
    }
    {
#if defined(__ANDROID__)
        /* The way out, where a phone puts one: top right, a finger wide. */
        int cw = text_w(L.ts, "CLOSE") + L.pad * 2;
        int cx = L.x1 - L.pad - cw;
        int cy = L.y0 + L.pad / 2;
        px_fill(px, w, h, cx, cy, cx + cw, cy + 7 * L.ts + L.pad, C_PANEL2);
        px_fill(px, w, h, cx, cy + 7 * L.ts + L.pad - 3, cx + cw, cy + 7 * L.ts + L.pad, C_ACCENT);
        text_draw(px, w, h, cx + L.pad, cy + L.pad / 2, L.ts, "CLOSE", C_TEXT, 1);
        s_close_tap[0] = cx; s_close_tap[1] = cy;
        s_close_tap[2] = cx + cw; s_close_tap[3] = cy + 7 * L.ts + L.pad;
#else
        {
            int tw = text_w(L.ts, "SSX 3");
            text_draw(px, w, h, L.x1 - L.pad - tw, L.y0 + L.pad + (7 * big - 7 * L.ts) / 2,
                      L.ts, "SSX 3", C_DIM, 1);
        }
#endif
    }

    s_panel[0] = L.x0; s_panel[1] = L.y0; s_panel[2] = L.x1; s_panel[3] = L.y1;
    s_close_tap[0] = s_close_tap[1] = s_close_tap[2] = s_close_tap[3] = 0;

    s_row_n = 0;
    scroll_to_show(s_sel, L.vis);
    y = L.list_top;
    for (i = s_scroll; i < n && i < s_scroll + L.vis; i++, y += L.row_h) {
        int row = row_of_slot(i);
        const Ssx3Setting *s = &ssx3_settings[row];
        int selected = i == s_sel;
        int restart = s_changed[row] && !(s->flags & SSX3_LIVE);
        int vw = text_w(L.ts, s->names[ssx3_SettingsGet(row)]);
        int vy = y + (L.row_h - 7 * L.ts) / 2;
        int lx = L.x0 + L.pad * 2 + L.skew / 2;
        int vx = L.x1 - L.pad * 3 - vw - 2 * L.ts;   /* room for the < > marks */

        if (s_row_n < (int)(sizeof s_row_y / sizeof s_row_y[0])) {
            s_row_y[s_row_n] = y;
            s_row_n++;
        }
        if (restart)
            changed_restart = 1;
        if (selected) {
            bar_skew(px, w, h, L.x0 + L.pad, y + 2, L.x1 - L.pad, y + L.row_h - 2,
                     C_ACCENT, L.skew / 2);
            text_draw(px, w, h, lx, vy, L.ts, s->label, C_INK, 1);
            if (restart) {
                int rx = lx + text_w(L.ts, s->label) + L.pad;
                px_fill(px, w, h, rx, vy + L.ts, rx + 3 * L.ts, vy + 6 * L.ts, C_INK);
            }
            if (ssx3_SettingsGet(row) > 0)
                text_draw(px, w, h, vx - text_w(L.ts, "<") - L.ts, vy, L.ts, "<", C_INK, 0);
            text_draw(px, w, h, vx, vy, L.ts, s->names[ssx3_SettingsGet(row)], C_INK, 1);
            if (ssx3_SettingsGet(row) < s->count - 1)
                text_draw(px, w, h, vx + vw + L.ts / 2, vy, L.ts, ">", C_INK, 0);
        } else {
            uint32_t lc = restart ? C_ACCENT2 : C_TEXT;
            text_draw(px, w, h, lx, vy, L.ts, s->label, lc, 1);
            if (restart) {
                int rx = lx + text_w(L.ts, s->label) + L.pad;
                px_fill(px, w, h, rx, vy + L.ts, rx + 3 * L.ts, vy + 6 * L.ts, C_ACCENT);
            }
            text_draw(px, w, h, vx, vy, L.ts, s->names[ssx3_SettingsGet(row)], C_DIM, 1);
        }
        if (!selected)
            px_fill(px, w, h, L.x0 + L.pad, y + L.row_h - 1, L.x1 - L.pad, y + L.row_h, C_RULE);
    }

    /* Help for the row under the cursor, then the way out. */
    {
        int row = row_of_slot(s_sel);
        text_draw(px, w, h, L.x0 + L.pad * 2, L.help_y, L.hs, ssx3_settings[row].help, C_DIM, 0);
        if (changed_restart) {
            const char *note = "RESTART NEEDED";
            int tw = text_w(L.hs, note);
            text_draw(px, w, h, L.x1 - L.pad * 2 - tw, L.help_y, L.hs, note, C_ACCENT2, 0);
        }
    }
    {
        const char *hint =
#if defined(__ANDROID__)
            "TAP A ROW TO CHANGE";
#else
            "UP/DOWN SELECT   LEFT/RIGHT CHANGE   F10 OR ESC CLOSE";
#endif
        int hy = L.hint_y;
        text_draw(px, w, h, L.x0 + L.pad * 2, hy, L.hs, hint, C_DIM, 0);
    }
}

static void draw(uint32_t *px, uint32_t w, uint32_t h)
{
    s_fw = (int)w;
    s_fh = (int)h;
#ifdef _WIN32
    poll_pad();
#endif
    if (s_open)
        draw_open(px, (int)w, (int)h);
    else
        draw_closed(px, (int)w, (int)h);
}

/* ---- input ------------------------------------------------------------- */

static void open_(void)
{
    s_open = 1;
    s_sel = 0;
    s_scroll = 0;
}

static void close_(void)
{
    s_open = 0;
}

static void move(int d)
{
    int n = slot_count();
    s_sel = (s_sel + d + n) % n;
}

static void change(int d)
{
    int row = row_of_slot(s_sel);
    const Ssx3Setting *s = &ssx3_settings[row];
    int c = ssx3_SettingsGet(row) + d;
    if (c < 0)
        c = s->count - 1;
    if (c >= s->count)
        c = 0;
    ssx3_SettingsSet(row, c);
    s_changed[row] = 1;
    if (s->flags & SSX3_LIVE)
        s_changed[row] = 0;             /* already in effect; nothing to wait for */
}

static int key(int vk, int down)
{
    if (!down)
        return s_open;

    switch (vk) {
    case 0x79:                          /* F10 */
        if (s_open) close_(); else open_();
        return 1;
    case 0x1B:                          /* Esc */
        if (s_open) {
            close_();
            return 1;
        }
        return 0;
    default:
        break;
    }
    if (!s_open)
        return 0;
    switch (vk) {
    case 0x26: move(-1); break;         /* up */
    case 0x28: move(1);  break;         /* down */
    case 0x25: change(-1); break;       /* left */
    case 0x27: change(1);  break;       /* right */
    case 0x0D: close_(); break;         /* enter */
    default: break;
    }
    return 1;
}

/* A tap on a row selects it; a second tap on the row it already sits on moves
 * the value, which is the whole of the pointer grammar. */
static void touch_row(int y)
{
    int i;
    for (i = 0; i < s_row_n; i++) {
        if (y >= s_row_y[i] && y < s_row_y[i] + layout_of(s_fw, s_fh).row_h) {
            int slot = s_scroll + i;
            if (slot == s_sel)
                change(1);
            else
                s_sel = slot;
            return;
        }
    }
}

static int touch(float nx, float ny, int phase)
{
    int x = (int)(nx * (float)s_fw), y = (int)(ny * (float)s_fh);

    if (phase != 0)
        return s_open;
    if (!s_open) {
#if defined(__ANDROID__)
        int x0, y0, x1, y1;
        pill_rect(s_fw, s_fh, &x0, &y0, &x1, &y1);
        if (x >= x0 && x < x1 && y >= y0 && y < y1) {
            open_();
            return 1;
        }
#endif
        return 0;
    }
#if defined(__ANDROID__)
    if (s_close_tap[2] > s_close_tap[0] && x >= s_close_tap[0] && x < s_close_tap[2]
        && y >= s_close_tap[1] && y < s_close_tap[3]) {
        close_();
        return 1;
    }
#endif
    touch_row(y);
    return 1;
}

static int blocks_input(void)
{
    return s_open;
}

#ifdef _WIN32
/* Back opens the screen and closes it; the d-pad and the left stick walk the
 * list. Read from XInput directly: these presses are the port's, and the title
 * must not see them (xbox_UiBlocksInput covers that while the screen is up). */
static void poll_pad(void)
{
    XINPUT_STATE st;
    unsigned b, pressed;
    unsigned long t = now_ms();

    if (XInputGetState(0, &st) != ERROR_SUCCESS) {
        s_pad_prev = 0;
        return;
    }
    b = st.Gamepad.wButtons;
    pressed = b & ~s_pad_prev;
    s_pad_prev = b;

    if (pressed & XINPUT_GAMEPAD_BACK) {
        if (s_open) close_(); else open_();
        return;
    }
    if (!s_open)
        return;
    if (pressed & XINPUT_GAMEPAD_B) {
        close_();
        return;
    }
    if (t - s_pad_nav_at < 170)
        return;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) { move(-1); s_pad_nav_at = t; return; }
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) { move(1); s_pad_nav_at = t; return; }
    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { change(-1); s_pad_nav_at = t; return; }
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { change(1); s_pad_nav_at = t; return; }
    if (pressed & XINPUT_GAMEPAD_A) { change(1); s_pad_nav_at = t; return; }
}
#endif

void ssx3_InGameMenuInstall(void)
{
    xbox_ui_draw = draw;
    xbox_ui_key = key;
    xbox_ui_touch = touch;
    xbox_ui_blocks_input = blocks_input;
}

int ssx3_InGameMenuOpen(void)
{
    return s_open;
}
