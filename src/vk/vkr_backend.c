/* Glue: the executor's Nv2aBackend on top of the Vulkan renderer.
 *
 * Milestone-2 scope: one "main" colour target (the largest non-swizzled
 * 32-bit surface drawn to). Draws and clears on any other target are declined
 * so the software rasteriser still handles them in guest memory. */
#include "vkr_thread.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

/* RECOMP_VKR_STATS=1: where the backend's time goes, once a second. */
static double s_t[6];            /* draw total, lookup+hash, decode+upload, vkr_draw, flip */
static unsigned s_n[3];          /* draws, texture creates, flips */
static int s_stats = -1;

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

static void stats_tick(void)
{
    static double last;
    double t = now_ms();
    if (!last) last = t;
    if (t - last < 1000.0)
        return;
    fprintf(stderr, "[VKRSTAT] draws %u tex-creates %u flips %u | ms/s: draw-total %.0f lookup+hash %.0f"
            " decode+upload %.0f vkr_draw %.0f flip %.0f\n", s_n[0], s_n[1], s_n[2],
            s_t[0], s_t[1], s_t[2], s_t[3], s_t[4]);
    memset(s_t, 0, sizeof s_t); memset(s_n, 0, sizeof s_n);
    last = t;
}

#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"
#include "vkr.h"
#include "vkr_backend.h"
#include "vkr_hdtex.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GuestToHost(uint32_t va);   /* any mapped guest range (also the web build) */
extern void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h);
extern int ssx3_WidescreenNow(void);
extern int ssx3_InGame(void);
extern int ssx3_InGameMenuUp(void);
extern float ssx3_Aspect(void);
extern void ssx3_SetAspect(float a);
extern int xbox_FramebufferClientSize(int *w, int *h);

_Static_assert(sizeof(VkrVertex) == sizeof(Nv2aVshOutput),
               "VkrVertex must mirror Nv2aVshOutput");

#define PRIM_POINTS         1
#define PRIM_LINES          2
#define PRIM_LINE_LOOP      3
#define PRIM_LINE_STRIP     4
#define PRIM_TRIANGLES      5
#define PRIM_TRIANGLE_STRIP 6
#define PRIM_TRIANGLE_FAN   7
#define PRIM_QUADS          8
#define PRIM_QUAD_STRIP     9
#define PRIM_POLYGON        10

static VkrContext *s_vk;
static uint32_t s_w, s_h, s_zmax;
static int s_dirty;                     /* drawn since the last flip */

/* Flipped frames on their way to guest memory. The executor submits a frame
 * and goes on with the next one; this thread waits for the GPU and copies
 * the result into the surface the title flipped, so the executor never
 * waits for the GPU to finish a frame (it did: ~5 ms a flip, a quarter of
 * its time in a race). The copy lands a few ms after the flip. */
typedef struct { int slot; uint32_t offset, pitch, x, y, w, h; unsigned frame; int shot; } FlipJob;
static void mbtest_write(const FlipJob *j, const uint8_t *px, uint32_t w, uint32_t h);
static FlipJob s_jobs[4];
static int s_job_head, s_job_n;
static pthread_mutex_t s_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_cond = PTHREAD_COND_INITIALIZER;

/* For the slow-frame catcher (ssx3_overrides.c, RECOMP_SLOW_STACKS): when
 * the last flip came and on which threads (the GPU executor, the
 * flip thread that reads frames back). */
volatile uint32_t ssx3_last_flip_ms, ssx3_exec_tid, ssx3_flip_tid;
extern uint32_t xbox_TickCountMs(void);

/* RECOMP_GLITCH_CATCH=<dir>: finds frames where something shows for that
 * one frame only -- the frames before and after agree, it differs from
 * both -- and writes the three (guest size, BMP) into <dir> as
 * glitch_<frame>_{a,b,c}.bmp, with a [GLITCH] line. On the flip thread,
 * from the guest-size copy, 4x4 blocks of luminance. */
static unsigned s_spike_frame, s_spike_said;   /* flips (be_flip); RECOMP_SPIKE_TRACE lines said */
static int s_gl_on = -1;                       /* RECOMP_GLITCH_CATCH set */
/* RECOMP_GLITCH_CATCH's draw record: every draw of the last frames
 * (s_draw_frame numbering) with its screen box -- CPU-path draws only;
 * GPU-path ones have no box (run with RECOMP_GPU_VSH=0 for all). */
typedef struct { unsigned frame; uint16_t ord, prim; uint32_t n, tex; float x0, x1, y0, y1, w0, w1; uint8_t off, vsh; } GlDraw;
#define GL_RING 65536
static GlDraw s_gl_ring[GL_RING];
static volatile unsigned s_gl_head, s_draw_frame, s_gl_ord;
static void gl_record(uint32_t prim, uint32_t n, const Nv2aVshOutput *v, int off, int vsh)
{
    GlDraw *d = &s_gl_ring[s_gl_head++ % GL_RING];
    Nv2aTexInfo ti;
    uint32_t q;
    d->frame = s_draw_frame; d->ord = (uint16_t)s_gl_ord++; d->prim = (uint16_t)prim; d->n = n;
    d->tex = nv2a_exec_tex_info(0, &ti) ? ti.offset : 0;
    d->off = (uint8_t)off; d->vsh = (uint8_t)vsh;
    d->x0 = d->y0 = d->w0 = 1e30f; d->x1 = d->y1 = d->w1 = -1e30f;
    for (q = 0; v && q < n; q++) {
        const float *p = v[q].pos;
        if (p[0] < d->x0) d->x0 = p[0];
        if (p[0] > d->x1) d->x1 = p[0];
        if (p[1] < d->y0) d->y0 = p[1];
        if (p[1] > d->y1) d->y1 = p[1];
        if (p[3] < d->w0) d->w0 = p[3];
        if (p[3] > d->w1) d->w1 = p[3];
    }
}
static void gl_dump(const char *path, unsigned f0, unsigned f1)
{
    FILE *f = fopen(path, "w");
    unsigned h = s_gl_head, i;
    if (!f) return;
    for (i = h > GL_RING ? h - GL_RING : 0; i < h; i++) {
        const GlDraw *d = &s_gl_ring[i % GL_RING];
        if (d->frame < f0 || d->frame > f1)
            continue;
        fprintf(f, "%u %u %s%s prim %u n %u tex %08X x %.0f..%.0f y %.0f..%.0f w %.4g..%.4g\n",
                d->frame, d->ord, d->off ? "off" : "main", d->vsh ? " gpu" : "", d->prim, d->n, d->tex,
                d->x0, d->x1, d->y0, d->y1, d->w0, d->w1);
    }
    fclose(f);
}
#define GL_BW 160
#define GL_BH 120
static void glitch_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hd[54] = {'B', 'M'};
    uint32_t y, sz = 54 + w * h * 4;
    if (!f) return;
    memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
    memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4);
    hd[26] = 1; hd[28] = 32;
    fwrite(hd, 1, 54, f);
    for (y = h; y-- > 0;)
        fwrite(px + (size_t)y * w * 4, 1, (size_t)w * 4, f);
    fclose(f);
}
static void glitch_catch(const uint8_t *px, uint32_t w, uint32_t h, unsigned dframe)
{
    static int on = -1;
    static const char *dir;
    static uint8_t lum[3][GL_BH][GL_BW];
    static uint8_t *img[3];
    static unsigned frame, said, dfr[3];
    int x, y, nb = 0, x0 = GL_BW, x1 = -1, y0 = GL_BH, y1 = -1;
    uint8_t (*A)[GL_BW], (*B)[GL_BW], (*C)[GL_BW];
    if (on < 0) {
        dir = getenv("RECOMP_GLITCH_CATCH");
        on = dir && *dir;
    }
    if (!on || w < GL_BW * 4 || h < GL_BH * 4 || said >= 60)
        return;
    {
        int k;
        for (k = 0; k < 3; k++)
            if (!img[k] && !(img[k] = (uint8_t *)malloc((size_t)w * h * 4))) { on = 0; return; }
    }
    frame++;
    /* shift: 0 <- 1 <- 2 <- new */
    memmove(lum[0], lum[1], sizeof lum[0] * 2);
    { uint8_t *t = img[0]; img[0] = img[1]; img[1] = img[2]; img[2] = t; }
    dfr[0] = dfr[1]; dfr[1] = dfr[2]; dfr[2] = dframe;
    memcpy(img[2], px, (size_t)w * h * 4);
    for (y = 0; y < GL_BH; y++)
        for (x = 0; x < GL_BW; x++) {
            uint32_t sum = 0, i, j;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    const uint8_t *q = px + ((size_t)(y * h / GL_BH + j) * w + x * w / GL_BW + i) * 4;
                    sum += q[0] + 2u * q[1] + q[2];
                }
            lum[2][y][x] = (uint8_t)(sum / 64);
        }
    if (frame < 3)
        return;
    A = lum[0]; B = lum[1]; C = lum[2];
    {
        /* the camera moving fast: neighbours disagree everywhere -- skip */
        uint32_t tot = 0;
        for (y = 0; y < GL_BH; y++)
            for (x = 0; x < GL_BW; x++)
                tot += (uint32_t)abs(A[y][x] - C[y][x]);
        if (tot > 6u * GL_BW * GL_BH)
            return;
    }
    for (y = 0; y < GL_BH; y++)
        for (x = 0; x < GL_BW; x++) {
            int ab = abs(B[y][x] - A[y][x]), cb = abs(B[y][x] - C[y][x]), ac = abs(A[y][x] - C[y][x]);
            int d = ab < cb ? ab : cb;
            if (d > 28 && ac < d / 3 && (B[y][x] - A[y][x]) * (B[y][x] - C[y][x]) > 0) {
                nb++;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    {
        /* RECOMP_GLITCH_FROM=<frame>: skip the frames before (race-start
         * spotlights used the whole budget) */
        static long from = -1;
        if (from < 0) { const char *e = getenv("RECOMP_GLITCH_FROM"); from = e ? atol(e) : 0; }
        if ((long)frame - 1 < from)
            nb = 0;
    }
    if (nb >= 24 && ssx3_InGame() && !ssx3_InGameMenuUp()) {
        char path[512];
        said++;
        fprintf(stderr, "[GLITCH] frame %u (now at flip %u): %d blocks, box %d-%d x %d-%d (guest px)\n",
                frame - 1, s_spike_frame, nb, x0 * 4, x1 * 4 + 4, y0 * 4, y1 * 4 + 4);
        snprintf(path, sizeof path, "%s/glitch_%05u_a.bmp", dir, frame - 1); glitch_bmp(path, img[0], w, h);
        snprintf(path, sizeof path, "%s/glitch_%05u_b.bmp", dir, frame - 1); glitch_bmp(path, img[1], w, h);
        snprintf(path, sizeof path, "%s/glitch_%05u_c.bmp", dir, frame - 1); glitch_bmp(path, img[2], w, h);
        snprintf(path, sizeof path, "%s/glitch_%05u_draws.txt", dir, frame - 1);
        gl_dump(path, dfr[0], dfr[2]);
        fprintf(stderr, "[GLITCH]   draw frames a/b/c = %u/%u/%u\n", dfr[0], dfr[1], dfr[2]);
    }
}

/* RECOMP_RECORD=<file.mkv>: every full-resolution frame, gamma applied,
 * piped to ffmpeg as 60 FPS H.264 (Matroska survives a killed process).
 * Stops when the frame size changes. RECOMP_RECORD_INGAME=1: starts at the
 * first in-game frame (briefing / race). */
extern void xbox_FramebufferApplyGamma(uint32_t *px, size_t n);
extern int ssx3_InGame(void);

static void record_frame(const uint8_t *px, uint32_t w, uint32_t h)
{
    static int state;           /* 0 unchecked, 1 recording, -1 off */
    static FILE *pipe;
    static uint32_t rw, rh, *buf;
    if (state == 0) {
        const char *path = getenv("RECOMP_RECORD");
        char cmd[1024];
        if (path && *path && getenv("RECOMP_RECORD_INGAME") && !ssx3_InGame())
            return;
        state = -1;
        if (!path || !*path || !(buf = (uint32_t *)malloc((size_t)w * h * 4)))
            return;
        snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra"
                 " -s %ux%u -framerate 60 -i - -c:v libx264 -preset veryfast -crf 16"
                 " -pix_fmt yuv420p \"%s\"", w, h, path);
#ifdef _WIN32
        pipe = _popen(cmd, "wb");
#else
        pipe = popen(cmd, "w");
#endif
        if (!pipe)
            return;
        rw = w; rh = h; state = 1;
        fprintf(stderr, "[RECORD] %ux%u to %s\n", w, h, path);
    }
    if (state != 1)
        return;
    if (w != rw || h != rh) {
        fprintf(stderr, "[RECORD] frame size changed: stopped\n");
        fclose(pipe);
        state = -1;
        return;
    }
    memcpy(buf, px, (size_t)w * h * 4);
    xbox_FramebufferApplyGamma(buf, (size_t)w * h);
    fwrite(buf, 4, (size_t)w * h, pipe);
}

static void *flip_thread(void *unused)
{
    (void)unused;
#ifdef _WIN32
    ssx3_flip_tid = (uint32_t)GetCurrentThreadId();
#endif
    for (;;) {
        FlipJob j;
        const uint8_t *px;
        uint32_t y;
        pthread_mutex_lock(&s_job_lock);
        while (!s_job_n)
            pthread_cond_wait(&s_job_cond, &s_job_lock);
        j = s_jobs[s_job_head];
        pthread_mutex_unlock(&s_job_lock);
        px = vkr_frame_pixels(s_vk, j.slot);
        if (px) {
            uint8_t *surf = (uint8_t *)xbox_GuestToHost(j.offset);
            uint32_t fw, fh;
            const uint8_t *full;
            for (y = 0; y < j.h; y++)
                memcpy(surf + (size_t)(j.y + y) * j.pitch + (size_t)j.x * 4,
                       px + (size_t)y * j.w * 4, (size_t)j.w * 4);
            glitch_catch(px, j.w, j.h, j.frame);
            /* Higher internal resolution: the window shows the full image. */
            full = vkr_frame_pixels_full(s_vk, j.slot, &fw, &fh);
            if (full) {
                xbox_FramebufferPublishImage((const uint32_t *)full, fw, fh);
                record_frame(full, fw, fh);
            }
            if (full && j.shot)
                mbtest_write(&j, full, fw, fh);
        }
        pthread_mutex_lock(&s_job_lock);
        s_job_head = (s_job_head + 1) % 4;
        s_job_n--;
        pthread_mutex_unlock(&s_job_lock);
        vkr_frame_release(s_vk, j.slot);
    }
    return NULL;
}

/* Is t the surface the Vulkan target stands for? Adopts the first one that
 * looks like a frame buffer (>= 320x240 clip, linear, 32-bit). The target
 * covers the surface from its origin -- vertex positions are relative to it
 * -- and only grows: the clip rectangle (360..480 lines in a race: the
 * letterboxed start camera, HUD passes) becomes the scissor. Sizing the
 * target by the clip recreated it, blank, mid-frame and mapped positions
 * with the wrong size: black and misplaced geometry in races. */
/* Surfaces that have been flipped to the screen (double/triple buffered):
 * only those are the Vulkan target. Another linear 32-bit surface the title
 * renders into -- an offscreen image it later samples as a texture -- was
 * drawn into the main image instead: its memory stayed black (the black
 * shapes in races) and the frame got draws meant for it. Until the first
 * flip any frame-buffer-like surface is taken. */
static uint32_t s_front[4];
static int s_nfront;

static int is_front(uint32_t off)
{
    int i;
    for (i = 0; i < s_nfront; i++)
        if (s_front[i] == off)
            return 1;
    return s_nfront == 0;
}

/* Render targets the title draws (swizzled: the shadow maps), see note_rt. */
static uint32_t s_rt[16];
static int s_nrt;
static struct { uint32_t off, w, h; VkrTex *tex; } s_rt_tex[16];
static int s_nrt_tex;

/* Swizzled render targets the title draws (rider shadow maps): drawn in
 * software; RECOMP_VKR_NO_SHADOW=1 skips the draws that sample them (an
 * experiment for the black shadow-receiver shapes). */
static void note_rt(uint32_t off)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i] == off)
            return;
    if (s_nrt < 16)
        s_rt[s_nrt++] = off;
}

static int samples_rt(const VkrDrawState *st)
{
    int k, i;
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F) || !nv2a_exec_tex_info(k, &ti))
            continue;
        for (i = 0; i < s_nrt; i++)
            if (s_rt[i] == ti.offset)
                return 1;
    }
    return 0;
}

static int is_main(const Nv2aTarget *t)
{
    uint32_t w, h;
    if (t->swizzled)
        note_rt(t->color_offset);
    if (t->swizzled || t->bpp != 4)
        return 0;
    if (!is_front(t->color_offset)) {
        static uint32_t said[8];
        int i;
        for (i = 0; i < 8 && said[i] && said[i] != t->color_offset; i++)
            ;
        if (i < 8 && !said[i]) {
            said[i] = t->color_offset;
            fprintf(stderr, "[VKR] offscreen surface %08X %ux%u: drawn in software\n",
                    t->color_offset, t->width, t->height);
        }
        return 0;
    }
    if (t->width < 320 || t->height < 240)
        return 0;
    if (t->x + t->width > t->pitch / 4)
        return 0;
    w = t->x + t->width > s_w ? t->x + t->width : s_w;
    h = t->y + t->height > s_h ? t->y + t->height : s_h;
    if (w != s_w || h != s_h || t->zmax != s_zmax) {
        if (!vkr_set_target(s_vk, w, h, (float)t->zmax))
            return 0;
        s_w = w; s_h = h; s_zmax = t->zmax;
        fprintf(stderr, "[VKR] target %ux%u zmax %X\n", s_w, s_h, s_zmax);
    }
    vkr_set_scissor(s_vk, t->x, t->y, t->width, t->height);
    return 1;
}

/* Sharper shadows (RECOMP_SHADOW_RES, scale of the guest size, default 4:
 * SSX 3's 128x128 rider shadow maps at 512x512; 0 = the old software path):
 * a swizzled 32-bit render target is drawn on the GPU into an offscreen
 * target (vkr_offscreen_use) and later draws sample that image directly
 * (bind_textures). Its size: the clip rectangle rounded up to a power of
 * two (swizzled surfaces are). */
static float shadow_scale(void)
{
    static float sc = -1.0f;
    if (sc < 0.0f) {
        const char *e = getenv("RECOMP_SHADOW_RES");
        sc = e && *e ? (float)atof(e) : 4.0f;
        if (sc < 0.0f || sc > 8.0f)
            sc = 4.0f;
    }
    return sc;
}

static uint32_t pow2_ceil(uint32_t v)
{
    uint32_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

static int off_target(const Nv2aTarget *t)
{
    uint32_t w, h;
    if (shadow_scale() <= 0.0f || t->bpp != 4)
        return 0;
    if (!t->swizzled) {
        /* The same surface addressed in pitch layout: SSX 3 clears its
         * shadow maps that way every frame -- the GPU copy must see it. */
        if (!vkr_offscreen_size(s_vk, t->color_offset, &w, &h))
            return 0;
    } else {
        w = pow2_ceil(t->x + t->width); h = pow2_ceil(t->y + t->height);
        if (w > 1024 || h > 1024)
            return 0;
    }
    note_rt(t->color_offset);
    if (!vkr_offscreen_use(s_vk, t->color_offset, w, h, shadow_scale(), (float)t->zmax))
        return 0;
    vkr_set_scissor(s_vk, t->x, t->y, t->width, t->height);
    return 1;
}

/* The draw's target: an offscreen GPU target (1), the main target (1), or
 * neither (0: the executor draws it in software). */
static int pick_target(const Nv2aTarget *t, int *offscreen)
{
    *offscreen = off_target(t);
    if (*offscreen)
        return 1;
    vkr_offscreen_done(s_vk);
    return is_main(t);
}

static void fill_state(const Nv2aTarget *t, VkrDrawState *st)
{
    vkr_set_pillarbox(s_vk, !ssx3_WidescreenNow());
    VkrUniforms *u = &st->u;
    int k;
    st->point_size = t->point_size;
    st->point_params = t->point_params;
    st->point_sprite = t->point_sprite;
    st->depth_test = t->depth_test; st->depth_write = t->depth_write;
    st->depth_func = t->depth_func;
    st->stencil_test = t->stencil_test;
    st->stencil_func = t->stencil_func; st->stencil_ref = t->stencil_ref;
    st->stencil_read_mask = t->stencil_read_mask; st->stencil_write_mask = t->stencil_write_mask;
    st->stencil_fail = t->stencil_fail; st->stencil_zfail = t->stencil_zfail;
    st->stencil_zpass = t->stencil_zpass;
    st->w_buffer = t->w_buffer;
    st->blend_enable = t->blend_enable;
    st->blend_src = t->blend_sfactor; st->blend_dst = t->blend_dfactor;
    st->blend_eq = t->blend_equation;
    st->blend_color[0] = (float)((t->blend_color >> 16) & 0xFF) / 255.0f;
    st->blend_color[1] = (float)((t->blend_color >> 8) & 0xFF) / 255.0f;
    st->blend_color[2] = (float)(t->blend_color & 0xFF) / 255.0f;
    st->blend_color[3] = (float)(t->blend_color >> 24) / 255.0f;
    st->write_mask = ((t->color_mask >> 16) & 0xFF ? 1u : 0u) | ((t->color_mask >> 8) & 0xFF ? 2u : 0u)
                   | (t->color_mask & 0xFF ? 4u : 0u) | (t->color_mask >> 24 ? 8u : 0u);
    st->alpha_func = t->alpha_test ? t->alpha_func : 0;
    st->alpha_ref = t->alpha_ref & 0xFF;

    memcpy(u->color_icw, t->rc.color_icw, sizeof u->color_icw);
    memcpy(u->alpha_icw, t->rc.alpha_icw, sizeof u->alpha_icw);
    memcpy(u->color_ocw, t->rc.color_ocw, sizeof u->color_ocw);
    memcpy(u->alpha_ocw, t->rc.alpha_ocw, sizeof u->alpha_ocw);
    memcpy(u->factor0, t->rc.factor0, sizeof u->factor0);
    memcpy(u->factor1, t->rc.factor1, sizeof u->factor1);
    u->final0 = t->rc.final0; u->final1 = t->rc.final1;
    u->final_c0 = t->rc.final_c0; u->final_c1 = t->rc.final_c1;
    u->control = t->rc.control; u->stage_program = t->rc.stage_program;
    u->rc_seen = (uint32_t)t->rc_seen;
    u->fog_enable = t->fog_enable;
    {
        static int no_fog = -1;         /* RECOMP_VKR_NO_FOG=1: experiment */
        if (no_fog < 0) no_fog = getenv("RECOMP_VKR_NO_FOG") != NULL;
        if (no_fog) u->fog_enable = 0;
    }
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    u->fog_color[0] = (float)(t->fog_color & 0xFF) / 255.0f;
    u->fog_color[1] = (float)((t->fog_color >> 8) & 0xFF) / 255.0f;
    u->fog_color[2] = (float)((t->fog_color >> 16) & 0xFF) / 255.0f;
    u->fog_color[3] = (float)(t->fog_color >> 24) / 255.0f;
    u->fog_param[0] = t->fog_param[0]; u->fog_param[1] = t->fog_param[1];
    u->fog_param[2] = (float)t->fog_mode;
    {
        /* RECOMP_FOG_TRACE=1: each distinct fog setting once (linear fog
         * f = p0 + p1 * d: start where f = 1, end where f = 0). */
        static int on = -1, n;
        static float seen[64][3];
        int i;
        if (on < 0)
            on = getenv("RECOMP_FOG_TRACE") != NULL;
        if (on && t->fog_enable && n < 64) {
            for (i = 0; i < n; i++)
                if (seen[i][0] == (float)t->fog_mode && seen[i][1] == t->fog_param[0]
                    && seen[i][2] == t->fog_param[1])
                    break;
            if (i == n) {
                float p0 = t->fog_param[0], p1 = t->fog_param[1];
                seen[n][0] = (float)t->fog_mode; seen[n][1] = p0; seen[n][2] = p1; n++;
                fprintf(stderr, "[FOG] mode %X p0 %.6g p1 %.6g -> start %.1f end %.1f colour %08X\n",
                        t->fog_mode, p0, p1, p1 != 0.0f ? (1.0f - p0) / p1 : 0.0f,
                        p1 != 0.0f ? -p0 / p1 : 0.0f, t->fog_color);
            }
        }
    }
    for (k = 0; k < 4; k++) {
        u->tex_scale[k][0] = u->tex_scale[k][1] = 1.0f;
        u->tex_scale[k][2] = u->tex_scale[k][3] = 0.0f;
        st->addr_u[k] = st->addr_v[k] = 3;
    }
}


static int is_rt(uint32_t off)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i] == off)
            return 1;
    return 0;
}

/* Bind the texture stages the combiners use. */
static void bind_textures(VkrDrawState *st)
{
    int k;
    {
        /* RECOMP_VKR_STAGES=1: each distinct stage program once, with which
         * stages hold a cube map -- what the title's materials use. */
        static int on = -1, n;
        static uint32_t seen[64];
        int i;
        if (on < 0)
            on = getenv("RECOMP_VKR_STAGES") != NULL;
        if (on && n < 64) {
            uint32_t key = st->u.stage_program;
            for (k = 0; k < 4; k++) {
                Nv2aTexInfo ti;
                if (((key >> (k * 5)) & 0x1F) && nv2a_exec_tex_info(k, &ti) && ti.cube)
                    key |= 0x80000000u >> k;
            }
            for (i = 0; i < n && seen[i] != key; i++)
                ;
            if (i == n) {
                seen[n++] = key;
                fprintf(stderr, "[VK-STAGES] program %05X cube stages %X final %08X %08X\n",
                        key & 0xFFFFF, key >> 28, st->u.final0, st->u.final1);
            }
        }
    }
    {
        static uint32_t gen;                /* the texture cache was purged: drop our pointers */
        if (vkr_tex_generation(s_vk) != gen) {
            gen = vkr_tex_generation(s_vk);
            s_nrt_tex = 0;
        }
    }
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        VkrTex *tex;
        double t0 = s_stats ? now_ms() : 0;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F))
            continue;
        if (!nv2a_exec_tex_info(k, &ti))
            continue;
        /* A render target drawn on the GPU (shadow maps): its image, not
         * guest memory (which nothing writes any more). */
        tex = is_rt(ti.offset) && !ti.cube ? vkr_offscreen_tex(s_vk, ti.offset) : NULL;
        if (!tex)
            tex = vkr_tex_find(s_vk, ti.hash);
        if (s_stats) s_t[1] += now_ms() - t0;
        if (!tex) {
            double t1 = s_stats ? now_ms() : 0;
            uint32_t rows = ti.cube ? ti.height * 6 : ti.height;  /* cube: faces stacked */
            uint32_t *px = (uint32_t *)malloc((size_t)ti.width * rows * 4);
            int rt = is_rt(ti.offset), j;
            uint64_t key = ti.hash;
            if (px && nv2a_exec_tex_decode(k, px)) {
                /* A render target redrawn every frame: one texture per
                 * address, updated in place, not a new one per frame. */
                VkrTex *old = NULL;
                if (!rt && !ti.cube && hdtex_active()) {
                    /* HD replacement / texture dump (vkr_hdtex.c). One that
                     * cannot be made this frame: the game's pixels under a
                     * key nothing looks up, so the next frame tries again. */
                    uint64_t pk = hdtex_key(px, ti.width, ti.height);
                    int r;
                    hdtex_dump(pk, px, ti.width, ti.height, ti.format);
                    r = hdtex_get(s_vk, pk, ti.hash, &tex);
                    if (r < 0)
                        key = ti.hash ^ 0x8000000000000000ull;
                }
                if (rt)
                    for (j = 0; j < s_nrt_tex; j++)
                        if (s_rt_tex[j].off == ti.offset && s_rt_tex[j].w == ti.width
                            && s_rt_tex[j].h == ti.height)
                            old = s_rt_tex[j].tex;
                if (tex)
                    ;                   /* the HD replacement */
                else if (old && vkr_tex_update(s_vk, old, ti.hash, px))
                    tex = old;
                else {
                    /* Mips for ordinary textures; a render target is
                     * rewritten in place, a cube's faces are stacked. */
                    tex = vkr_tex_create_ex(s_vk, key, ti.width, rows, px, !rt && !ti.cube);
                    if (tex && rt && s_nrt_tex < 16) {
                        s_rt_tex[s_nrt_tex].off = ti.offset; s_rt_tex[s_nrt_tex].w = ti.width;
                        s_rt_tex[s_nrt_tex].h = ti.height; s_rt_tex[s_nrt_tex].tex = tex;
                        s_nrt_tex++;
                    }
                }
            }
            free(px);
            if (s_stats) { s_t[2] += now_ms() - t1; s_n[1]++; }
        }
        st->tex[k] = tex;
        st->addr_u[k] = ti.cube ? 3 : ti.addr_u;       /* a cube's faces are clamped */
        st->addr_v[k] = ti.cube ? 3 : ti.addr_v;
        st->linear[k] = ti.linear_filter;
        st->u.tex_scale[k][0] = ti.normalised ? 1.0f : 1.0f / (float)ti.width;
        st->u.tex_scale[k][1] = ti.normalised ? 1.0f : 1.0f / (float)ti.height;
        /* z: 1 = cube map, -1 = alpha on the PS2 scale (an HD replacement) */
        st->u.tex_scale[k][2] = ti.cube ? 1.0f
                              : (vkr_tex_flags(tex) & VKR_TEX_ALPHA2) ? -1.0f : 0.0f;
        st->u.tex_scale[k][3] = (float)ti.height;
    }
}

/* RECOMP_VKR_TRACE_AT=<ms since start>: log every large draw (> 2000 px of
 * screen box) of the first frame after that time -- what a wrong-looking
 * region is drawn with. s_trace: 0 off/done, 1 armed, 2 this frame. */
static int s_trace = -1;
static double s_trace_at, s_t0;
/* RECOMP_VKR_TRACE_EVERY=<ms>: re-arm after each traced frame, so a run
 * traces one frame per period (vkr_trace_<k>.bmp, log lines tagged k). */
static double s_trace_every;
static double s_trace_brief = -1;
extern volatile int g_ssx3_at_briefing;     /* autonav.c */
extern int xbox_PadInject(const char *line);   /* usb_gamepad.c */
static unsigned s_trace_no;

static void trace_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n,
                       const VkrDrawState *st)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f;
    uint32_t i;
    int k;
    for (i = 0; i < n; i++) {
        if (v[i].pos[2] < z0) z0 = v[i].pos[2];
        if (v[i].pos[2] > z1) z1 = v[i].pos[2];
        if (v[i].pos[0] < x0) x0 = v[i].pos[0];
        if (v[i].pos[0] > x1) x1 = v[i].pos[0];
        if (v[i].pos[1] < y0) y0 = v[i].pos[1];
        if (v[i].pos[1] > y1) y1 = v[i].pos[1];
    }
    if ((x1 - x0) * (y1 - y0) < 2000.0f)
        return;
    fprintf(stderr, "[VKRTRACE] prim %u n %u box %.0f,%.0f-%.0f,%.0f z %.4g..%.4g w0 %.3g d0 %.2f,%.2f,%.2f,%.2f | stages %05X",
            prim, n, x0, y0, x1, y1, z0 / (t->zmax ? (float)t->zmax : 1.0f), z1 / (t->zmax ? (float)t->zmax : 1.0f),
            v[0].pos[3], v[0].d0[0], v[0].d0[1], v[0].d0[2], v[0].d0[3],
            st->u.stage_program);
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F))
            continue;
        if (nv2a_exec_tex_info(k, &ti)) {
            const uint32_t *m = (const uint32_t *)xbox_GuestToHost(ti.offset);
            fprintf(stderr, " t%d:%08X fmt %X %ux%u%s [%08X %08X %08X %08X]", k, ti.offset, ti.format,
                    ti.width, ti.height, st->tex[k] ? "" : " (none)", m[0], m[1], m[2], m[3]);
        }
        else
            fprintf(stderr, " t%d:invalid", k);
    }
    fprintf(stderr, " | rc %u final %08X %08X fog %u | blend %d %X/%X alpha %X ref %u mask %X depth %d/%d %X\n",
            st->u.rc_seen, st->u.final0, st->u.final1, st->u.fog_enable, st->blend_enable, st->blend_src,
            st->blend_dst, st->alpha_func, st->alpha_ref, st->write_mask, st->depth_test, st->depth_write,
            st->depth_func);
    (void)t;
}

/* RECOMP_VKR_TRACE_BLACK=<n> (with RECOMP_VKR_TRACE_AT): in the traced frame,
 * read the target back after each of the first n draws and log how many
 * pixels are pure black after it, with the draw's vertices -- which draw
 * paints, or fails to cover, a black region. A readback per draw; n < 0
 * logs the first -n draws without reading back. */
static int s_tblack, s_tblack_init;
static unsigned s_draw_no;
static FILE *s_tbf;
static uint32_t *s_tbpx;   /* its own readback buffer, s_w x s_h */

static long count_black(void)
{
    long nb = 0;
    uint32_t i;
    for (i = 0; i < s_w * s_h; i++)
        if (!(s_tbpx[i] & 0xFFFFFF))
            nb++;
    return nb;
}

static void trace_black(uint32_t prim, const Nv2aVshOutput *v, uint32_t n, const VkrDrawState *st)
{
    uint32_t i;
    if (!s_tblack_init) {
        const char *e = getenv("RECOMP_VKR_TRACE_BLACK");
        s_tblack_init = 1;
        s_tblack = e ? atoi(e) : 0;
        /* Its own buffered file: thousands of lines through stderr took
         * seconds per draw. */
        if (s_tblack && !(s_tbf = fopen("vkr_black.txt", "w")))
            s_tblack = 0;
        if (s_tblack > 0 && !(s_tbpx = (uint32_t *)malloc((size_t)s_w * s_h * 4)))
            s_tblack = 0;
    }
    if (s_tblack == 0)
        return;
    /* A negative n: no readbacks, every draw's vertices (black count -1). */
    if (++s_draw_no > (unsigned)abs(s_tblack))
        return;
    if (s_tblack > 0 && !vkr_read_back(s_vk, (uint8_t *)s_tbpx))
        return;
    fprintf(s_tbf, "[VKRBLACK] %u draw #%u black %ld prim %u n %u stages %05X final %08X %08X"
            " blend %d %X/%X alpha %X/%u depth %d/%d %X fog %u\n", s_trace_no, s_draw_no,
            s_tblack > 0 ? count_black() : -1L,
            prim, n, st->u.stage_program, st->u.final0, st->u.final1, st->blend_enable, st->blend_src,
            st->blend_dst, st->alpha_func, st->alpha_ref, st->depth_test, st->depth_write,
            st->depth_func, st->u.fog_enable);
    {
        Nv2aTexInfo ti;
        if ((st->u.stage_program & 0x1F) && nv2a_exec_tex_info(0, &ti))
            fprintf(s_tbf, "[VKRBLACK]   t0 %08X fmt %X %ux%u addr %u/%u\n", ti.offset, ti.format,
                    ti.width, ti.height, ti.addr_u, ti.addr_v);
    }
    {
        /* RECOMP_VKR_TRACE_VSH=<a>-<b>: draws a..b of the traced frame also
         * log their program, constants and vertex inputs. */
        static int lo = -1, hi;
        if (lo < 0) {
            const char *e = getenv("RECOMP_VKR_TRACE_VSH");
            lo = hi = 0;
            if (e) sscanf(e, "%d-%d", &lo, &hi);
        }
        if ((int)s_draw_no >= lo && (int)s_draw_no <= hi)
            nv2a_exec_dump_batch(s_tbf, 64);
    }
    for (i = 0; i < n && i < 512; i++)
        fprintf(s_tbf, "[VKRBLACK]   v%u pos %g %g %g %g d0 %.2f %.2f %.2f %.2f fog %g t0 %g %g %g %g\n",
                i, v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3], v[i].d0[0], v[i].d0[1],
                v[i].d0[2], v[i].d0[3], v[i].fog[0], v[i].tex[0][0], v[i].tex[0][1],
                v[i].tex[0][2], v[i].tex[0][3]);
}

/* Triangle-list indices for n vertices of primitive prim, in a buffer
 * reused across calls; *k gets the count. NULL: not drawable here (points,
 * lines) or out of memory. */
/* Index list for one NV2A primitive; *topo: 0 triangle list, 1 line list,
 * 2 point list (VKR_TOPO_*). */
static uint32_t *build_idx(uint32_t prim, uint32_t n, uint32_t *k_out, uint32_t *topo)
{
    static uint32_t *idx;
    static uint32_t cap;
    uint32_t i, k = 0, need = n * 6;
    if (cap < need) {
        free(idx);
        cap = need;
        idx = (uint32_t *)malloc(cap * sizeof *idx);
        if (!idx) { cap = 0; return NULL; }
    }
    *topo = VKR_TOPO_TRIANGLES;
    switch (prim) {
    case PRIM_POINTS:
        *topo = VKR_TOPO_POINTS;
        for (i = 0; i < n; i++)
            idx[k++] = i;
        break;
    case PRIM_LINES:
        *topo = VKR_TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) {
            idx[k++] = i; idx[k++] = i + 1;
        }
        break;
    case PRIM_LINE_STRIP:
    case PRIM_LINE_LOOP:
        *topo = VKR_TOPO_LINES;
        for (i = 0; i + 1 < n; i++) {
            idx[k++] = i; idx[k++] = i + 1;
        }
        if (prim == PRIM_LINE_LOOP && n > 2) {
            idx[k++] = n - 1; idx[k++] = 0;
        }
        break;
    case PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
        }
        break;
    case PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            /* keep the winding consistent; culling is off, order is free */
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
        }
        break;
    case PRIM_TRIANGLE_FAN:
    case PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) {
            idx[k++] = 0; idx[k++] = i; idx[k++] = i + 1;
        }
        break;
    case PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
            idx[k++] = i; idx[k++] = i + 2; idx[k++] = i + 3;
        }
        break;
    case PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 3;
            idx[k++] = i; idx[k++] = i + 3; idx[k++] = i + 2;
        }
        break;
    default:
        {   /* anything else: say so once per type */
            static uint32_t said;
            if (prim < 32 && !(said & (1u << prim))) {
                said |= 1u << prim;
                fprintf(stderr, "[VKR] primitive %u (%u vertices) not drawn on the GPU\n", prim, n);
                fflush(stderr);
            }
        }
        return NULL;
    }
    *k_out = k;
    return idx;
}


/* RECOMP_VKR_PRIMLOG=1: each distinct state a line or point batch is drawn
 * with, once (topology, blend, stages, combiner, point state). */
/* RECOMP_VKR_PRIMLOG=1: each distinct state a line/point batch is drawn
 * with; =2 also blended triangle batches (trails, effects). With the depth
 * state (test/write/func, W-buffer). */
static uint32_t state_hash(const VkrDrawState *st)
{
    return st->topology * 0x9E3779B1u ^ st->u.stage_program * 31u ^ st->u.color_icw[0] * 7u
        ^ st->u.final0 ^ st->u.final1 * 3u ^ (st->blend_src << 8 | st->blend_dst) * 13u
        ^ (uint32_t)st->blend_enable * 17u ^ st->u.color_ocw[0]
        ^ ((uint32_t)st->depth_test << 1 | (uint32_t)st->depth_write << 2 | (uint32_t)st->w_buffer << 3
           | st->depth_func << 4) * 0x85EBCA6Bu
        ^ ((uint32_t)st->stencil_test | st->stencil_zpass << 1 | st->stencil_func << 17) * 0xC2B2AE35u;
}

static void primlog(const VkrDrawState *st, uint32_t n, int gpu_vsh)
{
    static int on = -1;
    static uint32_t seen[256];
    static int nseen;
    uint32_t h;
    int i;
    if (on < 0) {
        const char *e = getenv("RECOMP_VKR_PRIMLOG");
        on = e ? atoi(e) : 0;
    }
    if (!on || (st->topology == VKR_TOPO_TRIANGLES && (on < 2 || !st->blend_enable)))
        return;
    h = state_hash(st);
    for (i = 0; i < nseen; i++)
        if (seen[i] == h)
            return;
    if (nseen < 256)
        seen[nseen++] = h;
    fprintf(stderr, "[PRIMLOG] %s n %u vsh %d stage_prog %08X tex %d%d%d%d blend %d %X/%X eq %X"
            " rc %u icw0 %08X ocw0 %08X ia0 %08X final %08X %08X fac0 %08X pt %.1f/%d/%d alpha %X/%u"
            " depth test %d write %d func %X wbuf %d stencil %d func %X ref %X mask %X/%X op %X/%X/%X hash %08X\n",
            st->topology == VKR_TOPO_LINES ? "lines" : st->topology == VKR_TOPO_POINTS ? "points" : "tris",
            n, gpu_vsh, st->u.stage_program,
            st->tex[0] != NULL, st->tex[1] != NULL, st->tex[2] != NULL, st->tex[3] != NULL,
            st->blend_enable, st->blend_src, st->blend_dst, st->blend_eq, st->u.control & 0xFF,
            st->u.color_icw[0], st->u.color_ocw[0], st->u.alpha_icw[0], st->u.final0, st->u.final1,
            st->u.factor0[0], st->point_size, st->point_params, st->point_sprite,
            st->alpha_func, st->alpha_ref, st->depth_test, st->depth_write, st->depth_func, st->w_buffer,
            st->stencil_test, st->stencil_func, st->stencil_ref, st->stencil_read_mask, st->stencil_write_mask,
            st->stencil_fail, st->stencil_zfail, st->stencil_zpass, h);
    fflush(stderr);
}

/* HUD at the screen edges (widescreen, in game; RECOMP_HUD_EDGES, default
 * on; RECOMP_HUD_MARGIN = inset in % of the way to the edge). The game draws
 * its HUD for 4:3; the vertex shaders keep 2D draws in the 4:3 middle. A HUD
 * element whose box centre lies in the left third of the guest screen moves
 * with the left edge, right third with the right edge; the middle third
 * (timer, trick names) stays centred, and full-width passes keep their
 * stretch. Only while the HUD is the only screen up: the briefing, pause
 * menu and dialogs (opening and closing too) stay whole in the middle.
 *
 * An element is many draws -- text in batches of glyphs, a panel as body and
 * end caps -- in no particular order, and the HUD is one immediate-mode draw
 * function (sub_000AE9E0) with no widget boundaries to hook; the GPU runs
 * behind the game anyway. So elements are found from geometry: at each flip
 * the frame's small 2D boxes are joined into groups of touching pieces, and
 * each group is placed once, by its whole box. A draw in the next frame
 * takes the shift of the group it falls in, else (a new element, for its
 * first frame) its own. The groups are rebuilt from raw positions every
 * frame (deciding per draw tore the music notification and trick names
 * apart); only the placement is handed from an element to the same element
 * in the next frame, and changes after it has held for HUD_CONFIRM frames
 * and (leaving an edge) the box centre is HUD_BAND past a third's line -- pieces joining and
 * parting otherwise flipped elements between the edge and the middle. Draws wider
 * than HUD_PIECE_MAX of the screen -- 2D effects across the race, not HUD --
 * are placed alone and join no group.
 *
 * RECOMP_HUD_TRACE=1 logs one frame's decisions a second, =2 every frame. */
#define HUD_TOUCH_PX  4.0f             /* gap still counted as touching */
#define HUD_MOTION_PX 24.0f            /* sideways drift allowed against last frame:
                                          the music notification slides in ~8 px a frame */
#define HUD_PIECE_MAX 0.45f            /* of the guest width; the notification body is 0.36 */
#define HUD_MAX_BOXES 512

typedef struct { float x0, x1, y0, y1; } HudBox;
/* shift: the placement in use; pending/pend_n: a different placement and
 * for how many frames in a row the element's box has asked for it;
 * changed: the frame the placement last changed, from: what it was before
 * (the flicker report). */
typedef struct { HudBox b; float shift, pending, from; int pend_n; unsigned changed; } HudGroup;
#define HUD_CONFIRM   30               /* frames (0.5 s) a new placement must hold before it is taken:
                                          a passing trick name merged with the speed readout for 14 */
#define HUD_FLICKER   90               /* a change undone within this many frames is a flicker */

static HudBox   s_hud_boxes[HUD_MAX_BOXES];    /* this frame's pieces */
static int      s_hud_nboxes;
static HudGroup s_hud_groups[HUD_MAX_BOXES];   /* last frame's elements */
static int      s_hud_ngroups;
static int      s_hud_trace = -1, s_hud_trace_frame, s_hud_order;
static int      s_hud_menu;                    /* a menu was up this frame */
static double   s_hud_trace_t;

/* The layout runs in widescreen in game; with a menu up its pieces are still
 * recorded (so the first frame after the menu closes knows its elements) but
 * nothing moves. */
static int hud_edges_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HUD_EDGES");
        on = !(e && *e == '0');
    }
    return on && ssx3_WidescreenNow();
}

static int hud_near(const HudBox *a, const HudBox *b, float dx)
{
    return a->x0 <= b->x1 + dx && a->x1 >= b->x0 - dx
        && a->y0 <= b->y1 + HUD_TOUCH_PX && a->y1 >= b->y0 - HUD_TOUCH_PX;
}

/* One element: touching, or on the same text line with no more than a word
 * space between them (gap <= HUD_LINE_GAP x the line's height, vertical
 * overlap >= 60% of the lower box) -- a label and its value drawn apart,
 * like the "$" and the amount of Conquer the Mountain's money counter, which
 * sit on both sides of the two-thirds line and were placed apart. */
#define HUD_LINE_GAP 1.5f
static int hud_touch(const HudBox *a, const HudBox *b)
{
    float ha = a->y1 - a->y0, hb = b->y1 - b->y0, h = ha < hb ? ha : hb;
    float ov = (a->y1 < b->y1 ? a->y1 : b->y1) - (a->y0 > b->y0 ? a->y0 : b->y0);
    float gap = a->x0 > b->x1 ? a->x0 - b->x1 : b->x0 > a->x1 ? b->x0 - a->x1 : 0.0f;
    float hmax = ha > hb ? ha : hb;
    if (hud_near(a, b, HUD_TOUCH_PX))
        return 1;
    /* Text only: both pieces at most a line high (24 px) and of similar
     * height. Without that, two tall pieces -- the progress meter on the
     * left, the boost meter on the right, ~300 px each -- got a 450 px
     * allowance and chained the whole HUD into one element, which then sat
     * in the middle. */
    if (hmax > 24.0f || hmax > 1.5f * h)
        return 0;
    return h > 2.0f && ov >= 0.6f * h && gap <= HUD_LINE_GAP * h;
}

/* The NDC x offset for a box of the guest screen, by its centre. */
static float hud_place(const HudBox *b)
{
    static float margin = -1.0f;
    float gw = s_w ? (float)s_w : 640.0f, sq, cx;
    if (margin < 0.0f) {
        const char *e = getenv("RECOMP_HUD_MARGIN");
        margin = e && *e ? (float)atof(e) / 100.0f : 0.0f;
        if (margin < 0.0f) margin = 0.0f;
        if (margin > 1.0f) margin = 1.0f;
    }
    sq = 1.0f / (ssx3_Aspect() * 0.75f);   /* the shaders' squeeze2d */
    cx = (b->x0 + b->x1) * 0.5f;
    /* A box that starts at an edge (within 15%) and does not reach the
     * other belongs to that edge, whatever its width: the radio box with a
     * long song title (53-380) has its centre in the middle third. */
    if (b->x0 < gw * 0.15f && b->x1 <= gw * 0.85f)
        cx = 0.0f;
    else if (b->x1 > gw * 0.85f && b->x0 >= gw * 0.15f)
        cx = gw;
    if (cx < gw / 3.0f)
        return (sq - 1.0f) * (1.0f - margin);
    if (cx > gw * 2.0f / 3.0f)
        return (1.0f - sq) * (1.0f - margin);
    return 0.0f;
}

/* hud_place for an element already at an edge: its centre must pass the
 * third's line by HUD_BAND of the width before it leaves that edge. A text
 * box whose centre sits near the line (the freestyle tip panel, 53-349 px,
 * plus a prompt reaching to 388 for ~0.7 s) otherwise crossed it whenever a
 * piece came or went. Only the edges hold: held in the middle, the radio
 * box that slides in from the left stayed there. */
#define HUD_BAND 0.05f
static float hud_place_kept(const HudBox *b, float cur)
{
    float gw = s_w ? (float)s_w : 640.0f, band = gw * HUD_BAND;
    float cx = (b->x0 + b->x1) * 0.5f;
    if ((cur < 0.0f && (cx < gw / 3.0f + band || b->x0 < gw * 0.15f))
        || (cur > 0.0f && (cx > gw * 2.0f / 3.0f - band || b->x1 > gw * 0.85f)))
        return cur;
    return hud_place(b);
}

/* At the flip: this frame's pieces joined into elements (union-find over
 * touching boxes), each placed by its union box, for the next frame. */
static void hud_build_groups(void)
{
    static int parent[HUD_MAX_BOXES];
    int i, j, n = s_hud_nboxes;
    for (i = 0; i < n; i++)
        parent[i] = i;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (hud_touch(&s_hud_boxes[i], &s_hud_boxes[j])) {
                int a = i, b = j;
                while (parent[a] != a) a = parent[a] = parent[parent[a]];
                while (parent[b] != b) b = parent[b] = parent[parent[b]];
                if (a != b)
                    parent[b] = a;
            }
    /* Roots point to themselves; each becomes a group, its entry relabelled
     * -1 - group so the walk below stops there. */
    s_hud_ngroups = 0;
    for (i = 0; i < n; i++) {
        if (parent[i] == i) {
            int g = s_hud_ngroups++;
            s_hud_groups[g].b = s_hud_boxes[i];
            parent[i] = -1 - g;
        }
    }
    for (i = 0; i < n; i++) {
        int r = i, g;
        if (parent[i] < 0)
            continue;
        while (parent[r] >= 0) r = parent[r];
        g = -1 - parent[r];
        if (s_hud_boxes[i].x0 < s_hud_groups[g].b.x0) s_hud_groups[g].b.x0 = s_hud_boxes[i].x0;
        if (s_hud_boxes[i].x1 > s_hud_groups[g].b.x1) s_hud_groups[g].b.x1 = s_hud_boxes[i].x1;
        if (s_hud_boxes[i].y0 < s_hud_groups[g].b.y0) s_hud_groups[g].b.y0 = s_hud_boxes[i].y0;
        if (s_hud_boxes[i].y1 > s_hud_groups[g].b.y1) s_hud_groups[g].b.y1 = s_hud_boxes[i].y1;
    }
    {
        /* Each element against last frame's: the one at the same place
         * (overlapping, or centres within HUD_MOTION_PX) hands over its
         * placement, and the placement changes only after the element's box
         * has asked for another one HUD_CONFIRM frames in a row. Without
         * this, an element whose pieces join and part from frame to frame
         * (text whose width changes, glyphs that come and go) moved its box
         * centre across a third's line and back: the HUD flickered between
         * the edge and the middle. A new element is placed at once. */
        static HudGroup prev[HUD_MAX_BOXES];
        static int nprev;
        static unsigned frame, flicker_said;
        float gw = s_w ? (float)s_w : 640.0f;
        frame++;
        /* With a menu up nothing moves, and its panels overlap the HUD's
         * elements: carried over, their middle placement held the HUD in the
         * middle for HUD_CONFIRM frames after the briefing or the pause menu
         * closed, then it jumped to the edges. The first frame after a menu
         * places every element afresh. */
        if (s_hud_menu)
            nprev = 0;
        for (i = 0; i < s_hud_ngroups; i++) {
            HudGroup *g = &s_hud_groups[i];
            float want = hud_place(&g->b), best = 1e9f;
            int k, m = -1;
            for (k = 0; k < nprev; k++) {
                const HudBox *a = &prev[k].b;
                float dx = (a->x0 + a->x1 - g->b.x0 - g->b.x1) * 0.5f;
                float dy = (a->y0 + a->y1 - g->b.y0 - g->b.y1) * 0.5f;
                float d = dx * dx + dy * dy;
                int over = a->x0 < g->b.x1 && a->x1 > g->b.x0 && a->y0 < g->b.y1 && a->y1 > g->b.y0;
                if ((over || d <= HUD_MOTION_PX * HUD_MOTION_PX) && d < best) {
                    best = d;
                    m = k;
                }
            }
            /* A small part of a wide element (over a third of the screen)
             * is a new element: the briefing panel fading out over the HUD,
             * a dialog going away. Inheriting the wide group's middle kept
             * the leaderboard there for HUD_CONFIRM frames. */
            if (m >= 0 && prev[m].b.x1 - prev[m].b.x0 > gw / 3.0f
                && g->b.x1 - g->b.x0 < 0.5f * (prev[m].b.x1 - prev[m].b.x0))
                m = -1;
            if (m < 0) {
                g->shift = want; g->pending = want; g->pend_n = 0;
                g->changed = frame; g->from = want;
                continue;
            }
            g->shift = prev[m].shift; g->from = prev[m].from; g->changed = prev[m].changed;
            want = hud_place_kept(&g->b, g->shift);
            if (want == g->shift) {
                if (prev[m].pend_n > 0 && flicker_said < 50) {
                    /* Asked for another placement for a few frames, then back:
                     * before this rule that was a visible flicker. */
                    flicker_said++;
                    fprintf(stderr, "[HUD-FLICKER] held: element %.0f-%.0f x %.0f-%.0f asked %+.3f for %d frame(s), kept %+.3f\n",
                            g->b.x0, g->b.x1, g->b.y0, g->b.y1, prev[m].pending, prev[m].pend_n, g->shift);
                }
                g->pend_n = 0;
            } else {
                g->pend_n = prev[m].pending == want ? prev[m].pend_n + 1 : 1;
                if (g->pend_n >= HUD_CONFIRM) {
                    if (want == g->from && frame - g->changed < HUD_FLICKER && flicker_said < 50) {
                        flicker_said++;
                        fprintf(stderr, "[HUD-FLICKER] element %.0f-%.0f x %.0f-%.0f went %+.3f -> %+.3f -> %+.3f within %u frames\n",
                                g->b.x0, g->b.x1, g->b.y0, g->b.y1, g->from, g->shift, want,
                                frame - g->changed);
                    }
                    g->from = g->shift; g->shift = want; g->changed = frame; g->pend_n = 0;
                }
            }
            g->pending = want;
        }
        memcpy(prev, s_hud_groups, sizeof prev[0] * (size_t)s_hud_ngroups);
        nprev = s_hud_menu ? 0 : s_hud_ngroups;
        if (s_hud_menu) {
            /* The menu's panels joined the HUD under them into wide groups
             * placed in the middle; the first frame after the menu would
             * draw the HUD there once. Its pieces go by their own centres. */
            int k = 0;
            for (i = 0; i < s_hud_ngroups; i++)
                if (s_hud_groups[i].b.x1 - s_hud_groups[i].b.x0 <= gw / 3.0f)
                    s_hud_groups[k++] = s_hud_groups[i];
            s_hud_ngroups = k;
        }
        s_hud_menu = 0;
    }
    for (i = 0; i < s_hud_ngroups; i++) {
        if (s_hud_trace_frame)
            fprintf(stderr, "[HUD] group %d box %.0f-%.0f x %.0f-%.0f shift %+.3f\n", i,
                    s_hud_groups[i].b.x0, s_hud_groups[i].b.x1, s_hud_groups[i].b.y0,
                    s_hud_groups[i].b.y1, s_hud_groups[i].shift);
    }
    s_hud_nboxes = 0;
}

static float hud_traced(const HudBox *b, uint32_t n, float shift, const char *why)
{
    if (s_hud_trace_frame)
        fprintf(stderr, "[HUD] #%d box %.0f-%.0f x %.0f-%.0f n %u shift %+.3f %s\n",
                s_hud_order, b->x0, b->x1, b->y0, b->y1, n, shift, why);
    return shift;
}

static float hud_shift(const Nv2aVshOutput *v, uint32_t n)
{
    HudBox b = {1e9f, -1e9f, 1e9f, -1e9f};
    float gw = s_w ? (float)s_w : 640.0f;
    uint32_t i;
    int g, menu;
    if (!n || !hud_edges_on())
        return 0.0f;
    menu = ssx3_InGameMenuUp();
    s_hud_menu |= menu;
    for (i = 0; i < n; i++) {
        if (v[i].pos[3] != 1.0f)
            return 0.0f;                /* 3D */
        if (v[i].pos[0] < b.x0) b.x0 = v[i].pos[0];
        if (v[i].pos[0] > b.x1) b.x1 = v[i].pos[0];
        if (v[i].pos[1] < b.y0) b.y0 = v[i].pos[1];
        if (v[i].pos[1] > b.y1) b.y1 = v[i].pos[1];
    }
    s_hud_order++;
    if (b.x1 - b.x0 > gw * 0.9f)
        return hud_traced(&b, n, 0.0f, "full width");
    if (b.x1 - b.x0 > gw * HUD_PIECE_MAX)
        return hud_traced(&b, n, menu ? 0.0f : hud_place(&b), "wide: alone, by its centre");
    if (s_hud_nboxes < HUD_MAX_BOXES)
        s_hud_boxes[s_hud_nboxes++] = b;
    if (menu)
        return hud_traced(&b, n, 0.0f, "menu up: recorded, not moved");
    {
        int best = -1;
        float best_gap = 1e9f;
        for (g = 0; g < s_hud_ngroups; g++) {
            const HudBox *e = &s_hud_groups[g].b;
            float gap = b.x0 > e->x1 ? b.x0 - e->x1 : e->x0 > b.x1 ? e->x0 - b.x1 : 0.0f;
            if (gap < best_gap && hud_near(&b, e, HUD_MOTION_PX)) {
                best_gap = gap;
                best = g;
            }
        }
        if (best >= 0)
            return hud_traced(&b, n, s_hud_groups[best].shift, "in last frame's element");
    }
    return hud_traced(&b, n, hud_place(&b), "new: by its own centre");
}

/* Vertex 0 of a GPU-path draw through the CPU interpreter (the program and
 * constants are the current ones): 1 if it comes out 2D (w == 1). */
static int draw_is_2d(const float *in, const Nv2aVshProgram *p)
{
    float v[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    int a;
    memset(v, 0, sizeof v);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(v[a], in + 4 * p->in_slot[a], 16);
    if (!nv2a_vsh_run((const float (*)[4])v, &o))
        return 0;
    return o.pos[3] == 1.0f;
}

/* RECOMP_SPIKE_TRACE=1: logs triangles of GPU-path draws with an edge (in
 * the program's input position, attribute 0) RECOMP_SPIKE_RATIO (default 20)
 * times the draw's typical edge, degenerate strip joins left out -- a
 * vertex taken from elsewhere, the course slab stretched to the vanishing
 * point for one frame. Their CPU-run screen positions are logged too.
 * Diagnostic only (slow). */
static int s_spike = -1;
static float s_spike_px;
static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static void spike_check(const float *in, uint32_t n, const Nv2aVshProgram *p,
                        const uint32_t *idx, uint32_t k)
{
    static float *e;
    static uint32_t cap;
    uint32_t t, nt = k / 3, m, ps;
    float typ;
    if (s_spike_said >= 40 || nt < 4 || p->in_slot[0] == 0xFF)
        return;
    ps = p->in_slot[0];
    if (cap < nt) {
        free(e);
        cap = nt;
        e = (float *)malloc(cap * sizeof *e);
        if (!e) { cap = 0; return; }
    }
    for (t = m = 0; t < nt; t++) {
        const float *q[3];
        float mx = 0;
        int j, degen = 0;
        for (j = 0; j < 3; j++)
            q[j] = in + 4 * ((size_t)idx[3 * t + j] * p->nin + ps);
        for (j = 0; j < 3; j++) {
            const float *u = q[j], *v = q[(j + 1) % 3];
            float dx = u[0] - v[0], dy = u[1] - v[1], dz = u[2] - v[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d == 0.0f) degen = 1;
            if (d > mx) mx = d;
        }
        e[t] = degen ? -1.0f : mx;
        if (!degen) m++;
    }
    if (m < 4)
        return;
    {
        static float *srt;
        static uint32_t scap;
        uint32_t j = 0;
        if (scap < m) { free(srt); scap = m; srt = (float *)malloc(scap * sizeof *srt); if (!srt) { scap = 0; return; } }
        for (t = 0; t < nt; t++) if (e[t] >= 0.0f) srt[j++] = e[t];
        qsort(srt, m, sizeof *srt, cmp_float);
        typ = srt[m / 2];
    }
    if (!(typ > 0.0f))
        return;
    for (t = 0; t < nt && s_spike_said < 40; t++) {
        int j, a;
        if (!(e[t] > s_spike_px * typ))
            continue;
        s_spike_said++;
        fprintf(stderr, "[SPIKE] frame %u prog %08X n %u tris %u: tri %u edge %g = %.0fx typical %g\n",
                s_spike_frame, p->hash, n, nt, t, e[t], e[t] / typ, typ);
        for (j = 0; j < 3; j++) {
            uint32_t vi = idx[3 * t + j];
            float v[NV2A_VSH_INPUTS][4];
            Nv2aVshOutput o;
            const float *ip = in + 4 * ((size_t)vi * p->nin + ps);
            memset(v, 0, sizeof v);
            for (a = 0; a < NV2A_VSH_INPUTS; a++)
                if (p->in_slot[a] != 0xFF)
                    memcpy(v[a], in + 4 * ((size_t)vi * p->nin + p->in_slot[a]), 16);
            memset(&o, 0, sizeof o);
            nv2a_vsh_run((const float (*)[4])v, &o);
            fprintf(stderr, "[SPIKE]   v%u in %g %g %g %g -> screen %g %g %g w %g\n", vi,
                    ip[0], ip[1], ip[2], ip[3], o.pos[0], o.pos[1], o.pos[2], o.pos[3]);
        }
    }
}

static void spike_init(void)
{
    if (s_spike < 0) {
        const char *e = getenv("RECOMP_SPIKE_TRACE");
        s_spike = e && *e;
        e = getenv("RECOMP_SPIKE_RATIO");
        s_spike_px = e && *e ? (float)atof(e) : 20.0f;
    }
}

/* The scene camera (RECOMP_MBLUR): per frame, the map from world space to
 * what the terrain's vertex program outputs -- pixels x, y, depth-buffer
 * units z, clip w -- measured by running the program on the CPU at the
 * origin and the three unit points: (x w, y w, z w, w) is linear in the world
 * position, so four runs give the 4x4 matrix. Taken from the first draw of a
 * terrain program (RECOMP_SCENE_VSH, default E4F2B8D0, the slopes) each
 * frame; frames without one (menus) get no motion blur. */
static struct {
    int valid;                  /* this frame */
    double m[4][4];             /* rows: x w, y w, z w, w */
    double eye[3];
} s_cam;
static double s_cam_inv[4][4];  /* the last fitted camera's inverse */
static uint32_t s_rider_sharp, s_rider_blur;  /* rider draws (RECOMP_MBLUR_TRACE) */
static int s_cam_inv_ok;
static uint32_t s_cam_prog;      /* the program the camera was fitted from (trace) */
static int inv4(double a[4][4], double r[4][4]);

/* Program lists from the environment (hex, comma separated), with a default. */
static int prog_list(const char *env, const char *def, uint32_t *out, int max)
{
    const char *l = getenv(env);
    int n = 0;
    if (!l) l = def;
    while (*l && n < max) {
        char *end;
        uint32_t v = (uint32_t)strtoul(l, &end, 16);
        if (end == l) break;
        out[n++] = v;
        l = *end == ',' ? end + 1 : end;
    }
    return n;
}

static int cam_program(uint32_t hash)
{
    static int n = -1;
    static uint32_t want[8];
    int i;
    if (n < 0)
        n = prog_list("RECOMP_SCENE_VSH", "E4F2B8D0", want, 8);
    for (i = 0; i < n; i++)
        if (want[i] == hash)
            return 1;
    return 0;
}

/* Rider tags: a rider's draw by its vertex program (RECOMP_MBLUR_RIDER,
 * default E9FDBED0 SSX 3's riders, 5D983485 their boards -- two draws a
 * frame at the rider's distance, RECOMP_MBLUR_PROGS); the motion blur
 * leaves them sharp. */
static int rider_program(uint32_t hash)
{
    static int n = -1;
    static uint32_t rider[8];
    int i;
    if (n < 0)
        n = prog_list("RECOMP_MBLUR_RIDER", "E9FDBED0,5D983485", rider, 8);
    for (i = 0; i < n; i++)
        if (rider[i] == hash)
            return 1;
    return 0;
}

static void scene_camera(const float *in, const Nv2aVshProgram *p)
{
    /* the base vertex, then +-h along each axis: the program's output times
     * w is linear in the input, so a large step is exact and keeps its float
     * rounding (world coordinates ~1e5) out of the differences -- with a
     * unit step the fit changed with whichever vertex came first, and a
     * still camera showed motion. h stays under 10% of the vertex's w, so
     * no sample reaches the eye plane. */
    static const double pts[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    float vin[NV2A_VSH_INPUTS][4];
    double out[7][4], h = 1.0;
    int a, k, r;
    if (s_cam.valid || !cam_program(p->hash) || p->in_slot[0] == 0xFF)
        return;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    for (k = 0; k < 7; k++) {
        Nv2aVshOutput o;
        vin[0][0] = (float)(in[p->in_slot[0] * 4 + 0] + pts[k][0] * h);
        vin[0][1] = (float)(in[p->in_slot[0] * 4 + 1] + pts[k][1] * h);
        vin[0][2] = (float)(in[p->in_slot[0] * 4 + 2] + pts[k][2] * h);
        vin[0][3] = 1.0f;
        if (!nv2a_vsh_run((const float (*)[4])vin, &o))
            return;
        out[k][0] = (double)o.pos[0] * o.pos[3]; out[k][1] = (double)o.pos[1] * o.pos[3];
        out[k][2] = (double)o.pos[2] * o.pos[3]; out[k][3] = o.pos[3];
        if (k == 0) {
            /* a vertex at least 256 in front, so h >= 25.6: world coordinates
             * are ~3e5, where a float steps by 1/32 -- with a vertex near the
             * eye (h ~1) the fit came out rounded to 1/128ths, its
             * reprojection failed and that frame had no blur (the blur
             * flickered off for a frame in the Metro City pipes). The next
             * terrain draw of the frame is tried instead. */
            if (!(o.pos[3] > 256.0f))
                return;
            h = fmin(256.0, fmax(1.0, 0.1 * o.pos[3]));
        } else if (!(o.pos[3] > 0.0f))
            return;
    }
    {
        double base[3] = {in[p->in_slot[0] * 4], in[p->in_slot[0] * 4 + 1], in[p->in_slot[0] * 4 + 2]};
        for (r = 0; r < 4; r++) {
            for (k = 0; k < 3; k++)
                s_cam.m[r][k] = (out[1 + 2 * k][r] - out[2 + 2 * k][r]) / (2.0 * h);
            /* constant term at the world origin, not at the first vertex */
            s_cam.m[r][3] = out[0][r] - s_cam.m[r][0] * base[0] - s_cam.m[r][1] * base[1] - s_cam.m[r][2] * base[2];
        }
    }
    {
        /* the eye: where x w = y w = w = 0 (rows 0, 1, 3) */
        double a[3][4], f;
        int i, j, q;
        for (i = 0; i < 3; i++) {
            int row = i < 2 ? i : 3;
            for (j = 0; j < 3; j++) a[i][j] = s_cam.m[row][j];
            a[i][3] = -s_cam.m[row][3];
        }
        for (i = 0; i < 3; i++) {             /* Gauss-Jordan, partial pivot */
            int pv = i;
            for (q = i + 1; q < 3; q++) if (fabs(a[q][i]) > fabs(a[pv][i])) pv = q;
            for (j = 0; j < 4; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; }
            if (fabs(a[i][i]) < 1e-30) return;
            for (q = 0; q < 3; q++) {
                if (q == i) continue;
                f = a[q][i] / a[i][i];
                for (j = 0; j < 4; j++) a[q][j] -= f * a[i][j];
            }
        }
        for (i = 0; i < 3; i++) s_cam.eye[i] = a[i][3] / a[i][i];
    }
    s_cam.valid = 1;
    s_cam_prog = p->hash;
    s_cam_inv_ok = inv4(s_cam.m, s_cam_inv);
}

/* Which rider a rider draw is: its first vertex, taken back to the world
 * through the last fitted camera, lies within 3 m of that rider's position
 * (ssx3_RiderPos, game_state.c). The draw's tag (xbox.frag, scene.frag):
 * 255 player 1 (kept sharp), 1..7 another rider's slot (blurred by its own
 * motion on screen, scene_flip), 0 none found (blurred with the scene).
 * RECOMP_MBLUR_RIDERS=1 keeps every rider sharp. */
#define MB_RIDERS 8
extern int ssx3_RiderPos(int i, float out[3]);

static uint32_t rider_tag(const float *in, const Nv2aVshProgram *p)
{
    static int all = -1;
    float vin[NV2A_VSH_INPUTS][4], rp[3];
    double v[4], wp[4], best = 300.0 * 300.0;
    Nv2aVshOutput o;
    uint32_t tag = 0;
    int a, r, i;
    if (all < 0) {
        const char *e = getenv("RECOMP_MBLUR_RIDERS");
        all = e && *e && *e != '0';
    }
    if (all || !s_cam_inv_ok)
        return 255;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    if (!nv2a_vsh_run((const float (*)[4])vin, &o) || !(o.pos[3] > 0.0f))
        return 255;
    v[0] = (double)o.pos[0] * o.pos[3]; v[1] = (double)o.pos[1] * o.pos[3];
    v[2] = (double)o.pos[2] * o.pos[3]; v[3] = o.pos[3];
    for (r = 0; r < 4; r++)
        wp[r] = s_cam_inv[r][0] * v[0] + s_cam_inv[r][1] * v[1] + s_cam_inv[r][2] * v[2] + s_cam_inv[r][3] * v[3];
    if (fabs(wp[3]) < 1e-9)
        return 255;
    for (i = 0; i < MB_RIDERS; i++) {
        double d2 = 0.0;
        if (!ssx3_RiderPos(i, rp))
            continue;
        for (r = 0; r < 3; r++)
            d2 += (wp[r] / wp[3] - rp[r]) * (wp[r] / wp[3] - rp[r]);
        if (d2 < best) {
            best = d2;
            tag = i == 0 ? 255u : (uint32_t)i;
        }
    }
    return tag;
}

/* 4x4 inverse (Gauss-Jordan, partial pivoting); 0 if singular. */
static int inv4(double a[4][4], double r[4][4])
{
    double m[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            m[i][j] = j < 4 ? a[i][j] : (j - 4 == i);
    for (i = 0; i < 4; i++) {
        int p = i;
        for (k = i + 1; k < 4; k++) if (fabs(m[k][i]) > fabs(m[p][i])) p = k;
        if (fabs(m[p][i]) < 1e-300) return 0;
        for (j = 0; j < 8; j++) { double t = m[i][j]; m[i][j] = m[p][j]; m[p][j] = t; }
        for (k = 0; k < 4; k++) {
            double f;
            if (k == i) continue;
            f = m[k][i] / m[i][i];
            for (j = 0; j < 8; j++) m[k][j] -= f * m[i][j];
        }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = m[i][j + 4] / m[i][i];
    return 1;
}

/* At a flip: the frame's camera to the renderer (vkr_scene_set), positions
 * relative to the eye, depth shifted by zmax (scene_common.glsl). */
static int s_scene_sent;            /* vkr_scene_set done this frame */

/* Frame-to-frame camera numbers for the test harness (RECOMP_MBLUR_TEST)
 * and RECOMP_MBLUR_TRACE: speed in world units per frame, the largest
 * reprojection on a 5x5 grid of points 30 m away in guest pixels. */
static float s_mb_speed, s_mb_px;
static int s_mb_off_next;           /* the next frame without the pass (reference shot) */
static uint32_t s_mb_at_draw, s_draw_no, s_draw_total;  /* trace: the pass ran before draw N of the frame's total */

/* (gx, gy) in guest pixels of an eye-relative point through camera m
 * (eye-relative, 4x4); 0 when it is behind the camera */
static int cam_project(double m[4][4], const double p[3], double *gx, double *gy)
{
    double c[4];
    int r;
    for (r = 0; r < 4; r++)
        c[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    if (c[3] <= 1e-6)
        return 0;
    *gx = c[0] / c[3]; *gy = c[1] / c[3];
    return 1;
}

/* clip w of a world point through camera m (its distance in front, ~) */
static double rider_w(double m[4][4], const double p[3])
{
    return m[3][0] * p[0] + m[3][1] * p[1] + m[3][2] * p[2] + m[3][3];
}

static void scene_flip(void)
{
    static double prev[4][4], prev_eye[3], speed;
    static int have_prev;
    double me[4][4], pe[4][4], inv[4][4], step;
    VkrScene sc;
    int r, k;
    if (!s_cam.valid) {
        have_prev = 0;
        return;
    }
    if (s_scene_sent)
        return;
    s_scene_sent = 1;
    memset(&sc, 0, sizeof sc);
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 3; k++) me[r][k] = s_cam.m[r][k];
        me[r][3] = s_cam.m[r][0] * s_cam.eye[0] + s_cam.m[r][1] * s_cam.eye[1]
                 + s_cam.m[r][2] * s_cam.eye[2] + s_cam.m[r][3];
    }
    if (!inv4(me, inv))
        return;
    step = have_prev ? sqrt((s_cam.eye[0] - prev_eye[0]) * (s_cam.eye[0] - prev_eye[0])
                            + (s_cam.eye[1] - prev_eye[1]) * (s_cam.eye[1] - prev_eye[1])
                            + (s_cam.eye[2] - prev_eye[2]) * (s_cam.eye[2] - prev_eye[2])) : 0.0;
    /* a camera cut (a jump of over 5 m, or a different view): no blur this
     * frame -- it would smear the whole picture */
    if (step > 500.0)
        have_prev = 0;
    /* input (gx, gy, d - zmax, 1): column 3 takes zmax times column 2 */
    for (r = 0; r < 4; r++)
        for (k = 0; k < 4; k++)
            sc.inv_m[k * 4 + r] = (float)(inv[r][k] + (k == 3 ? inv[r][2] * (double)s_zmax : 0.0));
    /* last frame's camera on this frame's eye-relative positions */
    for (r = 0; r < 4; r++) {
        double (*pm)[4] = have_prev ? prev : me;
        for (k = 0; k < 3; k++) pe[r][k] = pm[r][k];
        pe[r][3] = pm[r][0] * s_cam.eye[0] + pm[r][1] * s_cam.eye[1] + pm[r][2] * s_cam.eye[2] + pm[r][3];
        for (k = 0; k < 4; k++)
            sc.prev_m[k * 4 + r] = (float)pe[r][k];
    }
    /* speed (smoothed over ~5 frames: a steady streak, not a pulsing one)
     * and the point the camera heads for */
    speed = have_prev ? speed + (step - speed) * 0.2 : 0.0;
    if (have_prev && step > 1.0) {
        double dir[3], gx, gy;
        for (k = 0; k < 3; k++)
            dir[k] = (s_cam.eye[k] - prev_eye[k]) / step * 1e5;
        if (cam_project(me, dir, &gx, &gy)) {
            sc.foe[0] = (float)gx; sc.foe[1] = (float)gy;
            sc.speed = (float)speed;
        }
    }
    {
        /* the reprojection on a grid at 30 m: zero for a still camera */
        double worst = 0.0;
        int gxn, gyn;
        for (gyn = 0; gyn < 5 && have_prev; gyn++)
            for (gxn = 0; gxn < 5; gxn++) {
                double q[4] = {(gxn + 0.5) / 5.0 * s_w, (gyn + 0.5) / 5.0 * s_h, 0.0, 1.0}, P[4], n, ax, ay, bx, by;
                q[2] = 0.5 * (double)s_zmax;
                for (r = 0; r < 4; r++)
                    P[r] = inv[r][0] * q[0] + inv[r][1] * q[1] + inv[r][2] * q[2] + inv[r][3];
                if (fabs(P[3]) < 1e-12) continue;
                for (k = 0; k < 3; k++) P[k] /= P[3];
                n = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
                if (n < 1e-9) continue;
                for (k = 0; k < 3; k++) P[k] *= 3000.0 / n;
                if (cam_project(me, P, &ax, &ay) && cam_project(pe, P, &bx, &by))
                    worst = fmax(worst, sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)));
            }
        s_mb_speed = (float)speed;
        s_mb_px = (float)worst;
        {
            /* RECOMP_MBLUR_TRACE=2: every frame */
            static int tr2 = -1;
            if (tr2 < 0) { const char *e = getenv("RECOMP_MBLUR_TRACE"); tr2 = e && atoi(e) >= 2; }
            if (tr2)
                fprintf(stderr, "[MBLUR2] f %u step %.2f speed %.2f reproj %.2f px prev %d same %d\n",
                        s_spike_frame, step, speed, worst, have_prev, !memcmp(prev, s_cam.m, sizeof prev));
            if (tr2 && have_prev && worst == 0.0 && step > 1.0) {
                for (r = 0; r < 4; r++)
                    fprintf(stderr, "[MBLUR2]   me %12.5g %12.5g %12.5g %12.5g | prev %12.5g %12.5g %12.5g %12.5g\n",
                            me[r][0], me[r][1], me[r][2], me[r][3], prev[r][0], prev[r][1], prev[r][2], prev[r][3]);
                fprintf(stderr, "[MBLUR2]   eye %.1f %.1f %.1f prev eye %.1f %.1f %.1f fit prog %08X\n",
                        s_cam.eye[0], s_cam.eye[1], s_cam.eye[2], prev_eye[0], prev_eye[1], prev_eye[2], s_cam_prog);
            }
        }
    }
    {
        /* RECOMP_MBLUR_TRACE=1: once a second, speed and reprojection */
        static int tr = -1;
        static double last, wpx;
        if (tr < 0) tr = getenv("RECOMP_MBLUR_TRACE") != NULL;
        wpx = fmax(wpx, s_mb_px);
        if (tr && now_ms() - last > 1000.0) {
            fprintf(stderr, "[MBLUR] speed %.1f/frame (%.0f km/h at 60 Hz), reprojection max %.2f px,"
                    " pass at draw %u of %u; rider draws sharp %u, blurred %u\n", speed, speed * 60.0 * 0.036,
                    wpx, s_mb_at_draw, s_draw_total, s_rider_sharp, s_rider_blur);
            s_rider_sharp = s_rider_blur = 0;
            last = now_ms();
            wpx = 0.0;
        }
    }
    {
        /* each other rider's motion on screen (guest pixels): this frame's
         * position through this frame's camera minus last frame's through
         * last frame's -- next to the player it is ~0, passing it is not */
        static double prev_pos[MB_RIDERS][3];
        static int prev_ok[MB_RIDERS];
        int i;
        for (i = 1; i < MB_RIDERS; i++) {
            float rp[3];
            double pos[3], ax, ay, bx, by;
            int ok = ssx3_RiderPos(i, rp);
            for (k = 0; k < 3; k++) pos[k] = rp[k];
            /* not across a respawn (a jump of over 5 m), nor for a rider at
             * the eye plane (its projection runs off to infinity) */
            if (ok && have_prev && prev_ok[i]
                    && (pos[0] - prev_pos[i][0]) * (pos[0] - prev_pos[i][0])
                     + (pos[1] - prev_pos[i][1]) * (pos[1] - prev_pos[i][1])
                     + (pos[2] - prev_pos[i][2]) * (pos[2] - prev_pos[i][2]) < 500.0 * 500.0
                    && rider_w(s_cam.m, pos) > 100.0 && rider_w(prev, prev_pos[i]) > 100.0
                    && cam_project(s_cam.m, pos, &ax, &ay)
                    && cam_project(prev, prev_pos[i], &bx, &by)) {
                sc.rider_v[i][0] = (float)(ax - bx);
                sc.rider_v[i][1] = (float)(ay - by);
            }
            memcpy(prev_pos[i], pos, sizeof pos);
            prev_ok[i] = ok;
        }
        {
            /* RECOMP_MBLUR_TRACE=1: once a second, each rider's motion */
            static int tr = -1;
            static double last;
            if (tr < 0) tr = getenv("RECOMP_MBLUR_TRACE") != NULL;
            if (tr && now_ms() - last > 1000.0) {
                char buf[256];
                int n = 0;
                for (i = 1; i < MB_RIDERS; i++)
                    if (prev_ok[i])
                        n += snprintf(buf + n, sizeof buf - n, " %d:%.1f", i,
                                      hypot(sc.rider_v[i][0], sc.rider_v[i][1]));
                buf[n] = 0;
                fprintf(stderr, "[MBLUR-RIDERS] px/frame%s\n", n ? buf : " none");
                last = now_ms();
            }
        }
    }
    sc.off = s_mb_off_next;
    s_mb_off_next = 0;
    memcpy(prev, s_cam.m, sizeof prev);
    memcpy(prev_eye, s_cam.eye, sizeof prev_eye);
    have_prev = 1;
    vkr_scene_set(s_vk, &sc);
}

/* The first 2D draw (screen space: w == 1) after the world in a frame: the
 * scene pass runs before it, so the HUD stays untouched (vkr_scene_run). */
static int s_scene_ran;

static void scene_before_2d(float w, float z)
{
    /* w == 1 at the far plane is the backdrop (SSX 3's sky sprites, drawn
     * between the terrain and the rocks, trees and riders), not the HUD */
    if (!s_cam.valid || s_scene_ran || w != 1.0f || z >= (float)s_zmax * 0.999f)
        return;
    s_scene_ran = 1;
    s_mb_at_draw = s_draw_no;
    {
        static int tr2 = -1;
        if (tr2 < 0) { const char *e = getenv("RECOMP_MBLUR_TRACE"); tr2 = e && atoi(e) >= 2; }
        if (tr2)
            fprintf(stderr, "[MBLUR2] f %u pass at draw %u (gl %u) w %g z %g\n", s_spike_frame,
                    s_draw_no, s_gl_ord, w, z);
    }
    scene_flip();
    vkr_scene_run(s_vk);
}

/* w (and z) of the program's first vertex, run on the CPU. */
static float vsh_first_w(const float *in, const Nv2aVshProgram *p, float *z)
{
    float vin[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    int a;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    if (!nv2a_vsh_run((const float (*)[4])vin, &o))
        return 0.0f;
    *z = o.pos[2];
    return o.pos[3];
}

/* Live settings (src/live.c, keys F4/F6): HD textures on/off (the cache
 * is emptied so every texture is made again with or without its
 * replacement) and rider shadows off (draws into the shadow maps are
 * dropped: the maps stay cleared, i.e. transparent, and nothing shows). */
static volatile int s_no_shadows;

int ssx3_LiveHdTextures(int on)
{
    hdtex_request(on);
    if (s_vk)
        vkr_tex_purge(s_vk);
    return on;
}

int ssx3_LiveHdTexturesOn(void)
{
    return hdtex_enabled();
}

void ssx3_LiveShadows(int on)
{
    s_no_shadows = !on;
}

int ssx3_LiveShadowsOn(void)
{
    return !s_no_shadows;
}

/* for the player mark's state line: 0 without a renderer */
int ssx3_RenderInfo(uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes)
{
    if (!s_vk)
        return 0;
    vkr_info(s_vk, w, h, msaa, tex_bytes);
    return 1;
}

static int be_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n)
{
    uint32_t *idx, k = 0, topo;
    int offscreen;

    if (s_no_shadows && t->swizzled)
        return 1;                       /* a rider shadow map's caster */

    double td = 0;
    if (s_stats < 0) s_stats = getenv("RECOMP_VKR_STATS") != NULL;
    if (s_stats) { td = now_ms(); stats_tick(); }
    if (!pick_target(t, &offscreen)) {
        /* RECOMP_VKR_SKIP_RT=1 (measurement): drop draws into swizzled
         * render targets (the rider shadow maps) instead of handing them to
         * the software rasteriser -- what drawing them costs. */
        static int skip_rt = -1;
        if (skip_rt < 0)
            skip_rt = getenv("RECOMP_VKR_SKIP_RT") != NULL;
        return skip_rt && t->swizzled;
    }
    if (!(idx = build_idx(prim, n, &k, &topo)))
        return 0;
    if (!k)
        return 1;
    /* The vertex program output is relative to the surface origin, which is
     * the target's; the clip rectangle is the scissor (is_main). */
    {
        VkrDrawState st;
        memset(&st, 0, sizeof st);
        fill_state(t, &st);
        st.topology = topo;
        st.hud_shift = offscreen ? 0.0f : hud_shift(v, n);
        if (s_gl_on < 0) s_gl_on = getenv("RECOMP_GLITCH_CATCH") != NULL;
        if (s_gl_on) gl_record(prim, n, v, offscreen, 0);
        {
            /* RECOMP_DRAWLOG=<first>-<last>: every CPU-path draw of those
             * flips (s_spike_frame numbering), with its screen box and w
             * range -- for finding which draw a one-frame glitch is. */
            static int on = -1;
            static unsigned f0, f1, last_f, k;
            if (on < 0) {
                const char *e = getenv("RECOMP_DRAWLOG");
                on = e && sscanf(e, "%u-%u", &f0, &f1) == 2;
            }
            if (on && s_spike_frame >= f0 && s_spike_frame <= f1) {
                float b[8] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f};
                uint32_t q;
                int c;
                Nv2aTexInfo ti;
                if (!nv2a_exec_tex_info(0, &ti)) ti.offset = 0;
                if (last_f != s_spike_frame) { last_f = s_spike_frame; k = 0; }
                for (q = 0; q < n; q++)
                    for (c = 0; c < 4; c++) {
                        if (v[q].pos[c] < b[2 * c]) b[2 * c] = v[q].pos[c];
                        if (v[q].pos[c] > b[2 * c + 1]) b[2 * c + 1] = v[q].pos[c];
                    }
                fprintf(stderr, "[DL] f %u #%u%s prim %u n %u x %.0f..%.0f y %.0f..%.0f z %.3g..%.3g w %.3g..%.3g tex %08X\n",
                        s_spike_frame, k++, offscreen ? " off" : "", prim, n, b[0], b[1], b[2], b[3],
                        b[4], b[5], b[6], b[7], ti.offset);
            }
        }
        {
            /* Only a pass across the whole screen keeps its edge vertices on
             * the edges of the wide frame. Deciding that per vertex stretched
             * panels sliding in from off-screen (the radio box, the briefing)
             * from the screen edge to the 4:3 middle for a frame or two. */
            float mn = 1e9f, mx = -1e9f, gw = s_w ? (float)s_w : 640.0f;
            uint32_t q;
            for (q = 0; q < n; q++) {
                if (v[q].pos[0] < mn) mn = v[q].pos[0];
                if (v[q].pos[0] > mx) mx = v[q].pos[0];
            }
            st.keep_edges = mx - mn > gw * 0.9f;
            st.clip2d = !offscreen && !st.keep_edges && n && v[0].pos[3] == 1.0f && ssx3_WidescreenNow();
        }
        double t2;
        bind_textures(&st);
        primlog(&st, n, 0);
        {
            static int no_shadow = -1;
            if (no_shadow < 0) no_shadow = getenv("RECOMP_VKR_NO_SHADOW") != NULL;
            if (no_shadow && samples_rt(&st))
                return 1;               /* swallowed: experiment */
        }
        if (s_trace == 2)
            trace_draw(t, prim, v, n, &st);
        if (n)
            scene_before_2d(v[0].pos[3], v[0].pos[2]);
        t2 = s_stats ? now_ms() : 0;
        vkr_draw(s_vk, (const VkrVertex *)v, n, idx, k, &st);
        if (s_stats) { s_t[3] += now_ms() - t2; s_n[0]++; }
        if (s_trace == 2)
            trace_black(prim, v, n, &st);
    }
    s_dirty = 1;
    if (s_stats) s_t[0] += now_ms() - td;
    return 1;
}

/* RECOMP_MBLUR_PROGS=1: finding programs to keep sharp (the board). Each
 * frame's main-target draws are kept as (program, distance of the first
 * vertex); at the flip, programs drawn at the rider's distance (0.7..1.4 x
 * the nearest rider draw's) are counted, and once a second the counts are
 * logged: [MBLUR-PROGS] hash:draws ... */
static int s_progs = -1, s_nprog_draw;
static struct { uint32_t hash; float w; } s_prog_draw[1024];

static void progs_note(uint32_t hash, float w)
{
    if (s_progs < 0) s_progs = getenv("RECOMP_MBLUR_PROGS") != NULL;
    if (s_progs && s_nprog_draw < 1024 && w > 0.0f) {
        s_prog_draw[s_nprog_draw].hash = hash;
        s_prog_draw[s_nprog_draw++].w = w;
    }
}

static void progs_flip(void)
{
    static uint32_t hs[32], cnt[32];
    static int nh;
    static double last;
    float rw = 0.0f;
    int i, j;
    if (s_progs <= 0)
        return;
    for (i = 0; i < s_nprog_draw; i++)
        if (rider_program(s_prog_draw[i].hash) && (rw == 0.0f || s_prog_draw[i].w < rw))
            rw = s_prog_draw[i].w;
    for (i = 0; rw > 0.0f && i < s_nprog_draw; i++) {
        float r = s_prog_draw[i].w / rw;
        if (r < 0.7f || r > 1.4f || rider_program(s_prog_draw[i].hash))
            continue;
        for (j = 0; j < nh && hs[j] != s_prog_draw[i].hash; j++)
            ;
        if (j == nh && nh < 32) hs[nh++] = s_prog_draw[i].hash;
        if (j < nh) cnt[j]++;
    }
    s_nprog_draw = 0;
    if (now_ms() - last > 1000.0) {
        char buf[600];
        int n = 0;
        last = now_ms();
        for (j = 0; j < nh && n < (int)sizeof buf - 24; j++)
            n += snprintf(buf + n, sizeof buf - n, " %08X:%u", hs[j], cnt[j]);
        fprintf(stderr, "[MBLUR-PROGS] rider w %.0f |%s\n", rw, nh ? buf : " none");
        nh = 0;
        memset(cnt, 0, sizeof cnt);
    }
}

/* The draw with the vertex program left to the GPU. Declined (so the CPU
 * runs it and calls be_draw) for other surfaces, and while a trace that needs
 * the transformed vertices is on. */
static int be_draw_vsh(const Nv2aTarget *t, uint32_t prim, const float *in, uint32_t n,
                       const Nv2aVshProgram *p)
{
    uint32_t *idx, k = 0, topo;
    int offscreen;
    double td = 0;
    if (s_trace == 2)
        return 0;
    if (s_stats < 0) s_stats = getenv("RECOMP_VKR_STATS") != NULL;
    if (s_stats) { td = now_ms(); stats_tick(); }
    if (!pick_target(t, &offscreen))
        return 0;
    /* 2D draws in widescreen game: the CPU runs the program, so be_draw sees
     * where the draw lands on screen and can anchor it (hud_shift). Which
     * draws are 2D: vertex 0 run on the CPU, w == 1. (A size limit decided
     * it before -- a text batch that grew past it went back to the middle
     * for a frame: the HUD jumped between the edge and the 4:3 middle.) */
    if (!offscreen && hud_edges_on() && draw_is_2d(in, p))
        return 0;
    if (!(idx = build_idx(prim, n, &k, &topo)))
        return 0;
    if (!k)
        return 1;
    {
        VkrDrawState st;
        double t2;
        memset(&st, 0, sizeof st);
        fill_state(t, &st);
        st.topology = topo;
        st.keep_edges = 1;              /* 2D here only with the HUD layout off */
        if (s_gl_on < 0) s_gl_on = getenv("RECOMP_GLITCH_CATCH") != NULL;
        if (s_gl_on) gl_record(prim, n, NULL, offscreen, 1);
        bind_textures(&st);
        primlog(&st, n, 1);
        scene_camera(in, p);
        {
            static int no_shadow = -1;
            if (no_shadow < 0) no_shadow = getenv("RECOMP_VKR_NO_SHADOW") != NULL;
            if (no_shadow && samples_rt(&st))
                return 1;
        }
        {
            /* RECOMP_DUMP_CONSTS=1: each distinct program's constants, once
             * (finding the camera projection for the widescreen work). */
            static int on = -1, nseen;
            static uint32_t seen[64];
            int i;
            if (on < 0) on = getenv("RECOMP_DUMP_CONSTS") != NULL;
            if (on) {
                for (i = 0; i < nseen && seen[i] != p->hash; i++)
                    ;
                if (i == nseen && nseen < 64) {
                    seen[nseen++] = p->hash;
                    fprintf(stderr, "[CONSTS] program %08X (%u insns) prim %u n %u\n", p->hash, p->nins, prim, n);
                    for (i = 0; i < 192; i++)
                        if (p->consts[i][0] || p->consts[i][1] || p->consts[i][2] || p->consts[i][3])
                            fprintf(stderr, "[CONSTS]   c[%d] %g %g %g %g\n", i, p->consts[i][0],
                                    p->consts[i][1], p->consts[i][2], p->consts[i][3]);
                }
            }
        }
        spike_init();
        if (s_spike && !offscreen && topo == VKR_TOPO_TRIANGLES)
            spike_check(in, n, p, idx, k);
        st.rider = 0;
        if (rider_program(p->hash)) {
            st.rider = (int)rider_tag(in, p);
            if (st.rider == 255) s_rider_sharp++; else s_rider_blur++;
        }
        s_draw_no++;
        if (s_cam.valid && !s_scene_ran && n) {
            float z0 = 0.0f, w0 = vsh_first_w(in, p, &z0);
            scene_before_2d(w0, z0);
        }
        if (s_progs != 0 && n) {
            float z0 = 0.0f;
            progs_note(p->hash, vsh_first_w(in, p, &z0));
        }
        t2 = s_stats ? now_ms() : 0;
        if (!vkr_draw_vsh(s_vk, in, n, p->nin, &p->ins[0][0], p->nins, p->hash, p->consts,
                          idx, k, &st))
            return 0;
        if (s_stats) { s_t[3] += now_ms() - t2; s_n[0]++; }
    }
    s_dirty = 1;
    if (s_stats) s_t[0] += now_ms() - td;
    return 1;
}

/* RECOMP_VKR_CLEARLOG=1: once a second, each distinct clear of the main
 * target (flags, clip rectangle, depth value) with how often it ran. */
static void clearlog(const Nv2aTarget *t, uint32_t flags, float depth)
{
    static int on = -1;
    static struct { uint32_t flags, x, y, w, h; float d; unsigned n; } e[16];
    static int ne;
    static double t0;
    int i;
    if (on < 0) on = getenv("RECOMP_VKR_CLEARLOG") != NULL;
    if (!on)
        return;
    for (i = 0; i < ne; i++)
        if (e[i].flags == flags && e[i].x == t->x && e[i].y == t->y && e[i].w == t->width
            && e[i].h == t->height && e[i].d == depth)
            break;
    if (i == ne && ne < 16) {
        e[i].flags = flags; e[i].x = t->x; e[i].y = t->y; e[i].w = t->width; e[i].h = t->height;
        e[i].d = depth; e[i].n = 0; ne++;
    }
    if (i < ne)
        e[i].n++;
    if (now_ms() - t0 >= 1000.0) {
        for (i = 0; i < ne; i++)
            fprintf(stderr, "[CLEARLOG] flags %X clip %u,%u %ux%u depth %g x%u\n", e[i].flags, e[i].x,
                    e[i].y, e[i].w, e[i].h, e[i].d, e[i].n);
        ne = 0;
        t0 = now_ms();
    }
}

static int be_clear(const Nv2aTarget *t, uint32_t flags, uint32_t argb, float depth)
{
    float c[4];
    int offscreen;
    if (!pick_target(t, &offscreen))
        return 0;
    if (!offscreen)
        clearlog(t, flags, depth);
    c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(argb & 0xFF) / 255.0f;
    c[3] = (float)(argb >> 24) / 255.0f;
    if (!(flags & (NV2A_CLEAR_COLOR | NV2A_CLEAR_Z | NV2A_CLEAR_STENCIL)))
        return 1;
    if (flags & NV2A_CLEAR_COLOR)
        vkr_clear(s_vk, c, 0, 0.0f);
    /* depth / stencil: colour untouched */
    vkr_clear_ds(s_vk, (flags & NV2A_CLEAR_Z) != 0, depth,
                 (flags & NV2A_CLEAR_STENCIL) != 0, t->stencil_clear);
    s_dirty = 1;
    return 1;
}

/* RECOMP_WIDESCREEN=auto: the frame takes the window's shape. Checked at
 * flips (10/s); a new shape is applied once the window has kept it for
 * 250 ms (not on every step of a drag), by rebuilding the target between
 * frames, and the camera (ssx3_Aspect) follows from the next frame on.
 * Narrower than 4:3 stays 4:3; wider than 8:1 stays 8:1. */
static void follow_window(void)
{
    static int on = -1;
    static double t_check, t_seen;
    static float seen;
    double now;
    float a, cur;
    int cw, ch;
    if (on < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        on = e && !strcmp(e, "auto");
    }
    if (!on || !s_w)
        return;
    now = now_ms();
    if (now - t_check < 100.0)
        return;
    t_check = now;
    if (!xbox_FramebufferClientSize(&cw, &ch))
        return;
    a = (float)cw / (float)ch;
    if (a < 4.0f / 3.0f) a = 4.0f / 3.0f;
    if (a > 8.0f) a = 8.0f;
    cur = ssx3_Aspect();
    if (fabsf(a - cur) < cur * 0.01f) {
        seen = 0.0f;
        return;
    }
    if (fabsf(a - seen) >= seen * 0.01f) {
        seen = a;
        t_seen = now;
        return;
    }
    if (now - t_seen < 250.0)
        return;
    ssx3_SetAspect(a);
    vkr_set_aspect(s_vk, ssx3_Aspect());
    seen = 0.0f;
    if (!vkr_set_target(s_vk, s_w, s_h, (float)s_zmax)) {
        s_w = s_h = 0;      /* rebuilt at the next draw */
        return;
    }
    fprintf(stderr, "[VKR] window %dx%d: frame aspect %.3f\n", cw, ch, ssx3_Aspect());
}


/* RECOMP_MBLUR_TEST=<dir>: a repeatable motion blur test, with
 * RECOMP_AUTONAV=race. Timed from the briefing (autonav presses A there):
 * the stick is held back from the start for brake= ms so the other riders
 * pass in front, then released; at each shot time a pair of frames is
 * saved -- <dir>/NN_blur.ppm and, the frame after, NN_ref.ppm without the
 * pass -- with the camera's speed and reprojection ([MBLUR-TEST]); after
 * the last one the process exits. RECOMP_MBLUR_TEST_PLAN=brake=<ms>,
 * shots=<ms>:<ms>:... (default brake=9000, shots at 4, 7, 10, 13, 16, 19,
 * 22, 25 s). */
static const char *s_mbt_dir;
static int s_mbt = -1, s_mbt_n, s_mbt_next, s_mbt_pending;
static uint32_t s_mbt_shots[32], s_mbt_brake = 9000;
static double s_mbt_t0;

static void mbtest_write(const FlipJob *j, const uint8_t *px, uint32_t w, uint32_t h)
{
    char path[1024];
    FILE *f;
    uint32_t i, n = w * h;
    uint8_t *row;
    snprintf(path, sizeof path, "%s/%02d_%s.ppm", s_mbt_dir, (j->shot - 1) / 2, j->shot & 1 ? "blur" : "ref");
    if (!(f = fopen(path, "wb")) || !(row = (uint8_t *)malloc((size_t)n * 3))) {
        if (f) fclose(f);
        return;
    }
    for (i = 0; i < n; i++) {           /* B G R A -> R G B */
        row[i * 3 + 0] = px[i * 4 + 2];
        row[i * 3 + 1] = px[i * 4 + 1];
        row[i * 3 + 2] = px[i * 4 + 0];
    }
    fprintf(f, "P6 %u %u 255\n", w, h);
    fwrite(row, 3, n, f);
    fclose(f);
    free(row);
    if ((j->shot - 1) / 2 + 1 >= s_mbt_n && !(j->shot & 1)) {
        fprintf(stderr, "[MBLUR-TEST] done\n");
        fflush(NULL);
        _exit(0);
    }
}

/* At each flip (recording side): the job's shot number, 0 = none. */
static int mbtest_flip(void)
{
    double t;
    if (s_mbt < 0) {
        const char *e = getenv("RECOMP_MBLUR_TEST"), *plan = getenv("RECOMP_MBLUR_TEST_PLAN");
        static const uint32_t def[] = {4000, 7000, 10000, 13000, 16000, 19000, 22000, 25000};
        s_mbt = e && *e;
        s_mbt_dir = e;
        memcpy(s_mbt_shots, def, sizeof def);
        s_mbt_n = 8;
        if (plan) {
            const char *b = strstr(plan, "brake="), *sh = strstr(plan, "shots=");
            if (b) s_mbt_brake = (uint32_t)strtoul(b + 6, NULL, 10);
            if (sh) {
                char *end;
                sh += 6;
                for (s_mbt_n = 0; s_mbt_n < 32; ) {
                    s_mbt_shots[s_mbt_n++] = (uint32_t)strtoul(sh, &end, 10);
                    if (*end != ':') break;
                    sh = end + 1;
                }
            }
        }
    }
    if (!s_mbt)
        return 0;
    if (!g_ssx3_at_briefing)
        return 0;
    t = now_ms();
    if (s_mbt_t0 == 0.0) {
        char line[32];
        s_mbt_t0 = t;
        if (s_mbt_brake) {
            snprintf(line, sizeof line, "ldown:%u", s_mbt_brake);
            xbox_PadInject(line);
        }
        fprintf(stderr, "[MBLUR-TEST] start: brake %u ms, %d shots\n", s_mbt_brake, s_mbt_n);
    }
    if (s_mbt_pending) {                /* the reference frame of the pair */
        int shot = s_mbt_pending;
        s_mbt_pending = 0;
        return shot;
    }
    if (s_mbt_next < s_mbt_n && t - s_mbt_t0 >= s_mbt_shots[s_mbt_next]) {
        fprintf(stderr, "[MBLUR-TEST] shot %02d t=%.0f speed %.1f/frame (%.0f km/h) reprojection %.2f px\n",
                s_mbt_next, t - s_mbt_t0, s_mb_speed, s_mb_speed * 60.0 * 0.036, s_mb_px);
        s_mb_off_next = 1;              /* the next frame: no pass */
        s_mbt_pending = s_mbt_next * 2 + 2;
        return s_mbt_next++ * 2 + 1;
    }
    return 0;
}

static void be_flip(const Nv2aTarget *t)
{
    FlipJob j;
    double tf;
    if (s_hud_trace < 0)
        s_hud_trace = getenv("RECOMP_HUD_TRACE") ? atoi(getenv("RECOMP_HUD_TRACE")) : 0;
    hud_build_groups();                 /* this frame's elements, for the next */
    s_spike_frame++;
    s_hud_order = 0;
    if (s_hud_trace_frame)
        fflush(stderr);
    s_hud_trace_frame = 0;
    if (s_hud_trace >= 2 || (s_hud_trace && now_ms() - s_hud_trace_t >= 1000.0)) {
        s_hud_trace_t = now_ms();
        s_hud_trace_frame = 1;
        fprintf(stderr, "[HUD] frame at %.0f ms\n", s_hud_trace_t);
    }
    ssx3_last_flip_ms = xbox_TickCountMs();
#ifdef _WIN32
    ssx3_exec_tid = (uint32_t)GetCurrentThreadId();
#endif
    if (s_trace < 0) {
        const char *e = getenv("RECOMP_VKR_TRACE_AT");
        s_trace = e ? 1 : 0;
        s_trace_at = e ? atof(e) : 0;
        e = getenv("RECOMP_VKR_TRACE_EVERY");
        s_trace_every = e ? atof(e) : 0;
        s_t0 = now_ms();
        /* RECOMP_VKR_TRACE_BRIEF=<ms>: the first traced frame that long
         * after autonav reached the briefing (what the probe scripts time
         * their shots from) instead of after start. */
        e = getenv("RECOMP_VKR_TRACE_BRIEF");
        if (e) {
            s_trace = 1;
            s_trace_brief = atof(e);
            s_trace_at = 1e18;
        }
    }
    if (s_trace_brief >= 0 && g_ssx3_at_briefing) {
        s_trace_at = now_ms() - s_t0 + s_trace_brief;
        s_trace_brief = -1;
    }
    if (s_trace == 2) {
        s_trace = s_trace_every > 0 ? 1 : 0;
        s_trace_at += s_trace_every;
        fprintf(stderr, "[VKRTRACE] end of frame %u\n", s_trace_no);
        if (s_tbf)
            fflush(s_tbf);
    } else if (s_trace == 1 && now_ms() - s_t0 >= s_trace_at) {
        s_trace = 2;
        s_draw_no = 0;
        s_trace_no++;
        fprintf(stderr, "[VKRTRACE] frame %u at %.0f ms (%u)\n", s_trace_no, now_ms() - s_t0,
                (unsigned)xbox_TickCountMs());
    }
    {
        int i;
        for (i = 0; i < s_nfront && s_front[i] != t->color_offset; i++)
            ;
        if (i == s_nfront && s_nfront < 4) {
            s_front[s_nfront++] = t->color_offset;
            fprintf(stderr, "[VKR] front buffer %08X\n", t->color_offset);
        }
    }
    if (!s_dirty || t->bpp != 4 || t->pitch / 4 < s_w)
        return;
    {
        extern void ssx3_PerfFlip(void);
        ssx3_PerfFlip();                /* [PERF] frame times (src/perf.c) */
    }
    tf = s_stats ? now_ms() : 0;
    progs_flip();
    scene_flip();
    s_draw_total = s_draw_no;
    s_draw_no = 0;
    j.shot = mbtest_flip();
    j.slot = vkr_flip_submit(s_vk);
    j.frame = s_draw_frame++;
    s_gl_ord = 0;
    if (s_stats) { s_t[4] += now_ms() - tf; s_n[2]++; }
    if (j.slot < 0)
        return;
    /* the target is the surface from its origin (is_main) */
    j.offset = t->color_offset; j.pitch = t->pitch;
    j.x = 0; j.y = 0; j.w = s_w; j.h = s_h;
    pthread_mutex_lock(&s_job_lock);
    s_jobs[(s_job_head + s_job_n) % 4] = j;  /* <= VKR_FRAMES pending: never full */
    s_job_n++;
    pthread_cond_signal(&s_job_cond);
    pthread_mutex_unlock(&s_job_lock);
    s_dirty = 0;
    s_cam.valid = 0;
    s_scene_sent = s_scene_ran = 0;
    follow_window();
}

static const Nv2aBackend s_backend = {be_draw, be_clear, be_flip, be_draw_vsh};

int vkr_backend_install(void)
{
    pthread_t th;
    s_vk = vkr_create();
    if (!s_vk)
        return 0;
    if (pthread_create(&th, NULL, flip_thread, NULL) != 0)
        return 0;
    pthread_detach(th);
    nv2a_exec_set_backend(&s_backend);
    fprintf(stderr, "[VKR] backend installed\n");
    return 1;
}
