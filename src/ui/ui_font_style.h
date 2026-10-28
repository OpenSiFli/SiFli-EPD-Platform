#ifndef UI_FONT_STYLE_H
#define UI_FONT_STYLE_H

#include "lvgl.h"

#define UI_FONT_STYLE_BOLD 1u
#define UI_FONT_STYLE_ITALIC 2u

/* Synthetic A8 styles. Zero-advance and empty glyph metrics remain unchanged.
 * Both functions use the ordinary font's line height and unstyled dimensions. */
void ui_font_style_metrics(lv_font_glyph_dsc_t *glyph, int32_t line_height, unsigned flags);
/* Returns independent, aligned pixels using LVGL's font-buffer handlers.
 * NULL on unsupported format/flags or allocation failure. The input is never
 * modified. Pass flags=0 for zero-advance glyphs. Release with lv_draw_buf_destroy. */
lv_draw_buf_t *ui_font_style_bitmap(const lv_draw_buf_t *source, int32_t line_height, unsigned flags);

#endif
