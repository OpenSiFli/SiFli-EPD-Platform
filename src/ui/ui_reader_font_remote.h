/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UI_READER_FONT_REMOTE_H
#define UI_READER_FONT_REMOTE_H
#include "platform/epd_app_font.h"

bool ui_reader_font_remote_can_create(unsigned count);
lv_font_t *ui_reader_font_remote_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity);
lv_font_t *ui_reader_font_remote_oblique(const lv_font_t *font);
bool ui_reader_font_remote_owns(const lv_font_t *font);
bool ui_reader_font_remote_ref(const lv_font_t *font);
void ui_reader_font_remote_destroy(lv_font_t *font);
void ui_reader_font_remote_cancel(const lv_font_t *font);
void ui_reader_font_remote_revoke(uint32_t id, bool storage);
/* UI-only memory gate; waits for file ownership, not retired ACPU memory. */
bool ui_reader_font_remote_release_storage(uint32_t release_id);
void ui_reader_font_remote_resume_cleanup(void);
int ui_reader_font_remote_status(const lv_font_t *font);
int ui_reader_font_remote_metrics(const lv_font_t *font, epd_app_font_metrics_t *out);
int ui_reader_font_remote_acquire(const lv_font_t *font, uint32_t letter, uint32_t next,
                                 bool pixels, epd_app_font_glyph_t *out);
void ui_reader_font_remote_release(epd_app_font_glyph_t *glyph);
int ui_reader_font_remote_ready(const lv_font_t *font, uint32_t letter, uint32_t next, bool pixels);
int ui_reader_font_remote_prepare(const lv_font_t *font, const char *text, size_t length, bool pixels);
int ui_reader_font_remote_prepare_preview(const lv_font_t *font, const char *text, size_t length);
int ui_reader_font_remote_purpose(const lv_font_t *font, unsigned purpose);
epd_app_font_page_t ui_reader_font_remote_page_create(void);
int ui_reader_font_remote_page_pin(epd_app_font_page_t page, const lv_font_t *font,
                                   uint32_t letter, uint32_t next, bool pixels);
void ui_reader_font_remote_page_release(epd_app_font_page_t page);
size_t ui_reader_font_remote_reclaim_pixels(size_t min_available);
/* Ready glyph uses the builtin missing-glyph fallback; memory-only. */
bool ui_reader_font_remote_glyph_fallback(const lv_font_t *font, uint32_t letter);
void ui_reader_font_remote_stats(epd_app_font_remote_stats_t *out);
void ui_reader_font_remote_draw_stats(epd_app_font_cache_stats_t *out);
#endif
