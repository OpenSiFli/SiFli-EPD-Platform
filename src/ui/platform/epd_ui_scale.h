#ifndef EPD_UI_SCALE_H
#define EPD_UI_SCALE_H

#include "lvgl.h"

typedef enum { EPD_FONT_BODY, EPD_FONT_SMALL, EPD_FONT_TITLE, EPD_FONT_CAPTION,
               EPD_FONT_TEMPERATURE } epd_font_role_t;

/* UI-thread metrics. Coordinates are physical pixels; only design dimensions
 * are scaled. Reader text and image source dimensions remain caller-owned. */
unsigned epd_ui_scale_percent(void);
uint32_t epd_ui_scale_revision(void);
int32_t epd_ui_px(int32_t base);
int32_t epd_ui_touch(int32_t base);
void epd_ui_content_area(lv_obj_t *screen, lv_area_t *area);
/* Preview a role at one of the five settings indexes without changing the
 * system scale. NULL means allocation failed. Returned fonts remain valid. */
const lv_font_t *epd_ui_font_at(unsigned index, epd_font_role_t role);

#endif
