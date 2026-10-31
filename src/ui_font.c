/*
 * ui_font.c -- see ui_font.h. Nearest-sample scaling: the menu is a flat
 * surface with one colour per pixel, so a filter would only cost time.
 */
#include "ui_font.h"
#include "ui_font_data.h"

static const ui_face *face_for(int size, float *scale)
{
    const ui_face *f = &s_faces[0];
    int i;
    for (i = 0; i < (int)(sizeof s_faces / sizeof s_faces[0]); i++) {
        if (s_faces[i].px <= size)
            f = &s_faces[i];
    }
    if (size <= 0)
        size = f->px;
    *scale = (float)size / (float)f->px;
    return f;
}

int ui_text_h(int size)
{
    float k;
    const ui_face *f = face_for(size, &k);
    return (int)((f->ascent + f->descent) * k + 0.5f);
}

int ui_text_w(int size, const char *s)
{
    float k;
    const ui_face *f = face_for(size, &k);
    int total = 0;
    for (; s && *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < UI_FONT_FIRST || c >= UI_FONT_FIRST + UI_FONT_COUNT)
            continue;
        total += (int)(f->glyphs[c - UI_FONT_FIRST].advance * k + 0.5f);
    }
    return total;
}

int ui_text(uint32_t *px, int w, int h, int x, int y, int size,
            const char *s, uint32_t color, int alpha)
{
    float k;
    const ui_face *f = face_for(size, &k);
    int pen = x, start = x;
    unsigned si;

    for (; s && *s; s++) {
        const ui_glyph *g;
        int gw, gh, dx, dy, adv;
        unsigned c = (unsigned char)*s;
        if (c < UI_FONT_FIRST || c >= UI_FONT_FIRST + UI_FONT_COUNT) {
            pen += (int)((f->px / 3) * k);
            continue;
        }
        g = &f->glyphs[c - UI_FONT_FIRST];
        adv = (int)(g->advance * k + 0.5f);
        gw = (int)(g->w * k + 0.5f);
        gh = (int)(g->h * k + 0.5f);
        dx = pen + (int)(g->bx * k + 0.5f);
        dy = y + (int)(g->by * k + 0.5f);
        for (int row = 0; row < gh; row++) {
            int sy = dy + row;
            int src_row;
            if (sy < 0 || sy >= h)
                continue;
            src_row = (int)(row / k);
            if (src_row >= g->h)
                src_row = g->h - 1;
            for (int col = 0; col < gw; col++) {
                int sx = dx + col;
                int src_col;
                unsigned a;
                if (sx < 0 || sx >= w)
                    continue;
                src_col = (int)(col / k);
                if (src_col >= g->w)
                    src_col = g->w - 1;
                a = f->pixels[g->off + src_row * g->w + src_col];
                if (!a)
                    continue;
                a = a * (unsigned)alpha / 255u;
                {
                    uint32_t *dst = &px[(size_t)sy * (size_t)w + (size_t)sx];
                    unsigned dr = (*dst >> 16) & 0xFF, dg = (*dst >> 8) & 0xFF, db = *dst & 0xFF;
                    unsigned sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
                    dr = (sr * a + dr * (255 - a)) / 255;
                    dg = (sg * a + dg * (255 - a)) / 255;
                    db = (sb * a + db * (255 - a)) / 255;
                    *dst = (dr << 16) | (dg << 8) | db;
                }
            }
        }
        pen += adv;
    }
    (void)si;
    return pen - start;
}
