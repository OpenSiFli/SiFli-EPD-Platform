#ifndef UI_SHELL_BATTERY_H
#define UI_SHELL_BATTERY_H

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static inline void ui_shell_battery_layout(lv_obj_t *battery, lv_obj_t *percent,
                                           const lv_font_t *font,
                                           int width, int height, int border)
{
    lv_obj_set_size(battery, width, height);
    lv_obj_set_style_bg_color(battery, lv_color_hex(0xeaeaea), 0);
    lv_obj_set_style_bg_opa(battery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(battery, lv_color_hex(0x828282), 0);
    lv_obj_set_style_border_width(battery, border, 0);
    lv_obj_set_style_radius(battery, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(percent, font, 0);
    lv_obj_set_size(percent, width - 2 * border, LV_SIZE_CONTENT);
    lv_label_set_long_mode(percent, LV_LABEL_LONG_CLIP);
    lv_obj_center(percent);
}

static inline void ui_shell_battery_text(lv_obj_t *percent, int value)
{
    char text[8];
    if (value < 0 || value > 100) snprintf(text, sizeof(text), "--");
    else snprintf(text, sizeof(text), "%d", value);
    if (strcmp(lv_label_get_text(percent), text)) lv_label_set_text(percent, text);
}

#endif
