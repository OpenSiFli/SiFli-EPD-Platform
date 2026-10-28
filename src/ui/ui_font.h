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
/* UI thread. ID 0 creates MiSans. An unavailable external ID or a creation
 * failure returns NULL. The caller owns both the font and returned fallback. */
lv_font_t *ui_font_reader_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity,
                                lv_font_t **fallback);
void ui_font_reader_destroy(lv_font_t *font);
bool ui_font_reader_faulted(const lv_font_t *font);
/* UI thread. Waits for drawing, closes streams and releases leases. Retained
 * font objects remain valid and resolve subsequent glyphs through fallback. */
void ui_font_reader_revoke(uint32_t id);
void ui_font_reader_revoke_storage(void);

#endif
