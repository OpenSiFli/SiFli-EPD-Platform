#ifndef UI_SYSTEM_FONT_CACHE_H
#define UI_SYSTEM_FONT_CACHE_H

#include "lvgl.h"

/* Immutable system TinyTTF fonts use cache_size=0 and share this byte budget.
 * Cached pixels, metrics and cache allocation overhead are included. */
#define UI_SYSTEM_FONT_CACHE_BYTES (2u * 1024u * 1024u)
bool ui_system_font_cache_attach(lv_font_t *font);
/* UI thread. Lightweight immutable wrapper; normal must already be attached.
 * Uses UI_FONT_STYLE_* flags and the same byte cache. Never owns normal or its
 * fallback. Destroy wrappers before destroying their normal face. */
lv_font_t *ui_system_font_cache_style_create(const lv_font_t *normal, unsigned flags);
void ui_system_font_cache_style_destroy(lv_font_t *font);
/* UI thread only, after removing objects that use this font. */
void ui_system_font_cache_detach(lv_font_t *font);

#endif
