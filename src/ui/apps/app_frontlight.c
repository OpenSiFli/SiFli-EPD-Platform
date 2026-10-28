#include "ui_internal.h"
#include "ui_shell.h"
#include "frontlight.h"
#include "storage.h"

typedef struct
{
    lv_obj_t *title, *value, *slider;
    int pending;
    bool commit, cancel, dragging;
} frontlight_page_t;

static frontlight_page_t *frontlight_page(void)
{
    return current && current->id == UI_PAGE_FRONTLIGHT ? current->feature : NULL;
}

static void brightness_event(lv_event_t *event)
{
    frontlight_page_t *view = frontlight_page();
    if (!view || !current->foreground || current->popup || storage_changing() ||
        pending_page != NAV_IDLE) return;
    switch (lv_event_get_code(event))
    {
    case LV_EVENT_PRESSED: view->dragging = true; break;
    case LV_EVENT_VALUE_CHANGED:
        view->pending = lv_slider_get_value(view->slider);
        if (!view->dragging) view->commit = true;
        break;
    case LV_EVENT_RELEASED:
        view->dragging = false;
        view->pending = lv_slider_get_value(view->slider);
        view->commit = true;
        break;
    case LV_EVENT_PRESS_LOST:
        view->dragging = false;
        view->cancel = true;
        view->commit = false;
        view->pending = -1;
        break;
    default: break;
    }
}

static void frontlight_layout(frontlight_page_t *view)
{
    lv_obj_update_layout(current->screen);
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(64);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (lv_obj_get_style_text_font(view->title, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), ui_font_settings_title()->line_height);
    if (lv_obj_get_style_text_font(view->value, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(view->value, ui_font_settings_title(), 0);
    lv_obj_set_pos(view->value, MARGIN, top);
    lv_obj_set_size(view->value, LV_MAX(1, width - 2 * MARGIN), ui_font_settings_title()->line_height);
    int32_t slider_height = epd_ui_px(16);
    int32_t click_pad = (epd_ui_touch(64) - slider_height + 1) / 2;
    int32_t side = MARGIN + click_pad;
    int32_t slider_y = LV_MIN(top + ui_font_settings_title()->line_height + epd_ui_px(64),
                             bottom - slider_height - click_pad);
    lv_obj_set_pos(view->slider, side, slider_y);
    lv_obj_set_size(view->slider, LV_MAX(1, width - 2 * side), slider_height);
    int32_t knob_pad = epd_ui_px(14);
    if (lv_obj_get_style_pad_top(view->slider, LV_PART_KNOB) != knob_pad ||
        lv_obj_get_style_pad_bottom(view->slider, LV_PART_KNOB) != knob_pad ||
        lv_obj_get_style_pad_left(view->slider, LV_PART_KNOB) != knob_pad ||
        lv_obj_get_style_pad_right(view->slider, LV_PART_KNOB) != knob_pad)
        lv_obj_set_style_pad_all(view->slider, knob_pad, LV_PART_KNOB);
    if (lv_obj_get_style_border_width(view->slider, LV_PART_KNOB) != epd_ui_px(3))
        lv_obj_set_style_border_width(view->slider, epd_ui_px(3), LV_PART_KNOB);
    lv_obj_set_ext_click_area(view->slider, click_pad);
}

void frontlight_page_process(void)
{
    frontlight_page_t *view = frontlight_page();
    if (!view || !current->foreground) return;
    bool ok = true;
    if (view->cancel)
    {
        frontlight_cancel_preview();
        view->cancel = false;
    }
    if (view->pending >= 0 && !current->popup)
    {
        frontlight_snapshot_t before;
        frontlight_get(&before);
        if (view->commit)
        {
            if (!before.preference_valid || before.committed_percent != view->pending)
                ok = frontlight_commit((uint8_t)view->pending);
            else if (before.previewing) frontlight_cancel_preview();
        }
        else if (before.preference_valid &&
                 (before.previewing ? before.preview_percent : before.committed_percent) != view->pending)
            ok = frontlight_preview((uint8_t)view->pending);
        view->pending = -1;
        view->commit = false;
    }
    frontlight_snapshot_t light;
    frontlight_get(&light);
    char text[16];
    if (light.output_valid) snprintf(text, sizeof(text), "%u%%", (unsigned)light.output_percent);
    else strcpy(text, "--");
    text_update(view->value, text);
    int value = light.previewing ? light.preview_percent : light.committed_percent;
    if (!view->dragging && lv_slider_get_value(view->slider) != value)
        lv_slider_set_value(view->slider, value, LV_ANIM_OFF);
    bool enabled = light.available && !light.locked;
    if (enabled) lv_obj_remove_state(view->slider, LV_STATE_DISABLED);
    else lv_obj_add_state(view->slider, LV_STATE_DISABLED);
    frontlight_layout(view);
    if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
}

void frontlight_page_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, LV_EXT_STR_ID(settings_frontlight), ui_font_settings_title());
    frontlight_page_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view) return;
    view->title = title;
    view->pending = -1;
    view->value = centered_label(screen, "--", 0, 0, 1, ui_font_settings_title());
    view->slider = lv_slider_create(screen);
    lv_obj_remove_style_all(view->slider);
    lv_slider_set_range(view->slider, 0, 100);
    lv_obj_set_style_radius(view->slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view->slider, lv_color_hex(0xc4c4c4), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view->slider, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(view->slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(view->slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(view->slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(view->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(view->slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_border_width(view->slider, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(view->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(view->slider, 14, LV_PART_KNOB);
    lv_obj_set_style_opa(view->slider, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(view->slider, 24);
    lv_obj_add_event_cb(view->slider, brightness_event, LV_EVENT_ALL, NULL);
    current->focus_items[current->focus_count++] = view->slider;
    lv_group_add_obj(current->group, view->slider);
    frontlight_page_process();
}

void frontlight_page_pause(void)
{
    frontlight_page_t *view = frontlight_page();
    if (!view) return;
    frontlight_cancel_preview();
    view->pending = -1;
    view->commit = view->cancel = view->dragging = false;
}

void frontlight_page_stop(void)
{
    frontlight_page_t *view = frontlight_page();
    if (!view) return;
    frontlight_page_pause();
    lv_obj_remove_event_cb(view->slider, brightness_event);
    lv_free(view);
    current->feature = NULL;
}
