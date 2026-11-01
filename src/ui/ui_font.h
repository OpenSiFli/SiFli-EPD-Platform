#ifndef UI_FONT_H
#define UI_FONT_H
#include "lvgl.h"
#include "ui_font_style.h"

bool ui_font_init(void);
bool ui_font_prepare_scale(unsigned index);
void ui_font_select_scale(unsigned index);
const lv_font_t *ui_font_at(unsigned index, unsigned role);
/* UI thread; normal must be a retained system face. Unknown flags or allocation
 * failure return normal. Returned fonts are owned by the system. */
const lv_font_t *ui_font_style(const lv_font_t *normal, unsigned flags);
/* After all UI objects and drawing have released their system fonts. */
void ui_font_deinit(void);
const void *ui_font_builtin_data(size_t *size);
const lv_font_t *ui_font_small(void);
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *ui_font_caption(void);
const lv_font_t *ui_font_temperature(void);
const lv_font_t *ui_font_settings_title(void);
const lv_font_t *ui_font_settings_body(void);
const lv_font_t *ui_font_settings_value(void);
/* UI thread. ID 0 is synchronous native MiSans, ready for immediate label
 * binding. Legacy nonzero catalog probes return an asynchronous shell; reading
 * views must use ui_font_reading_create. Caller owns font; fallback is NULL. */
lv_font_t *ui_font_reader_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity,
                                lv_font_t **fallback);
/* Asynchronous reading/preview shell. Wait for remote status and glyph readiness. */
lv_font_t *ui_font_reading_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity,
                                 lv_font_t **fallback);
/* Accepts both factory results after objects/drawing release their references. */
void ui_font_reader_destroy(lv_font_t *font);
bool ui_font_reader_faulted(const lv_font_t *font);
/* Cancel reading generations. Revoked remote shells remain terminal errors. */
void ui_font_reader_revoke(uint32_t id);
void ui_font_reader_revoke_storage(void);
bool ui_font_reader_release_storage(uint32_t release_id);
void ui_font_reader_resume(void);

#endif
