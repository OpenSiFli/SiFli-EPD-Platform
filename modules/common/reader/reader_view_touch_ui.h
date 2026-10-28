/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_VIEW_TOUCH_UI_H
#define COMMON_READER_VIEW_TOUCH_UI_H
#include "lvgl.h"
#include "reader_view_touch.h"

static inline bool reader_view_touch_input(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    return code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET;
}

static inline reader_touch_action_t reader_view_touch_event(reader_view_touch_t *touch, lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET)
    {
        reader_view_touch_reset(touch);
        return READER_TOUCH_NONE;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
        return READER_TOUCH_NONE;
    lv_indev_t *input = lv_event_get_indev(event);
    if (!input) return READER_TOUCH_NONE;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    return reader_view_touch_update(touch, code == LV_EVENT_PRESSED ? READER_TOUCH_DOWN :
        code == LV_EVENT_PRESSING ? READER_TOUCH_MOVE : READER_TOUCH_UP, lv_tick_get(), point.x, point.y);
}

static inline void reader_view_touch_controls(lv_obj_t *parent)
{
    if (!parent) return;
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE))
    {
        lv_obj_set_style_outline_width(parent, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(parent, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(parent, lv_color_hex(0x999999), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(parent, lv_color_hex(0x999999), LV_STATE_FOCUS_KEY);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i)
        reader_view_touch_controls(lv_obj_get_child(parent, i));
}
#endif
