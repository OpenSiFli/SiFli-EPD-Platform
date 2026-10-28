#ifndef WORDS_LAYOUT_H
#define WORDS_LAYOUT_H

#include "platform/epd_app.h"

static inline int words_line(epd_font_role_t role)
{
    return lv_font_get_line_height(epd_app_font_role(role)) + epd_ui_px(8);
}

static inline void words_box(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static inline void words_label_box(lv_obj_t *object, int x, int y, int width,
                                   int height, epd_font_role_t role)
{
    words_box(object, x, y, width, height);
    lv_obj_set_style_text_font(object, epd_app_font_role(role), 0);
}

static inline void words_button_box(lv_obj_t *object, int x, int y, int width, int height)
{
    words_box(object, x, y, width, height);
    lv_obj_t *label = lv_obj_get_child(object, 0);
    lv_obj_set_style_text_font(label, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_width(label, width - epd_ui_px(24));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, LV_MIN(height - epd_ui_px(8), words_line(EPD_FONT_BODY) * 2));
    lv_obj_center(label);
}

static inline void words_title_layout(lv_obj_t *screen, lv_obj_t *title, lv_obj_t *back)
{
    int x = epd_ui_px(32) + epd_ui_touch(60) + epd_ui_px(16);
    lv_obj_set_style_text_font(screen, epd_app_font_role(EPD_FONT_BODY), 0);
    epd_app_back_button_layout(back, epd_ui_px(24),
        epd_ui_px(81) + epd_app_font_role(EPD_FONT_TITLE)->line_height / 2);
    words_label_box(title, x, epd_ui_px(81), lv_obj_get_width(screen) - x - epd_ui_px(32),
                    words_line(EPD_FONT_TITLE), EPD_FONT_TITLE);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    epd_app_header_refresh(screen);
}

#endif
