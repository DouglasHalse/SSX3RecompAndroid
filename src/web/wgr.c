/*
 * WebGPU renderer for the web build: the vkr.h API (see src/vk/vkr.c, the
 * Vulkan one), so vkr_backend.c drives it unchanged.
 *
 * WebGPU objects belong to one JS thread and only make progress when that
 * thread returns to its event loop, which the GPU executor's thread never
 * does. So this side only records: each frame's commands and every byte they
 * need (vertices, indices, uniforms, programs, constants, texture pixels) go
 * into one of two frame slots in shared memory, and at the flip the slot is
 * handed to web/gpu.js on the page thread. That replays it through WebGPU,
 * shows the frame on the canvas, reads it back into the slot's readback
 * buffer and marks the slot done -- the backend's flip thread copies that
 * into guest memory, exactly as with Vulkan.
 *
 * Slot states (ctl->slot[i].state, futex words shared with JS):
 *   FREE -> RECORDING (here) -> SUBMITTED (flip) -> GPU (gpu.js took it)
 *   -> DONE (pixels in rb) -> FREE (vkr_frame_release).
 * Commands are u32 words: op | nwords << 8, then the operands (WGR_* below,
 * mirrored in gpu.js).
 */
#if defined(__EMSCRIPTEN__)
#include "vkr.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/console.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WGR_SLOTS      2
#define WGR_DATA_BYTES (32u << 20)      /* per slot: everything the draws read */
#define WGR_CMD_WORDS  (1u << 20)       /* per slot: 4 MB of commands */
#define WGR_DRAW_BYTES 512u             /* per-draw parameter block, 256-aligned */

enum { S_FREE, S_RECORDING, S_SUBMITTED, S_DONE, S_GPU /* gpu.js has it */ };

enum {
    WGR_TARGET = 1,     /* w h sw sh                         new colour + depth target */
    WGR_SCISSOR,        /* x y w h (target pixels) */
    WGR_CLEAR,          /* param_off flags(1 colour, 2 depth, 4 stencil) stencil */
    WGR_TEX_CREATE,     /* id w h data_off bytes_per_row */
    WGR_TEX_UPDATE,     /* id data_off bytes_per_row */
    WGR_TEX_FREE,       /* id */
    WGR_DRAW,           /* param_off nverts key0 key1 tex[4] samp[4] blend_color[4]
                           stencil(test func fail zfail zpass) masks(read | write << 8) ref */
    WGR_FLIP,           /* rb_ptr w h bars gamma_ptr gamma_serial */
};

typedef struct {
    volatile uint32_t state;
    uint32_t cmd_ptr, cmd_words, data_ptr, data_bytes;
    uint32_t frame;
    uint32_t pad[2];
} WgrSlot;

typedef struct {
    uint32_t magic;                     /* 'WGR1' */
    uint32_t nslots;
    volatile uint32_t submits;          /* bumped and woken on each hand-over */
    volatile uint32_t gpu_failed;       /* set by gpu.js: no device / lost */
    WgrSlot slot[WGR_SLOTS];
} WgrCtl;

/* The per-draw parameter block, as xbox.wgsl's Draw. */
typedef struct {
    VkrUniforms u;                      /* 320 bytes */
    float inv_size[2], inv_zmax, fog_mode;
    float fogp[2], point_size, point_params;
    uint32_t in_base, nin, prog_base, const_base;
    float squeeze2d, squeeze_all;
    uint32_t vsh, idx_base;
    uint32_t vtx_base, topo;
    float line_w;
    uint32_t alpha_func;
    uint32_t alpha_ref;
    float w_scale;
    uint32_t point_sprite, border;
    float tsize[2], ox, pad;
} WgrDraw;
_Static_assert(sizeof(VkrUniforms) == 320, "VkrUniforms layout");
_Static_assert(sizeof(WgrDraw) == 432, "WgrDraw layout (xbox.wgsl Draw)");

struct VkrTex {
    uint64_t key;
    uint32_t id, w, h;
    uint32_t last_use;
};

struct VkrContext {
    WgrCtl *ctl;
    uint32_t *cmd[WGR_SLOTS];
    uint8_t *data[WGR_SLOTS];
    uint8_t *rb[WGR_SLOTS];             /* w x h x 4, filled by gpu.js */
    uint32_t rb_w, rb_h;
    int cur;                            /* slot being recorded, -1 none */
    int next;                           /* slot the next recording uses */
    uint32_t ncmd, nbytes;              /* used in the current slot */
    uint32_t frame;

    uint32_t tex_gen, tex_id_next, flip_serial;
    VkrTex **texs;
    uint32_t ntexs, captexs;
    uint64_t tex_bytes;
    int32_t *hidx;
    uint32_t hcap;

    float scale, scale_x, squeeze2d, squeeze_all;
    uint32_t w, h, sw, sh;
    float zmax;
    uint32_t gsc[4];                    /* clip rectangle, guest pixels */
    uint32_t sc[4];                     /* the scissor last recorded, target pixels */
    int sc_valid;

    uint32_t vsh_const_off, vsh_prog_off, vsh_prog_hash, vsh_prog_n;
    float vsh_consts[192][4];
    uint32_t gamma[256];
};

static int s_said_full;

/* ---- slots ------------------------------------------------------------ */

static void slot_wait(VkrContext *c, int s, uint32_t want)
{
    uint32_t st;
    while ((st = __atomic_load_n(&c->ctl->slot[s].state, __ATOMIC_ACQUIRE)) != want) {
        if (c->ctl->gpu_failed)
            return;
        emscripten_futex_wait((void *)&c->ctl->slot[s].state, st, 100.0);
    }
}

static void slot_set(VkrContext *c, int s, uint32_t v)
{
    __atomic_store_n(&c->ctl->slot[s].state, v, __ATOMIC_RELEASE);
    emscripten_futex_wake((void *)&c->ctl->slot[s].state, INT_MAX);
}

/* Start recording into the next slot once its previous frame is released. */
static int begin(VkrContext *c)
{
    if (c->cur >= 0)
        return 1;
    if (c->ctl->gpu_failed)
        return 0;
    slot_wait(c, c->next, S_FREE);
    if (c->ctl->gpu_failed)
        return 0;
    c->cur = c->next;
    c->ncmd = 0;
    c->nbytes = 0;
    c->vsh_const_off = c->vsh_prog_off = 0xFFFFFFFFu;
    slot_set(c, c->cur, S_RECORDING);
    c->sc_valid = 0;                    /* the target carries over, the scissor is re-sent */
    return 1;
}

static uint32_t *emit(VkrContext *c, uint32_t op, uint32_t n)
{
    uint32_t *p;
    if (!begin(c))
        return NULL;
    if (c->ncmd + n + 1 > WGR_CMD_WORDS) {
        if (!s_said_full++)
            fprintf(stderr, "[WGR] command buffer full\n");
        return NULL;
    }
    p = c->cmd[c->cur] + c->ncmd;
    p[0] = op | n << 8;
    c->ncmd += n + 1;
    return p + 1;
}

/* Room in the slot's data area: offset of `bytes` at `align`. */
static uint32_t alloc(VkrContext *c, uint32_t bytes, uint32_t align, uint8_t **ptr)
{
    uint32_t off;
    if (!begin(c))
        return 0xFFFFFFFFu;
    off = (c->nbytes + align - 1) & ~(align - 1);
    if (off + bytes > WGR_DATA_BYTES) {
        if (!s_said_full++)
            fprintf(stderr, "[WGR] upload buffer full\n");
        return 0xFFFFFFFFu;
    }
    c->nbytes = off + bytes;
    *ptr = c->data[c->cur] + off;
    return off;
}

/* ---- context ---------------------------------------------------------- */

/* web/gpu.js lives on the page thread: hand it the control block there
 * (synchronously -- that thread is free, main() runs on a worker). */
static int wgr_js_attach(void *ctl)
{
    return MAIN_THREAD_EM_ASM_INT({
        return Module.wgrAttach ? (Module.wgrAttach($0) ? 1 : 0) : 0;
    }, ctl);
}

VkrContext *vkr_create(void)
{
    VkrContext *c;
    const char *r;
    double v;
    int s;

    if (!getenv("RECOMP_WEBGPU")) {
        fprintf(stderr, "[WGR] the page has no WebGPU device\n");
        return NULL;
    }
    c = (VkrContext *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->ctl = (WgrCtl *)aligned_alloc(64, sizeof *c->ctl);
    memset(c->ctl, 0, sizeof *c->ctl);
    c->ctl->magic = 0x31524757u;
    c->ctl->nslots = WGR_SLOTS;
    for (s = 0; s < WGR_SLOTS; s++) {
        c->cmd[s] = (uint32_t *)malloc(WGR_CMD_WORDS * 4);
        c->data[s] = (uint8_t *)aligned_alloc(256, WGR_DATA_BYTES);
        if (!c->cmd[s] || !c->data[s]) {
            fprintf(stderr, "[WGR] out of memory\n");
            return NULL;
        }
        c->ctl->slot[s].cmd_ptr = (uint32_t)(uintptr_t)c->cmd[s];
        c->ctl->slot[s].data_ptr = (uint32_t)(uintptr_t)c->data[s];
    }
    c->cur = -1;
    r = getenv("RECOMP_RES");
    v = r ? atof(r) : 0;
    c->scale = v >= 240 ? (float)(v / 480.0) : (v >= 1.0 && v <= 8.0) ? (float)v : 1.0f;
    r = getenv("RECOMP_ASPECT");
    vkr_set_aspect(c, r ? (float)atof(r) : 0.0f);
    c->tex_id_next = 1;                 /* 0 = gpu.js's white texture */
    /* The page thread attaches synchronously: it is free (PROXY_TO_PTHREAD). */
    if (!wgr_js_attach(c->ctl)) {
        fprintf(stderr, "[WGR] gpu.js did not attach\n");
        return NULL;
    }
    fprintf(stderr, "[WGR] WebGPU renderer, internal scale x%.2f\n", c->scale);
    return c;
}

void vkr_destroy(VkrContext *c)
{
    (void)c;                            /* lives as long as the page */
}

uint32_t vkr_tex_generation(VkrContext *c) { return c->tex_gen; }

/* The live HD-texture switch empties the cache; the web build has no HD
 * textures, so there is nothing to make again. */
void vkr_tex_purge(VkrContext *c) { (void)c; }

void vkr_info(VkrContext *c, uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes)
{
    *w = c->sw;
    *h = c->sh;
    *msaa = 1;
    *tex_bytes = c->tex_bytes;
}

/* ---- target ----------------------------------------------------------- */

static void wait_all_free(VkrContext *c)
{
    int s;
    for (s = 0; s < WGR_SLOTS; s++)
        if (s != c->cur)
            slot_wait(c, s, S_FREE);
}

static void apply_scissor(VkrContext *c);

int vkr_set_target(VkrContext *c, uint32_t w, uint32_t h, float zmax)
{
    uint32_t *p;
    int s;
    if (!begin(c))
        return 0;
    if (w * h > c->rb_w * c->rb_h || !c->rb[0]) {
        /* the readback buffers: frames in flight may still be read */
        wait_all_free(c);
        for (s = 0; s < WGR_SLOTS; s++) {
            free(c->rb[s]);
            c->rb[s] = (uint8_t *)malloc((size_t)w * h * 4);
        }
        c->rb_w = w; c->rb_h = h;
    }
    c->w = w; c->h = h; c->zmax = zmax;
    c->sw = (uint32_t)((float)w * c->scale_x + 0.5f);
    c->sh = (uint32_t)((float)h * c->scale + 0.5f);
    c->gsc[0] = c->gsc[1] = 0; c->gsc[2] = w; c->gsc[3] = h;
    p = emit(c, WGR_TARGET, 4);
    if (!p)
        return 0;
    p[0] = w; p[1] = h; p[2] = c->sw; p[3] = c->sh;
    c->sc_valid = 0;
    apply_scissor(c);
    if (c->scale != 1.0f || c->scale_x != 1.0f)
        fprintf(stderr, "[WGR] internal resolution %ux%u (x%.2f/x%.2f of %ux%u)\n",
                c->sw, c->sh, c->scale_x, c->scale, w, h);
    return 1;
}

static void apply_scissor(VkrContext *c)
{
    uint32_t x = c->gsc[0], y = c->gsc[1], w = c->gsc[2], h = c->gsc[3], *p;
    if (x >= c->w || y >= c->h) { x = y = 0; w = h = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (c->scale != 1.0f || c->scale_x != 1.0f) {
        float sx = c->squeeze_all > 0.5f ? c->scale : c->scale_x;
        float ox = c->squeeze_all > 0.5f ? ((float)c->sw - (float)c->w * c->scale) * 0.5f : 0.0f;
        uint32_t x1 = (uint32_t)(ox + (float)(x + w) * sx + 0.5f);
        uint32_t y1 = (uint32_t)((float)(y + h) * c->scale + 0.5f);
        x = (uint32_t)(ox + (float)x * sx + 0.5f);
        y = (uint32_t)((float)y * c->scale + 0.5f);
        if (x1 > c->sw) x1 = c->sw;
        if (y1 > c->sh) y1 = c->sh;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    if (c->sc_valid && c->sc[0] == x && c->sc[1] == y && c->sc[2] == w && c->sc[3] == h)
        return;
    c->sc[0] = x; c->sc[1] = y; c->sc[2] = w; c->sc[3] = h;
    c->sc_valid = 0;                    /* recorded lazily, in the slot that draws */
}

/* The scissor goes into the stream right before what uses it, so a new slot
 * always gets one. */
static int sync_scissor(VkrContext *c)
{
    uint32_t *p;
    if (c->sc_valid)
        return 1;
    p = emit(c, WGR_SCISSOR, 4);
    if (!p)
        return 0;
    memcpy(p, c->sc, 16);
    c->sc_valid = 1;
    return 1;
}

void vkr_set_scissor(VkrContext *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c->gsc[0] = x; c->gsc[1] = y; c->gsc[2] = w; c->gsc[3] = h;
    apply_scissor(c);
}

static void clear(VkrContext *c, const float rgba[4], uint32_t flags, float depth,
                  uint32_t stencil)
{
    WgrDraw *d;
    uint8_t *ptr;
    uint32_t off, *p;
    if (!begin(c) || !sync_scissor(c))
        return;
    off = alloc(c, WGR_DRAW_BYTES, 256, &ptr);
    if (off == 0xFFFFFFFFu)
        return;
    d = (WgrDraw *)ptr;
    memset(d, 0, sizeof *d);
    if (rgba)
        memcpy(d->u.fog_color, rgba, 16);
    d->u.fog_param[0] = depth;
    p = emit(c, WGR_CLEAR, 3);
    if (!p)
        return;
    p[0] = off; p[1] = flags; p[2] = stencil & 0xFF;
}

void vkr_clear(VkrContext *c, const float rgba[4], int clear_depth, float depth)
{
    clear(c, rgba, 1u | (clear_depth ? 2u : 0u), depth, 0);
}

void vkr_clear_ds(VkrContext *c, int clear_depth, float depth, int clear_stencil, uint32_t stencil)
{
    uint32_t flags = (clear_depth ? 2u : 0u) | (clear_stencil ? 4u : 0u);
    if (flags)
        clear(c, NULL, flags, depth, stencil);
}

/* ---- textures --------------------------------------------------------- */

static uint32_t tex_slot(uint64_t key, uint32_t cap)
{
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
    return (uint32_t)key & (cap - 1);
}

static int tex_rehash(VkrContext *c)
{
    uint32_t cap = 1024, i;
    while (cap < c->ntexs * 2 + 2)
        cap <<= 1;
    free(c->hidx);
    c->hidx = (int32_t *)malloc(cap * sizeof *c->hidx);
    if (!c->hidx) { c->hcap = 0; return 0; }
    c->hcap = cap;
    for (i = 0; i < cap; i++)
        c->hidx[i] = -1;
    for (i = 0; i < c->ntexs; i++) {
        uint32_t h = tex_slot(c->texs[i]->key, cap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (cap - 1);
        c->hidx[h] = (int32_t)i;
    }
    return 1;
}

static void tex_free_one(VkrContext *c, VkrTex *t)
{
    uint32_t *p = emit(c, WGR_TEX_FREE, 1);
    if (p)
        p[0] = t->id;
    c->tex_bytes -= (uint64_t)t->w * t->h * 4;
    free(t);
}

static void tex_evict(VkrContext *c, uint32_t age)
{
    uint32_t i, k = 0, freed = 0;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        if (age == 0 || c->flip_serial - t->last_use > age) {
            tex_free_one(c, t);
            freed++;
        } else
            c->texs[k++] = t;
    }
    if (!freed)
        return;
    c->ntexs = k;
    c->tex_gen++;
    tex_rehash(c);
}

VkrTex *vkr_tex_find(VkrContext *c, uint64_t key)
{
    uint32_t h;
    if (!c->hcap)
        return NULL;
    for (h = tex_slot(key, c->hcap); c->hidx[h] >= 0; h = (h + 1) & (c->hcap - 1)) {
        VkrTex *t = c->texs[c->hidx[h]];
        if (t->key == key) {
            t->last_use = c->flip_serial;
            return t;
        }
    }
    return NULL;
}

/* Pixels into the data area with rows padded to 256 bytes (WebGPU's
 * buffer-to-texture copy needs that). */
static uint32_t upload_pixels(VkrContext *c, uint32_t w, uint32_t h, const uint32_t *px,
                              uint32_t *bpr_out)
{
    uint32_t bpr = (w * 4 + 255) & ~255u, y, off;
    uint8_t *dst;
    off = alloc(c, bpr * h, 256, &dst);
    if (off == 0xFFFFFFFFu)
        return off;
    if (bpr == w * 4)
        memcpy(dst, px, (size_t)w * h * 4);
    else
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * bpr, px + (size_t)y * w, (size_t)w * 4);
    *bpr_out = bpr;
    return off;
}

int vkr_tex_update(VkrContext *c, VkrTex *t, uint64_t key, const uint32_t *pixels)
{
    uint32_t bpr, off = upload_pixels(c, t->w, t->h, pixels, &bpr), *p;
    if (off == 0xFFFFFFFFu)
        return 0;
    p = emit(c, WGR_TEX_UPDATE, 3);
    if (!p)
        return 0;
    p[0] = t->id; p[1] = off; p[2] = bpr;
    t->last_use = c->flip_serial;
    if (t->key != key) {
        t->key = key;
        tex_rehash(c);
    }
    return 1;
}

VkrTex *vkr_tex_create(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                       const uint32_t *pixels)
{
    VkrTex *t;
    uint32_t bpr, off, *p;
    if (!w || !h || w > 8192 || h > 8192)
        return NULL;
    off = upload_pixels(c, w, h, pixels, &bpr);
    if (off == 0xFFFFFFFFu)
        return NULL;
    p = emit(c, WGR_TEX_CREATE, 5);
    if (!p)
        return NULL;
    t = (VkrTex *)calloc(1, sizeof *t);
    t->key = key; t->w = w; t->h = h; t->id = c->tex_id_next++;
    t->last_use = c->flip_serial;
    p[0] = t->id; p[1] = w; p[2] = h; p[3] = off; p[4] = bpr;
    if (c->ntexs == c->captexs) {
        c->captexs = c->captexs ? c->captexs * 2 : 256;
        c->texs = (VkrTex **)realloc(c->texs, c->captexs * sizeof *c->texs);
    }
    c->texs[c->ntexs++] = t;
    c->tex_bytes += (uint64_t)w * h * 4;
    if (c->ntexs * 2 + 2 > c->hcap)
        tex_rehash(c);
    else {
        uint32_t hh = tex_slot(key, c->hcap);
        while (c->hidx[hh] >= 0)
            hh = (hh + 1) & (c->hcap - 1);
        c->hidx[hh] = (int32_t)(c->ntexs - 1);
    }
    return t;
}

/* ---- draws ------------------------------------------------------------ */

static void fill_draw(VkrContext *c, WgrDraw *d, const VkrDrawState *st)
{
    int k;
    memcpy(&d->u, &st->u, sizeof d->u);
    d->inv_size[0] = 1.0f / (float)c->w; d->inv_size[1] = 1.0f / (float)c->h;
    d->inv_zmax = 1.0f / c->zmax;
    d->fog_mode = st->u.fog_enable ? st->u.fog_param[2] : 0.0f;
    d->fogp[0] = st->u.fog_param[0]; d->fogp[1] = st->u.fog_param[1];
    d->point_size = st->point_size * c->scale;
    d->point_params = st->point_params ? c->scale : 0.0f;
    d->squeeze2d = c->squeeze2d; d->squeeze_all = c->squeeze_all;
    d->topo = st->topology;
    d->line_w = c->scale > 1.0f ? c->scale : 1.0f;
    d->alpha_func = st->alpha_func; d->alpha_ref = st->alpha_ref;
    d->w_scale = st->w_buffer ? 1.0f / c->zmax : 0.0f;
    d->point_sprite = st->topology == VKR_TOPO_POINTS && st->point_sprite;
    for (k = 0; k < 4; k++)
        if (st->addr_u[k] == 4 || st->addr_v[k] == 4)
            d->border |= 1u << k;
    d->tsize[0] = (float)c->sw; d->tsize[1] = (float)c->sh;
}

static uint32_t samp_key(const VkrDrawState *st, int k)
{
    return (st->addr_u[k] & 7) | (st->addr_v[k] & 7) << 3 | (uint32_t)(st->linear[k] != 0) << 6;
}

/* NV2A stencil op (GL enum) -> gpu.js STENCIL_OP index. */
static uint32_t stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return 1;              /* zero */
    case 0x1E01: return 2;              /* replace */
    case 0x1E02: return 3;              /* increment-clamp */
    case 0x1E03: return 4;              /* decrement-clamp */
    case 0x150A: return 5;              /* invert */
    case 0x8507: return 6;              /* increment-wrap */
    case 0x8508: return 7;              /* decrement-wrap */
    default:     return 0;              /* keep */
    }
}

static int emit_draw(VkrContext *c, uint32_t param_off, uint32_t nidx, const VkrDrawState *st)
{
    uint32_t *p, nverts, k;
    if (!sync_scissor(c))
        return 0;
    nverts = st->topology == VKR_TOPO_POINTS ? nidx * 6
           : st->topology == VKR_TOPO_LINES ? (nidx / 2) * 6 : nidx;
    if (!nverts)
        return 1;
    p = emit(c, WGR_DRAW, 19);
    if (!p)
        return 0;
    p[0] = param_off; p[1] = nverts;
    /* key0: blend enable, eq, mask, depth test/write/func, w-buffer */
    p[2] = (uint32_t)(st->blend_enable != 0) | (st->write_mask & 0xF) << 1
         | (uint32_t)(st->depth_test != 0) << 5 | (uint32_t)(st->depth_write != 0) << 6
         | ((st->depth_func >= 0x200 && st->depth_func <= 0x207 ? st->depth_func - 0x200 : 3) << 7)
         | (uint32_t)(st->w_buffer != 0) << 10
         | (st->blend_enable ? (st->blend_eq & 0xFFFF) << 16 : 0);
    /* key1: blend factors */
    p[3] = st->blend_enable ? (st->blend_src & 0xFFFF) | (st->blend_dst & 0xFFFF) << 16 : 0;
    for (k = 0; k < 4; k++) {
        p[4 + k] = st->tex[k] ? st->tex[k]->id : 0;
        p[8 + k] = samp_key(st, (int)k);
    }
    memcpy(&p[12], st->blend_color, 16);
    /* stencil: bit 0 test, 1-3 func, 4-6 fail, 7-9 zfail, 10-12 zpass */
    if (st->stencil_test) {
        p[16] = 1u | (st->stencil_func >= 0x200 && st->stencil_func <= 0x207 ? st->stencil_func - 0x200 : 7) << 1
              | stencil_op(st->stencil_fail) << 4 | stencil_op(st->stencil_zfail) << 7
              | stencil_op(st->stencil_zpass) << 10;
        p[17] = (st->stencil_read_mask & 0xFF) | (st->stencil_write_mask & 0xFF) << 8;
        p[18] = st->stencil_ref & 0xFF;
    } else {
        p[16] = p[17] = p[18] = 0;
    }
    return 1;
}

int vkr_draw(VkrContext *c, const VkrVertex *v, uint32_t nverts,
             const uint32_t *idx, uint32_t nidx, const VkrDrawState *st)
{
    uint8_t *pv, *pi, *pd;
    uint32_t vo, io, po;
    WgrDraw *d;
    if (!begin(c))
        return 0;
    vo = alloc(c, nverts * (uint32_t)sizeof *v, 16, &pv);
    io = alloc(c, nidx * 4, 16, &pi);
    po = alloc(c, WGR_DRAW_BYTES, 256, &pd);
    if (vo == 0xFFFFFFFFu || io == 0xFFFFFFFFu || po == 0xFFFFFFFFu)
        return 0;
    memcpy(pv, v, nverts * sizeof *v);
    memcpy(pi, idx, nidx * 4);
    d = (WgrDraw *)pd;
    memset(d, 0, sizeof *d);
    fill_draw(c, d, st);
    d->vsh = 0;
    d->vtx_base = vo / 16;
    d->idx_base = io / 4;
    return emit_draw(c, po, nidx, st);
}

int vkr_draw_vsh(VkrContext *c, const float *in, uint32_t nverts, uint32_t nin,
                 const uint32_t *prog, uint32_t nins, uint32_t prog_hash,
                 const float (*consts)[4], const uint32_t *idx, uint32_t nidx,
                 const VkrDrawState *st)
{
    uint8_t *pp, *pv, *pi, *pd;
    uint32_t in_bytes = nverts * (nin ? nin : 1) * 16, vo, io, po;
    WgrDraw *d;
    if (!begin(c))
        return 0;
    if (c->vsh_const_off == 0xFFFFFFFFu || memcmp(c->vsh_consts, consts, sizeof c->vsh_consts)) {
        uint32_t o = alloc(c, (uint32_t)sizeof c->vsh_consts, 16, &pp);
        if (o == 0xFFFFFFFFu)
            return 0;
        memcpy(pp, consts, sizeof c->vsh_consts);
        memcpy(c->vsh_consts, consts, sizeof c->vsh_consts);
        c->vsh_const_off = o;
    }
    if (c->vsh_prog_off == 0xFFFFFFFFu || c->vsh_prog_hash != prog_hash || c->vsh_prog_n != nins) {
        uint32_t o = alloc(c, nins * 32, 16, &pp);
        if (o == 0xFFFFFFFFu)
            return 0;
        memcpy(pp, prog, nins * 32);
        c->vsh_prog_off = o;
        c->vsh_prog_hash = prog_hash;
        c->vsh_prog_n = nins;
    }
    vo = alloc(c, in_bytes, 16, &pv);
    io = alloc(c, nidx * 4, 16, &pi);
    po = alloc(c, WGR_DRAW_BYTES, 256, &pd);
    if (vo == 0xFFFFFFFFu || io == 0xFFFFFFFFu || po == 0xFFFFFFFFu)
        return 0;
    memcpy(pv, in, in_bytes);
    memcpy(pi, idx, nidx * 4);
    d = (WgrDraw *)pd;
    memset(d, 0, sizeof *d);
    fill_draw(c, d, st);
    d->vsh = 1;
    d->in_base = vo / 16;
    d->nin = nin ? nin : 1;
    d->prog_base = c->vsh_prog_off / 16;
    d->const_base = c->vsh_const_off / 16;
    d->idx_base = io / 4;
    return emit_draw(c, po, nidx, st);
}

void vkr_set_pillarbox(VkrContext *c, int on)
{
    float v = on && c->squeeze2d != 1.0f ? 1.0f : 0.0f;
    if (v != c->squeeze_all) {
        c->squeeze_all = v;
        apply_scissor(c);
    }
}

void vkr_set_aspect(VkrContext *c, float aspect)
{
    c->scale_x = c->scale;
    c->squeeze2d = 1.0f;
    if (aspect > 4.0f / 3.0f + 0.01f) {
        float f = aspect * 0.75f;
        c->scale_x = c->scale * f;
        c->squeeze2d = 1.0f / f;
    }
}

uint32_t vkr_width(const VkrContext *c) { return c->w; }
uint32_t vkr_height(const VkrContext *c) { return c->h; }

/* ---- frames ----------------------------------------------------------- */

extern unsigned xbox_FramebufferGammaTexels(uint32_t *out256);
extern unsigned xbox_FramebufferGammaSerial(void);

int vkr_flip_submit(VkrContext *c)
{
    uint32_t *p, bars = 0, gserial;
    int slot;
    if (!begin(c) || !c->w)
        return -1;
    if (c->squeeze_all > 0.5f)
        bars = (uint32_t)(((float)c->sw - (float)c->w * c->scale) * 0.5f);
    if (bars) {
        /* Pillarboxed frame: the bars are black (nothing draws there). */
        static const float black[4] = {0, 0, 0, 1};
        uint32_t save[4];
        memcpy(save, c->sc, sizeof save);
        c->sc[0] = 0; c->sc[1] = 0; c->sc[2] = bars; c->sc[3] = c->sh;
        c->sc_valid = 0;
        clear(c, black, 1u, 0.0f, 0);
        c->sc[0] = c->sw - bars;
        c->sc_valid = 0;
        clear(c, black, 1u, 0.0f, 0);
        memcpy(c->sc, save, sizeof save);
        c->sc_valid = 0;
    }
    gserial = xbox_FramebufferGammaSerial();
    xbox_FramebufferGammaTexels(c->gamma);
    p = emit(c, WGR_FLIP, 6);
    if (!p)
        return -1;
    p[0] = (uint32_t)(uintptr_t)c->rb[c->cur]; p[1] = c->w; p[2] = c->h;
    p[3] = 0; p[4] = (uint32_t)(uintptr_t)c->gamma; p[5] = gserial;
    slot = c->cur;
    if (c->frame < 6) {
        char m[96];
        snprintf(m, sizeof m, "[WGR] flip %u slot %d: %u words, %u bytes", c->frame, slot, c->ncmd, c->nbytes);
        emscripten_console_log(m);
    }
    c->ctl->slot[slot].cmd_words = c->ncmd;
    c->ctl->slot[slot].data_bytes = c->nbytes;
    c->ctl->slot[slot].frame = ++c->frame;
    slot_set(c, slot, S_SUBMITTED);
    __atomic_add_fetch(&c->ctl->submits, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake((void *)&c->ctl->submits, INT_MAX);
    c->cur = -1;
    c->next = (slot + 1) % WGR_SLOTS;
    if (++c->flip_serial % 120 == 0)
        tex_evict(c, 300);
    if (c->tex_bytes > (768ull << 20))
        tex_evict(c, 0);
    return slot;
}

const uint8_t *vkr_frame_pixels(VkrContext *c, int slot)
{
    slot_wait(c, slot, S_DONE);
    return c->ctl->gpu_failed ? NULL : c->rb[slot];
}

/* gpu.js shows the full-resolution image itself. */
const uint8_t *vkr_frame_pixels_full(VkrContext *c, int slot, uint32_t *w, uint32_t *h)
{
    (void)c; (void)slot; (void)w; (void)h;
    return NULL;
}

void vkr_frame_release(VkrContext *c, int slot)
{
    slot_set(c, slot, S_FREE);
}

int vkr_read_back(VkrContext *c, uint8_t *out)
{
    int slot = vkr_flip_submit(c);
    const uint8_t *px = slot < 0 ? NULL : vkr_frame_pixels(c, slot);
    if (px)
        memcpy(out, px, (size_t)c->w * c->h * 4);
    if (slot >= 0)
        vkr_frame_release(c, slot);
    return px != NULL;
}
#endif /* __EMSCRIPTEN__ */
