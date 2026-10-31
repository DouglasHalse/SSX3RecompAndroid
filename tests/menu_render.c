/*
 * Renders the in-game options screen on the host, into raw RGB files, so the
 * layout can be looked at without a device: the Android branch (the OPTIONS
 * pill and its touch grammar) is compiled in, and touches are fed the way the
 * SDL presenter feeds them, in normalised window coordinates.
 *
 * Build and run: tools\menu_render.bat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "in_game_menu.h"
#include "video/ui_overlay.h"

/* The presenter's frame counter, which this test has no presenter for. */
void xbox_FramebufferSetFpsOverlay(int on) { (void)on; }

#define MAX_W 3840

static uint32_t *s_px;
static int s_w, s_h;

/* A stand-in for the presented frame: something with the sky and a horizon in
 * it, so a translucent panel reads the way it will over the game. */
static void backdrop(void)
{
    int x, y;
    for (y = 0; y < s_h; y++) {
        for (x = 0; x < s_w; x++) {
            int sky = y < s_h / 2;
            int r = sky ? 120 + y * 60 / (s_h / 2 + 1) : 235;
            int g = sky ? 170 + y * 40 / (s_h / 2 + 1) : 240;
            int b = sky ? 220 : 250;
            uint32_t c = (uint32_t)((r << 16) | (g << 8) | b);
            if (!sky && ((x / 90 + y / 70) & 1))
                c = 0xE8F2FA;
            s_px[(size_t)y * s_w + x] = c;
        }
    }
}

static void write_raw(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    for (i = 0; i < s_w * s_h; i++) {
        uint32_t p = s_px[i];
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s %dx%d\n", path, s_w, s_h);
}

static void frame(const char *path)
{
    backdrop();
    xbox_UiDraw(s_px, (uint32_t)s_w, (uint32_t)s_h);
    write_raw(path);
}

static void tap(float nx, float ny)
{
    xbox_UiTouch(nx, ny, 0);
    xbox_UiTouch(nx, ny, 1);
    xbox_UiTouch(nx, ny, 2);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 3 ? argv[3] : ".";
    char path[512];

    s_w = argc > 1 ? atoi(argv[1]) : 2400;
    s_h = argc > 2 ? atoi(argv[2]) : 1080;
    if (s_w < 320 || s_w > MAX_W || s_h < 240 || s_h > MAX_W) {
        fprintf(stderr, "size out of range\n");
        return 1;
    }
    s_px = (uint32_t *)calloc((size_t)s_w * s_h, 4);
    if (!s_px)
        return 1;

    ssx3_SettingsLoad();
    ssx3_InGameMenuInstall();

    snprintf(path, sizeof path, "%s/menu_android_0.raw", dir);   /* the pill, closed */
    frame(path);

    /* The pill sits at the top centre: h/40 to h/40 + h/15. */
    tap(0.5f, 0.05f);
    snprintf(path, sizeof path, "%s/menu_android_1.raw", dir);   /* opened by a tap */
    frame(path);

    /* Row three (anisotropic filtering): list starts under the header. */
    tap(0.3f, 0.30f);
    snprintf(path, sizeof path, "%s/menu_android_2.raw", dir);
    frame(path);
    tap(0.3f, 0.30f);                                            /* same row: cycle */
    snprintf(path, sizeof path, "%s/menu_android_3.raw", dir);
    frame(path);

    tap(0.5f, 0.5f);                                             /* a row, off to the side */
    snprintf(path, sizeof path, "%s/menu_android_4.raw", dir);
    frame(path);

    printf("open=%d\n", ssx3_InGameMenuOpen());
    free(s_px);
    return 0;
}
