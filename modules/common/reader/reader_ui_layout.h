/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_UI_LAYOUT_H
#define COMMON_READER_UI_LAYOUT_H
#include "platform/epd_app.h"

static inline void reader_ui_rect(lv_obj_t *object, int x, int y, int width, int height)
{
    if (!object) return;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static inline void reader_ui_label(lv_obj_t *label, int x, int y, int width, epd_font_role_t role)
{
    if (!label) return;
    lv_obj_set_style_text_font(label, epd_app_font_role(role), 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
}

static inline void reader_ui_button(lv_obj_t *button, int x, int y, int width, int height)
{
    if (!button) return;
    reader_ui_rect(button, x, y, width, height);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label && lv_obj_check_type(label, &lv_label_class))
    {
        lv_obj_set_style_text_font(label, epd_app_font_role(EPD_FONT_BODY), 0);
        lv_obj_set_width(label, width - 2 * epd_ui_px(8));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
}
#endif
