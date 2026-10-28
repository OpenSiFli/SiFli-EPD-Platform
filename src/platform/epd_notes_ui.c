#include "lvgl.h"
#include <rtm.h>

#if !LV_LABEL_TEXT_SELECTION
#error Notes requires LV_LABEL_TEXT_SELECTION
#endif

RTM_EXPORT(lv_textarea_get_label);
RTM_EXPORT(lv_textarea_set_text_selection);
RTM_EXPORT(lv_textarea_text_is_selected);
RTM_EXPORT(lv_textarea_clear_selection);
RTM_EXPORT(lv_label_get_text_selection_start);
RTM_EXPORT(lv_label_get_text_selection_end);
RTM_EXPORT(lv_label_set_text_selection_start);
RTM_EXPORT(lv_label_set_text_selection_end);
RTM_EXPORT(lv_indev_get_gesture_dir);
RTM_EXPORT(lv_event_get_layer);
RTM_EXPORT(lv_obj_get_coords);
RTM_EXPORT(lv_obj_invalidate);
RTM_EXPORT(lv_obj_invalidate_area);
RTM_EXPORT(lv_obj_set_style_opa);
RTM_EXPORT(lv_draw_label_dsc_init);
RTM_EXPORT(lv_draw_label);
RTM_EXPORT(lv_draw_rect_dsc_init);
RTM_EXPORT(lv_draw_rect);
RTM_EXPORT(lv_line_create);
RTM_EXPORT(lv_line_set_points);
RTM_EXPORT(lv_obj_set_style_line_width);
RTM_EXPORT(lv_obj_set_style_line_rounded);
RTM_EXPORT(lv_obj_set_style_line_color);
RTM_EXPORT(lv_obj_set_style_shadow_color);
RTM_EXPORT(lv_obj_set_style_shadow_opa);
RTM_EXPORT(lv_obj_set_style_shadow_offset_y);
RTM_EXPORT(lv_keyboard_class);
