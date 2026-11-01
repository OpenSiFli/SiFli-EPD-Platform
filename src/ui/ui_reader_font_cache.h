#ifndef UI_READER_FONT_CACHE_H
#define UI_READER_FONT_CACHE_H
#include "lvgl.h"
#include "platform/epd_app_font.h"

/* UI-thread ownership; generated bitmaps retain LVGL cache references.
 * Cache hits do not wait for TinyTTF source callbacks. Misses serialize access
 * to mutable streams; descriptor storage is capped at 1 MiB including roots.
 * Attaching a face may reclaim optional descriptor pages to fit its root,
 * preserving copied descriptors and retained pixels. The shared PSRAM reserve
 * remains 1 MiB. Attach drains drawing before locking callbacks then metadata. */
bool ui_reader_font_cache_attach(lv_font_t *font);
/* Invalidates this source's native/derived pixels; other bindings stay warm. */
void ui_reader_font_cache_detach(lv_font_t *font);
/* UI thread only: reclaim speculative pixels before a large shared allocation. */
void ui_reader_font_cache_reserve(size_t bytes);
/* UI thread, after drawing has drained; panel refresh may continue.
 * Pass the actual draw font, including a derived wrapper. Whole-book scans
 * fill spare room without refreshing nearby glyphs or evicting existing ones.
 * False also reports a wrapper that did not retain its generated pixels. */
bool ui_reader_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
/* Same snapshot semantics as epd_app_font_cache_stats; safe before first attach. */
void ui_reader_font_cache_stats(epd_app_font_cache_stats_t *out);
/* Shared pixel LRU. A successful acquire/store returns one owned reference.
 * store copies an A8 bitmap; a miss/allocation failure returns NULL and leaves
 * the caller's bitmap untouched. Callable from UI preparation/draw callbacks. */
const lv_draw_buf_t *ui_reader_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                  uint32_t style, lv_cache_entry_t **entry);
const lv_draw_buf_t *ui_reader_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, const lv_draw_buf_t *bitmap,
                                                lv_cache_entry_t **entry);
void ui_reader_font_derived_release(lv_cache_entry_t *entry);
/* UI thread only: wait for drawing, then invalidate this wrapper's pixels. */
void ui_reader_font_derived_drop(const lv_font_t *font);
#endif
