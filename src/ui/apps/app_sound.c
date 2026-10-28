#include "ui_internal.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "platform/epd_sound.h"
#include "storage.h"

#define SOUND_CHANNEL_BASE (-12000)
#define SOUND_MUTE (-12010)
#define SOUND_NOTIFICATION_TONE (-12011)
#define SOUND_TOUCH_TONE (-12012)
#define SOUND_TONE_BASE (-12020)

static const struct
{
    epd_sound_tone_t tone;
    lv_ext_str_id_t title;
} sound_tones[] = {
    {EPD_TONE_NONE, LV_EXT_STR_ID(settings_tone_none)},
    {EPD_TONE_ARGON, LV_EXT_STR_ID(settings_tone_argon)},
    {EPD_TONE_BERYLLIUM, LV_EXT_STR_ID(settings_tone_beryllium)},
    {EPD_TONE_CETI, LV_EXT_STR_ID(settings_tone_ceti)},
    {EPD_TONE_TICK, LV_EXT_STR_ID(settings_tone_tick)},
    {EPD_TONE_KEYPRESS, LV_EXT_STR_ID(settings_tone_keypress)}
};

typedef struct
{
    lv_obj_t *title, *body, *value, *slider;
    ui_settings_list_t *list;
    const lv_font_t *body_font, *small_font;
    uint32_t scale_revision;
    epd_sound_channel_t channel;
    int pending;
    bool dragging, edited, commit, previewing;
} sound_page_t;

static epd_sound_channel_t sound_channel_to_open;

static sound_page_t *sound_page(void)
{
    return current && (current->id == UI_PAGE_SOUND || current->id == UI_PAGE_SOUND_VOLUME ||
                       current->id == UI_PAGE_SOUND_TONE) ?
           current->feature : NULL;
}

static lv_ext_str_id_t sound_channel_title(epd_sound_channel_t channel)
{
    static const lv_ext_str_id_t titles[] = {
        LV_EXT_STR_ID(settings_sound_master), LV_EXT_STR_ID(settings_sound_media),
        LV_EXT_STR_ID(settings_sound_notification), LV_EXT_STR_ID(settings_sound_touch)
    };
    return titles[channel];
}

lv_ext_str_id_t sound_volume_title_id(void)
{
    sound_page_t *view = sound_page();
    return sound_channel_title(view ? view->channel : sound_channel_to_open);
}

lv_ext_str_id_t sound_tone_title_id(void)
{
    sound_page_t *view = sound_page();
    epd_sound_channel_t channel = view ? view->channel : sound_channel_to_open;
    return channel == EPD_SOUND_TOUCH ? LV_EXT_STR_ID(settings_touch_tone) :
                                      LV_EXT_STR_ID(settings_notification_tone);
}

static const char *sound_tone_name(epd_sound_tone_t tone)
{
    for (unsigned i = 0; i < sizeof(sound_tones) / sizeof(sound_tones[0]); ++i)
        if (sound_tones[i].tone == tone) return LV_EXT_STR_GET(sound_tones[i].title);
    return epd_get_str(settings_tone_none);
}

static bool sound_snapshot(epd_sound_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return epd_sound_read(snapshot) && snapshot->initialized &&
           snapshot->available && snapshot->output_ready;
}

static bool sound_input_allowed(void)
{
    return current && current->foreground && !current->popup && !ui_shell_modal_visible() &&
           !storage_changing() && pending_page == NAV_IDLE;
}

static void sound_cancel(sound_page_t *view)
{
    view->pending = -1;
    view->dragging = view->edited = view->commit = false;
}

static void sound_preview_cancel(sound_page_t *view)
{
    if (view->previewing) epd_sound_preview_stop();
    view->previewing = false;
}

static bool sound_preview_allowed(const epd_sound_snapshot_t *snapshot, epd_sound_channel_t channel)
{
    return !snapshot->muted && snapshot->volume[EPD_SOUND_MASTER] && snapshot->volume[channel];
}

static void sound_selected(int key, void *context)
{
    if (current == context && sound_input_allowed()) pending_page = key;
}

static void sound_slider_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_VALUE_CHANGED &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    sound_page_t *view = sound_page();
    if (!view || !view->slider) return;
    if (code == LV_EVENT_PRESS_LOST)
    {
        sound_cancel(view);
        return;
    }
    epd_sound_snapshot_t snapshot;
    if (!sound_input_allowed() || !sound_snapshot(&snapshot))
    {
        sound_cancel(view);
        return;
    }
    switch (code)
    {
    case LV_EVENT_PRESSED:
    {
        lv_indev_t *indev = lv_indev_active();
        view->dragging = !indev || lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
        view->edited = false;
        break;
    }
    case LV_EVENT_VALUE_CHANGED:
        view->pending = lv_slider_get_value(view->slider);
        view->edited = true;
        view->commit = !view->dragging;
        break;
    case LV_EVENT_RELEASED:
        if (view->dragging && view->edited) view->commit = true;
        view->dragging = false;
        break;
    default: break;
    }
}

static bool sound_layout(sound_page_t *view)
{
    lv_obj_update_layout(current->screen);
    int32_t width = lv_obj_get_width(current->screen);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (lv_obj_get_style_text_font(view->title, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    if (lv_label_get_long_mode(view->title) != LV_LABEL_LONG_WRAP)
        lv_label_set_long_mode(view->title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), LV_SIZE_CONTENT);
    lv_obj_update_layout(view->title);
    int32_t title_height = lv_obj_get_height(view->title);
    if (view->list)
    {
        int32_t top = SETTINGS_TITLE_Y + title_height + epd_ui_px(32);
        lv_obj_set_pos(view->body, 0, top);
        lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
        lv_obj_update_layout(view->body);
        if (view->scale_revision != epd_ui_scale_revision() ||
            view->body_font != ui_font_settings_body() || view->small_font != ui_font_settings_value())
        {
            if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value()))
                return false;
            view->body_font = ui_font_settings_body();
            view->small_font = ui_font_settings_value();
            view->scale_revision = epd_ui_scale_revision();
        }
        return ui_settings_list_update_layout(view->list);
    }
    if (view->slider)
    {
        int32_t top = SETTINGS_TITLE_Y + title_height + epd_ui_px(64);
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
    return true;
}

static void sound_render(sound_page_t *view)
{
    epd_sound_snapshot_t snapshot;
    bool ready = sound_snapshot(&snapshot);
    if (!ready || !sound_preview_allowed(&snapshot, view->channel)) sound_preview_cancel(view);
    if (!sound_layout(view)) { current->locale_retry = true; return; }
    if (view->list)
    {
        char values[EPD_SOUND_CHANNEL_COUNT][16];
        ui_settings_list_row_t rows[EPD_SOUND_CHANNEL_COUNT + 3] = {0};
        size_t count = 0;
        if (current->id == UI_PAGE_SOUND_TONE)
        {
            epd_sound_tone_t saved_tone = epd_sound_get_tone(view->channel);
            for (unsigned i = 0; i < sizeof(sound_tones) / sizeof(sound_tones[0]); ++i)
            {
                epd_sound_tone_t tone = sound_tones[i].tone;
                if (!epd_sound_tone_valid(view->channel, tone)) continue;
                rows[count++] = (ui_settings_list_row_t){.key=SOUND_TONE_BASE + tone,
                    .kind=UI_SETTINGS_ROW_CHOICE, .title=LV_EXT_STR_GET(sound_tones[i].title),
                    .checked=saved_tone == tone, .enabled=ready};
            }
        }
        else
        {
            for (unsigned i = 0; i < EPD_SOUND_CHANNEL_COUNT; ++i)
            {
                if (ready) snprintf(values[i], sizeof(values[i]), "%u%%", (unsigned)snapshot.volume[i]);
                else strcpy(values[i], "--");
                rows[i] = (ui_settings_list_row_t){.key=SOUND_CHANNEL_BASE + (int)i,
                    .kind=UI_SETTINGS_ROW_LINK, .title=LV_EXT_STR_GET(sound_channel_title(i)),
                    .value=values[i], .enabled=ready};
            }
            rows[EPD_SOUND_CHANNEL_COUNT] = (ui_settings_list_row_t){.key=SOUND_MUTE,
                .kind=UI_SETTINGS_ROW_SWITCH, .title=epd_get_str(settings_sound_mute),
                .value=ready ? NULL : "--", .checked=snapshot.muted, .enabled=ready};
            rows[EPD_SOUND_CHANNEL_COUNT + 1] = (ui_settings_list_row_t){.key=SOUND_NOTIFICATION_TONE,
                .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(settings_notification_tone),
                .value=sound_tone_name(epd_sound_get_tone(EPD_SOUND_NOTIFICATION)), .enabled=ready};
            rows[EPD_SOUND_CHANNEL_COUNT + 2] = (ui_settings_list_row_t){.key=SOUND_TOUCH_TONE,
                .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(settings_touch_tone),
                .value=sound_tone_name(epd_sound_get_tone(EPD_SOUND_TOUCH)), .enabled=ready};
            count = sizeof(rows) / sizeof(rows[0]);
        }
        if (!ui_settings_list_set_rows(view->list, rows, count))
            current->locale_retry = true;
    }
    else if (view->slider)
    {
        if (!ready) sound_cancel(view);
        int value = view->pending >= 0 ? view->pending : snapshot.volume[view->channel];
        char text[16];
        if (ready) snprintf(text, sizeof(text), "%u%%", (unsigned)value);
        else strcpy(text, "--");
        text_update(view->value, text);
        if (!view->dragging && lv_slider_get_value(view->slider) != value)
            lv_slider_set_value(view->slider, value, LV_ANIM_OFF);
        if (ready) lv_obj_remove_state(view->slider, LV_STATE_DISABLED);
        else lv_obj_add_state(view->slider, LV_STATE_DISABLED);
    }
}

void sound_page_resume(void)
{
    sound_page_t *view = sound_page();
    if (!view) return;
    if (current->popup) { current->locale_retry = true; return; }
    sound_render(view);
}

void sound_page_process(void)
{
    sound_page_t *view = sound_page();
    if (!view || !current->foreground) return;
    if (!sound_input_allowed())
    {
        sound_cancel(view);
        sound_preview_cancel(view);
    }
    if (current->popup) return;
    bool ok = true;
    if (view->commit && view->pending >= 0)
    {
        epd_sound_snapshot_t snapshot;
        if (sound_snapshot(&snapshot) && snapshot.volume[view->channel] != view->pending)
            ok = epd_sound_set_volume(view->channel, (unsigned)view->pending);
        sound_cancel(view);
    }
    sound_render(view);
    if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
}

bool sound_page_action(int *target)
{
    sound_page_t *view = sound_page();
    if (!view || !view->list) return false;
    if (current->id == UI_PAGE_SOUND_TONE)
    {
        if (*target == NAV_BACK) sound_preview_cancel(view);
        int tone = *target - SOUND_TONE_BASE;
        if (tone < EPD_TONE_NONE || tone > EPD_TONE_KEYPRESS) return false;
        epd_sound_snapshot_t snapshot;
        if (!sound_input_allowed() || !sound_snapshot(&snapshot) ||
            !epd_sound_tone_valid(view->channel, (epd_sound_tone_t)tone)) return true;
        sound_preview_cancel(view);
        bool ok = epd_sound_get_tone(view->channel) == tone ||
                  epd_sound_set_tone(view->channel, (epd_sound_tone_t)tone);
        sound_page_resume();
        if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
        else if (tone != EPD_TONE_NONE && sound_snapshot(&snapshot) &&
                 sound_preview_allowed(&snapshot, view->channel))
            view->previewing = epd_sound_preview(view->channel, (epd_sound_tone_t)tone);
        return true;
    }
    int channel = *target - SOUND_CHANNEL_BASE;
    bool tone_page = *target == SOUND_NOTIFICATION_TONE || *target == SOUND_TOUCH_TONE;
    if ((channel < 0 || channel >= EPD_SOUND_CHANNEL_COUNT) && *target != SOUND_MUTE && !tone_page) return false;
    epd_sound_snapshot_t snapshot;
    if (!sound_input_allowed() || !sound_snapshot(&snapshot)) return true;
    if (tone_page)
    {
        sound_channel_to_open = *target == SOUND_TOUCH_TONE ? EPD_SOUND_TOUCH : EPD_SOUND_NOTIFICATION;
        *target = UI_PAGE_SOUND_TONE;
        return false;
    }
    if (*target != SOUND_MUTE)
    {
        sound_channel_to_open = (epd_sound_channel_t)channel;
        *target = UI_PAGE_SOUND_VOLUME;
        return false;
    }
    bool ok = epd_sound_set_muted(!snapshot.muted);
    sound_page_resume();
    if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
    return true;
}

void sound_page_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, page_title_id(current->id), ui_font_settings_title());
    sound_page_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, PAGE_CONTENT_Y,
                            LV_MAX(1, lv_obj_get_width(screen) - 2 * MARGIN), ui_font_settings_body());
        return;
    }
    view->title = title;
    view->channel = sound_channel_to_open;
    if (current->id == UI_PAGE_SOUND_TONE && view->channel != EPD_SOUND_TOUCH)
        view->channel = EPD_SOUND_NOTIFICATION;
    sound_cancel(view);
    if (current->id != UI_PAGE_SOUND_VOLUME)
    {
        view->body = lv_obj_create(screen);
        epd_obj_init(view->body);
        view->body_font = ui_font_settings_body();
        view->small_font = ui_font_settings_value();
        view->list = ui_settings_list_create(view->body, view->body_font, view->small_font, sound_selected, current);
        if (view->list) ui_settings_list_set_group(view->list, current->group);
        else centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, PAGE_CONTENT_Y,
                                LV_MAX(1, lv_obj_get_width(screen) - 2 * MARGIN), ui_font_settings_body());
    }
    else
    {
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
        lv_obj_add_event_cb(view->slider, sound_slider_event, LV_EVENT_ALL, NULL);
        current->focus_items[current->focus_count++] = view->slider;
        lv_group_add_obj(current->group, view->slider);
    }
    sound_page_resume();
}

void sound_page_pause(void)
{
    sound_page_t *view = sound_page();
    if (view)
    {
        sound_cancel(view);
        sound_preview_cancel(view);
    }
}

void sound_page_stop(void)
{
    sound_page_t *view = sound_page();
    if (!view) return;
    sound_page_pause();
    if (view->slider) lv_obj_remove_event_cb(view->slider, sound_slider_event);
    if (view->list) ui_settings_list_destroy(view->list);
    lv_free(view);
    current->feature = NULL;
}

void sound_page_restore_focus(void)
{
    sound_page_t *view = sound_page();
    if (view && view->list) ui_settings_list_set_group(view->list, current->group);
}
