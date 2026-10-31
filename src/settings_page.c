/*
 * settings_page.c -- the port's settings as a page of SSX 3's own Options
 * screen.
 *
 * Nothing here is a new kind of screen: the page is drawn where the game's
 * Options list is drawn, in the game's own 640x480 layout units, with the
 * background colour sampled from that page, labels right-aligned against the
 * same column, values in the column beside it, and the same orange bar on the
 * selected line. The game keeps drawing its own title, its art and its footer
 * ("Modify Game options.   A Select  B Previous") underneath; the pad filter
 * only takes the buttons once the page is up, so navigation outside it is the
 * title's own.
 *
 * It is reached from the game's list: the Credits row is relabelled VIDEO and
 * pressing A on it opens this page instead of the credits reel.
 */
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "settings_page.h"
#include "ui_font.h"
#include "video/ui_overlay.h"

/* The page's own units: the game's pages are laid out at 640x480 and scaled by
 * the presenter, so the same numbers hold at every resolution. */
#define PAGE_H   480
#define BAND_X0  182
#define BAND_X1  488
#define BAND_Y0  110
#define BAND_Y1  288
#define LINE_PX  22
#define LABEL_X  300
#define VALUE_X  312

/* Sampled from the game's own Options page and its selection bar. */
#define C_BG     0x6FA9D0u
#define C_PANEL  0x1E3E5Bu
#define C_ORANGE 0xDE6412u
#define C_TEXT   0xF6F9FCu
#define C_SHADE  0x1D3A52u
#define C_HEAD   0x8FB6D4u

/* The row the settings page hangs off in the game's Options list. */
#define ENTRY_ROW 6            /* Credits */

static int s_open, s_sel, s_scroll;
static int s_prev_buttons;

static void fill(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, uint32_t c)
{
    int y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++) {
        uint32_t *row = &px[(size_t)y * (size_t)w];
        int x;
        for (x = x0; x < x1; x++)
            row[x] = c;
    }
}

/* ---- the lines the page shows: category headers and settings ------------ */

typedef struct {
    int row;                 /* -1 for a header */
    const char *text;
} PageLine;

static PageLine s_lines[48];
static int s_nlines;

static int cat_of(const char *key)
{
    if (!strcmp(key, "RECOMP_RES") || !strcmp(key, "RECOMP_WIDESCREEN")
        || !strcmp(key, "RECOMP_MSAA") || !strcmp(key, "RECOMP_ANISO"))
        return 0;
    if (!strcmp(key, "RECOMP_DRAW_DIST") || !strcmp(key, "RECOMP_SHADOWS")
        || !strcmp(key, "RECOMP_HDTEX"))
        return 1;
    if (!strcmp(key, "RECOMP_HUD_EDGES") || !strcmp(key, "RECOMP_CRT")
        || !strcmp(key, "RECOMP_MBLUR"))
        return 2;
    return 3;
}

static const char *const s_cats[4] = { "VIDEO", "WORLD", "PICTURE", "SYSTEM" };

static void build_lines(void)
{
    int c, i, n = 0;

    s_nlines = 0;
    for (c = 0; c < 4; c++) {
        int any = 0;
        for (i = 0; i < ssx3_SettingsRowCount(); i++) {
            int row = ssx3_SettingsRowAt(i);
            if (cat_of(ssx3_settings[row].key) != c)
                continue;
            if (!any) {
                if (n < 48) { s_lines[n].row = -1; s_lines[n].text = s_cats[c]; n++; }
                any = 1;
            }
            if (n < 48) { s_lines[n].row = row; s_lines[n].text = ssx3_settings[row].label; n++; }
        }
    }
    s_nlines = n;
}

static int visible_lines(int h)
{
    return ((int)(BAND_Y0 * ((float)h / PAGE_H)) * 0) + 8;
}

/* ---- drawing ------------------------------------------------------------ */

/* The panel's dashed border, redrawn because the page fills over the game's
 * own one: 2px marks every 6px, the same rhythm the game uses. */
static void border(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, int s, uint32_t c)
{
    int i, step = (int)(6 * s);
    if (step < 4) step = 4;
    for (i = x0; i < x1; i += step) {
        fill(px, w, h, i, y0, i + step / 2, y0 + (int)(2 * s), c);
        fill(px, w, h, i, y1 - (int)(2 * s), i + step / 2, y1, c);
    }
    for (i = y0; i < y1; i += step) {
        fill(px, w, h, x0, i, x0 + (int)(2 * s), i + step / 2, c);
        fill(px, w, h, x1 - (int)(2 * s), i, x1, i + step / 2, c);
    }
}

static void select_first(void)
{
    s_sel = 0;
    s_scroll = 0;
    while (s_sel < s_nlines && s_lines[s_sel].row < 0)
        s_sel++;
}

static void draw(uint32_t *px, int w, int h)
{
    float s;
    int x0, x1, y0, line, size, i, vis, sel_line = 0;

    if (!s_open && getenv("RECOMP_PAGE_OPEN")) {
        /* Development knob: show the page without walking the list to it. */
        s_open = 1;
        select_first();
    }
    if (!s_open || !px || w <= 0 || h <= 0)
        return;

    s = (float)h / (float)PAGE_H;
    x0 = (int)(BAND_X0 * s);
    x1 = (int)(BAND_X1 * s);
    y0 = (int)(BAND_Y0 * s);
    line = (int)(LINE_PX * s);
    if (line < 6) line = 6;
    size = (int)(14.0f * s);
    if (size < 8) size = 8;

    vis = ((int)(BAND_Y1 * s) - (int)(96 * s)) / line;
    if (vis < 1) vis = 1;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis) s_scroll = s_sel - vis + 1;
    if (s_scroll < 0) s_scroll = 0;

    /* The game's own list panel and its description bar get filled: the page
     * replaces what was there rather than sitting inside it. */
    fill(px, w, h, x0, (int)(96 * s), x1, (int)(BAND_Y1 * s), C_PANEL);
    border(px, w, h, x0, (int)(96 * s), x1, (int)(BAND_Y1 * s), (int)s, C_HEAD);
    fill(px, w, h, 0, (int)(390 * s), (int)(453 * s), (int)(440 * s), C_PANEL);
    {
        int sel_row = s_lines[s_sel].row;
        if (sel_row >= 0)
            ui_text(px, w, h, (int)(28 * s), (int)(402 * s), (int)(13 * s),
                    ssx3_settings[sel_row].help, C_TEXT, 255);
    }

    for (i = s_scroll; i < s_nlines && i < s_scroll + vis; i++) {
        int y = y0 + (i - s_scroll) * line;
        PageLine *L = &s_lines[i];
        if (L->row < 0) {
            ui_text(px, w, h, x0 + (int)(6 * s), y + (line - ui_text_h(size)) / 2,
                    size, L->text, C_HEAD, 255);
            continue;
        }
        if (i == s_sel) {
            int lx = (int)((BAND_X0 + 4) * s), rx = (int)((BAND_X1 - 4) * s);
            fill(px, w, h, lx, y + (int)(1 * s), rx, y + line - (int)(1 * s), C_ORANGE);
            sel_line = 1;
        } else {
            sel_line = 0;
        }
        {
            int v = ssx3_SettingsGet(L->row);
            const char *val = (v >= 0 && v < ssx3_settings[L->row].count)
                              ? ssx3_settings[L->row].names[v] : "?";
            int lsz = size, lw = ui_text_w(lsz, L->text), ty;
            int avail = (int)((LABEL_X - BAND_X0 - 10) * s);

            /* A label never spills past the panel's left edge: it steps down a
             * point at a time until it fits, which is what keeps the longest
             * name ("ANISOTROPIC FILTERING") inside the plate. */
            while (lw > avail && lsz > 8) {
                lsz--;
                lw = ui_text_w(lsz, L->text);
            }
            ty = y + (line - ui_text_h(lsz)) / 2;

            ui_text(px, w, h, (int)(LABEL_X * s) - lw, ty, lsz, L->text, C_TEXT, 255);
            if (!sel_line) {
                int ax = (int)(LABEL_X * s) - lw;
                ui_text(px, w, h, ax + 1, ty + 1, lsz, L->text, C_SHADE, 90);
            }
            ui_text(px, w, h, (int)(VALUE_X * s), y + (line - ui_text_h(size)) / 2,
                    size, val, C_TEXT, 255);
        }
    }
}

/* ---- model -------------------------------------------------------------- */

static void move(int d)
{
    if (!s_nlines)
        return;
    do {
        s_sel = (s_sel + d + s_nlines) % s_nlines;
    } while (s_lines[s_sel].row < 0);
}

static void change(int d)
{
    int row = s_lines[s_sel].row;
    int n = ssx3_settings[row].count;
    int v = ssx3_SettingsGet(row);
    if (row < 0 || n <= 1)
        return;
    ssx3_SettingsSet(row, (v + d + n) % n);
}

/* ---- input -------------------------------------------------------------- */

static int key(int vk, int down)
{
    if (!down)
        return 0;
    if (!s_open)
        return 0;
    switch (vk) {
    case 0x26: move(-1); return 1;
    case 0x28: move(1);  return 1;
    case 0x25: change(-1); return 1;
    case 0x27: change(1);  return 1;
    case 0x0D: change(1);  return 1;
    case 0x1B: s_open = 0; return 1;
    default: return 0;
    }
}

/* The pad state the title polls (XINPUT_GAMEPAD bits). We see every poll, so
 * edges are tracked here; returning 0 makes the caller zero the buttons and
 * the title never sees the press. */
#define B_UP    0x0001
#define B_DOWN  0x0002
#define B_LEFT  0x0004
#define B_RIGHT 0x0008
#define B_A     0x1000
#define B_B     0x2000

static int buttons(unsigned short *b, unsigned char *analog)
{
    int pressed;

    if (!b)
        return 1;
    pressed = *b & ~s_prev_buttons;
    s_prev_buttons = *b;

    if (!s_open) {
        int sel = ssx3_OptionsRowSelected();
        if (pressed && getenv("RECOMP_PAGE_TRACE")) {
            fprintf(stderr, "[PAD] press %04X sel %d\n", pressed, sel);
            fflush(stderr);
        }
        /* The entry: A on the row of the game's own list that this page owns. */
        if (sel == ENTRY_ROW && (pressed & B_A)) {
            s_open = 1;
            select_first();
            return 0;
        }
        return 1;
    }

    if (pressed & B_UP) move(-1);
    if (pressed & B_DOWN) move(1);
    if (pressed & B_LEFT) change(-1);
    if (pressed & B_RIGHT) change(1);
    if (pressed & B_A) change(1);
    if (pressed & B_B) s_open = 0;
    if (analog)
        memset(analog, 0, 8);
    return 0;        /* the page owns the pad while it is up */
}

static int blocks(void)
{
    return 0;    /* the pad filter above does the swallowing */
}

void ssx3_SettingsPageInstall(void)
{
    build_lines();
    /* Scaffolding, not the goal: drawing our own page over the game's list is
     * still a second menu. Kept behind RECOMP_UI_PAGE=1 while the real work --
     * rows the game draws itself -- lands. */
    if (!getenv("RECOMP_UI_PAGE"))
        return;
    xbox_ui_draw = draw;
    xbox_ui_key = key;
    xbox_ui_blocks_input = blocks;
    xbox_ui_buttons = buttons;
}
