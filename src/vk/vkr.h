/* Vulkan renderer for the NV2A executor: context, render targets, draws. */
#ifndef SSX3_VKR_H
#define SSX3_VKR_H

#include <stddef.h>
#include <stdint.h>

/* One vertex after the vertex program (mirrors Nv2aVshOutput). */
typedef struct {
    float pos[4];           /* x, y pixels; z depth-buffer units; w clip w */
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} VkrVertex;

typedef struct VkrContext VkrContext;
typedef struct VkrTex VkrTex;

/* Combiner + fog + per-stage state, uploaded as a uniform buffer. Layout is
 * std140: eight-dword arrays are two uvec4s. Mirrors Nv2aCombiner. */
typedef struct {
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8];
    uint32_t final0, final1, final_c0, final_c1;
    uint32_t control, stage_program, rc_seen, fog_enable;
    float    fog_color[4];          /* r, g, b already in order */
    float    tex_scale[4][4];       /* per stage: xy multiplier to normalised coords */
    float    fog_param[4];          /* x, y; z = mode (as float), w unused */
} VkrUniforms;

/* Per-draw state. GL enums as the NV2A takes them. */
typedef struct {
    int      depth_test, depth_write;
    uint32_t depth_func;            /* 0x200 NEVER .. 0x207 ALWAYS */
    int      w_buffer;              /* depth = clip w (W-buffering), not z */
    int      blend_enable;
    uint32_t blend_src, blend_dst;  /* GL blend factors */
    uint32_t blend_eq;              /* 0x8006 ADD, 0x800A SUB, 0x800B RSUB, 0x8007 MIN, 0x8008 MAX */
    float    blend_color[4];
    uint32_t write_mask;            /* bit0 R, bit1 G, bit2 B, bit3 A */
    uint32_t alpha_func;            /* 0 = alpha test off, else 0x200..0x207 */
    uint32_t alpha_ref;             /* 0..255 */
    VkrUniforms u;
    VkrTex  *tex[4];                /* NULL = white */
    uint32_t addr_u[4], addr_v[4];  /* 1 wrap 2 mirror 3 clamp 4 border 5 clamp-to-edge */
    int      linear[4];
    uint32_t topology;              /* VKR_TOPO_*: what the index list holds */
    float    point_size;            /* pixels, when !point_params */
    int      point_params;          /* size from the program's oPts.x */
    int      point_sprite;          /* stage 3 coordinates = gl_PointCoord */
    float    hud_shift;             /* NDC x offset of 2D vertices (HUD anchored to an edge) */
    int      keep_edges;            /* 2D vertices on the screen edges stay there (full-width passes) */
    int      clip2d;                /* discard outside the guest screen (2D, widescreen) */
    /* Stencil (NV2A STENCIL_*; func GL enum 0x200..0x207, ops GL enums) */
    int      stencil_test;
    uint32_t stencil_func, stencil_ref, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
    int      rider;                 /* RECOMP_MBLUR tag: 255 player 1 (sharp), 1..7 another rider's slot, 0 none */
} VkrDrawState;

#define VKR_TOPO_TRIANGLES 0
#define VKR_TOPO_LINES     1
#define VKR_TOPO_POINTS    2

/* Headless device (no window). Returns NULL and prints why on failure. */
VkrContext *vkr_create(void);
void vkr_destroy(VkrContext *c);

/* Colour (BGRA8) + depth target; replaces any previous one. */
int vkr_set_target(VkrContext *c, uint32_t w, uint32_t h, float zmax);
void vkr_set_scissor(VkrContext *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vkr_clear(VkrContext *c, const float rgba[4], int clear_depth, float depth);
/* Depth and/or stencil only (colour untouched). */
void vkr_clear_ds(VkrContext *c, int clear_depth, float depth, int clear_stencil, uint32_t stencil);

/* Textures, keyed by a content hash. pixels: w*h dwords, A8R8G8B8. */
VkrTex *vkr_tex_find(VkrContext *c, uint64_t key);
VkrTex *vkr_tex_create(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                       const uint32_t *pixels);
/* mips: build the mip chain too (not for textures later vkr_tex_update'd). */
VkrTex *vkr_tex_create_ex(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                          const uint32_t *pixels, int mips);
int vkr_tex_update(VkrContext *c, VkrTex *t, uint64_t key, const uint32_t *pixels);
int vkr_has_bc7(VkrContext *c);
/* Offscreen GPU targets keyed by the guest surface address (shadow maps):
 * make one current (w x h guest pixels, drawn at scale), go back to the
 * main target, or get its image to sample (NULL if never drawn). */
int     vkr_offscreen_use(VkrContext *c, uint32_t key, uint32_t w, uint32_t h, float scale, float zmax);
void    vkr_offscreen_done(VkrContext *c);
VkrTex *vkr_offscreen_tex(VkrContext *c, uint32_t key);
int     vkr_offscreen_size(VkrContext *c, uint32_t key, uint32_t *w, uint32_t *h);
/* BC7 with mips (levels back to back, as in a DDS); NULL if not possible now. */
#define VKR_TEX_ALPHA2 1u                /* alpha on the PS2 scale: x2 when sampled */
VkrTex *vkr_tex_create_bc7(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                           uint32_t mips, const void *data, size_t size, uint32_t flags);
uint32_t vkr_tex_flags(const VkrTex *t);
uint32_t vkr_tex_generation(VkrContext *c);   /* changes when all textures were freed */
void     vkr_tex_purge(VkrContext *c);          /* free every texture at the next flip */
/* the main target's size and MSAA samples, the texture cache's bytes */
void     vkr_info(VkrContext *c, uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes);

/* Draw an indexed triangle, line or point list (st->topology). */
int vkr_draw(VkrContext *c, const VkrVertex *v, uint32_t nverts,
             const uint32_t *idx, uint32_t nidx, const VkrDrawState *st);

/* The same with the NV2A vertex program run on the GPU: in is nverts x nin
 * float4 program inputs, prog nins x 8 words (toolkit nv2a_vsh_export),
 * consts the 192 constants (float[192][4]). */
int vkr_draw_vsh(VkrContext *c, const float *in, uint32_t nverts, uint32_t nin,
                 const uint32_t *prog, uint32_t nins, uint32_t prog_hash,
                 const float (*consts)[4], const uint32_t *idx, uint32_t nidx,
                 const VkrDrawState *st);

/* The scene camera for the frame being recorded (RECOMP_MBLUR): inv_m maps
 * (guest pixel x, y, depth units - zmax, 1) to eye-relative world positions
 * (homogeneous), prev_m eye-relative positions to last frame's (x w, y w,
 * z w, w); column-major. Valid until the flip. */
typedef struct {
    float inv_m[16], prev_m[16];
    float foe[2];       /* where the camera is heading, guest pixels */
    float speed;        /* the camera's speed, world units per frame (0: no heading) */
    int off;            /* no scene pass this frame (RECOMP_MBLUR_TEST reference shots) */
    float rider_v[8][2];  /* rider slot k's motion on screen, guest pixels (tag k; 0 unused) */
} VkrScene;
void vkr_scene_set(VkrContext *c, const VkrScene *s);
/* Run the scene pass now (before the HUD); once per frame, after vkr_scene_set. */
void vkr_scene_run(VkrContext *c);

/* RECOMP_WIDESCREEN target: 1 = show the 4:3 picture pillarboxed (menus). */
void vkr_set_pillarbox(VkrContext *c, int on);
/* Target shape, width / height (> 4:3 = widescreen); applies at the next
 * vkr_set_target. */
void vkr_set_aspect(VkrContext *c, float aspect);
uint32_t vkr_width(const VkrContext *c);
uint32_t vkr_height(const VkrContext *c);

/* Submit, wait, and copy the colour target to out (w*h*4 bytes, B8G8R8A8,
 * i.e. the Xbox's little-endian A8R8G8B8). */
int vkr_read_back(VkrContext *c, uint8_t *out);

/* The same without waiting: end the frame and submit it with a copy of the
 * colour target, and go on recording the next. Returns the frame's slot (-1
 * on failure). vkr_frame_pixels waits for it and returns the copy (w*h*4);
 * vkr_frame_release hands the slot back -- recording blocks until it is. Any
 * thread may wait for and release a slot. */
int vkr_flip_submit(VkrContext *c);
const uint8_t *vkr_frame_pixels(VkrContext *c, int slot);
void vkr_frame_release(VkrContext *c, int slot);
/* With RECOMP_RES above the guest's: the full-resolution image of the frame
 * (after vkr_frame_pixels), else NULL. */
const uint8_t *vkr_frame_pixels_full(VkrContext *c, int slot, uint32_t *w, uint32_t *h);

#endif
