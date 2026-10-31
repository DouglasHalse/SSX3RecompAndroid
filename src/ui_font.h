/*
 * ui_font -- proportional text for a screen the port draws itself.
 *
 * The glyphs come from src/ui_font_data.h (tools/gen_ui_font.py renders a
 * system font into trimmed alpha bitmaps); a requested pixel size picks the
 * closer of the two faces and scales the samples, so the same menu is legible
 * over a 480p frame and a 4K one without shipping a rasteriser.
 */
#ifndef SSX3_UI_FONT_H
#define SSX3_UI_FONT_H

#include <stdint.h>

/* Width of `s` at `size` px, in pixels. */
int ui_text_w(int size, const char *s);

/* Height of a line box at `size` px. */
int ui_text_h(int size);

/* Draw `s` with its top-left at (x, y); `alpha` scales the glyph coverage.
 * Returns the advance width. */
int ui_text(uint32_t *px, int w, int h, int x, int y, int size,
            const char *s, uint32_t color, int alpha);

#endif /* SSX3_UI_FONT_H */
