#ifndef UI_POPUP_STYLE_H
#define UI_POPUP_STYLE_H

#include "lvgl.h"
#include "platform/epd_ui_scale.h"

static inline void ui_popup_style_dialog(lv_obj_t *dialog)
{
    lv_obj_set_style_radius(dialog, epd_ui_px(25), 0);
    lv_obj_set_style_bg_color(dialog, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x828282), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_shadow_width(dialog, 0, 0);
}

static inline void ui_popup_style_button(lv_obj_t *button)
{
    lv_obj_set_style_radius(button, epd_ui_px(20), 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
}

#endif
