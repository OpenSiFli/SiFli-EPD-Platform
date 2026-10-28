#include "system_time_internal.h"
#include "ui_internal.h"
#include "ui_shell.h"
#include "system_time.h"
#include "epd_date_picker.h"
#include "platform/epd_app.h"
#include "platform/epd_locale.h"

#define TIME_EDIT_SAVE (-4200)

typedef struct
{
    lv_obj_t *title, *body, *picker, *date_label, *time_label, *save;
    epd_date_picker_value_t value;
    epd_date_picker_mode_t mode;
    uint32_t locale_revision, scale_revision;
    int32_t bottom, width;
    bool rebuild;
} time_edit_view_t;

static const char *time_edit_text(const char *zh, const char *en)
{
    return !strcmp(epd_locale_get(), "en_us") ? en : zh;
}
static void time_edit_labels(time_edit_view_t *view)
{
    char text[64];
    snprintf(text, sizeof(text), "%04d/%02u/%02u", view->value.date.year, view->value.date.month, view->value.date.day);
    lv_label_set_text(view->date_label, text);
    unsigned hour = view->value.minute / 60;
    if (view->value.hour24) snprintf(text, sizeof(text), "%02u:%02u", hour, view->value.minute % 60);
    else snprintf(text, sizeof(text), "%s %u:%02u", hour < 12 ? time_edit_text("上午", "AM") : time_edit_text("下午", "PM"), hour % 12 ? hour % 12 : 12, view->value.minute % 60);
    lv_label_set_text(view->time_label, text);
}
static bool time_edit_active(void)
{
    return current && current->foreground && current->id == UI_PAGE_TIME_EDIT &&
           !current->popup && pending_page == NAV_IDLE && current->feature;
}
static void time_edit_changed(lv_event_t *event)
{
    if (!time_edit_active()) return;
    time_edit_view_t *view = current->feature;
    epd_date_picker_get(lv_event_get_current_target(event), &view->value);
    time_edit_labels(view);
}
static void time_edit_choose(lv_event_t *event)
{
    if (!time_edit_active()) return;
    time_edit_view_t *view = current->feature;
    view->mode = (epd_date_picker_mode_t)(uintptr_t)lv_event_get_user_data(event);
    view->rebuild = true;
}
static lv_obj_t *time_edit_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    epd_obj_init(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, epd_ui_px(28), 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}
static void time_edit_row(time_edit_view_t *view, lv_obj_t *card, int width, int y, epd_date_picker_mode_t mode)
{
    bool date = mode == EPD_PICK_DATE;
    const char *name = date ? time_edit_text("日期", "Date") : time_edit_text("时间", "Time");
    label_create(card, name, epd_ui_px(24), y + epd_ui_px(22), width / 3, ui_font_settings_body());
    int w = epd_ui_px(date ? 230 : view->value.hour24 ? 140 : 205);
    lv_obj_t *button = epd_app_button(card, width - epd_ui_px(24) - w, y + epd_ui_px(10), w, epd_ui_px(60), "", time_edit_choose, (void *)(uintptr_t)mode);
    lv_obj_set_style_radius(button, epd_ui_px(30), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(view->mode == mode ? 0x222222 : 0xf1f1f1), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, ui_font_settings_value(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(view->mode == mode ? 0xffffff : 0x222222), 0);
    lv_obj_center(label);
    if (date) view->date_label = label; else view->time_label = label;
}
static void time_edit_layout(time_edit_view_t *view, int width, int top, int bottom)
{
    if (view->body) lv_obj_delete(view->body);
    view->body = time_edit_box(current->screen, MARGIN, top, width, bottom - top - epd_ui_px(16));
    lv_obj_set_style_bg_opa(view->body, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(view->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view->body, LV_SCROLLBAR_MODE_AUTO);
    int height = epd_date_picker_height(view->mode), y = epd_ui_px(12);
    lv_obj_t *card = time_edit_box(view->body, 0, 0, width, height + epd_ui_px(216));
    for (int i = 0; i < 2; ++i)
    {
        time_edit_row(view, card, width, y, (epd_date_picker_mode_t)i);
        y += epd_ui_px(84);
        if (view->mode == i)
        {
            lv_obj_t *separator = time_edit_box(card, epd_ui_px(24), y, width - epd_ui_px(48), 1);
            lv_obj_set_style_bg_color(separator, lv_color_hex(0xe0e0e0), 0);
            view->picker = epd_date_picker_create(card, view->mode, &view->value, width - epd_ui_px(32));
            if (view->picker)
            {
                lv_obj_set_pos(view->picker, epd_ui_px(16), y + epd_ui_px(12));
                lv_obj_add_event_cb(view->picker, time_edit_changed, LV_EVENT_VALUE_CHANGED, NULL);
            }
            y += height + epd_ui_px(24);
        }
    }
    lv_obj_set_height(card, y + epd_ui_px(12));
    y += epd_ui_px(36);
    int button_width = (width - epd_ui_px(16)) / 2;
    lv_obj_t *cancel = button_create_i18n(view->body, 0, y, button_width, epd_ui_touch(72), LV_EXT_STR_ID(platform_cancel), NAV_BACK);
    view->save = button_create_i18n(view->body, button_width + epd_ui_px(16), y, button_width, epd_ui_touch(72), LV_EXT_STR_ID(settings_save), TIME_EDIT_SAVE);
    lv_obj_set_style_radius(cancel, epd_ui_px(28), 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_set_style_border_width(view->save, 0, 0);
    lv_obj_set_style_radius(view->save, epd_ui_px(28), 0);
    lv_obj_set_style_bg_color(view->save, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(view->save, 0), lv_color_white(), 0);
    time_edit_labels(view);
}
void settings_time_edit_process(void)
{
    if (!current || current->id != UI_PAGE_TIME_EDIT || !current->feature) return;
    time_edit_view_t *view = current->feature;
    int width = lv_obj_get_width(current->screen) - 2 * MARGIN;
    int bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int top = LV_MAX(PAGE_CONTENT_Y + epd_ui_px(16), SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32));
    system_time_settings_t settings; system_time_get_settings(&settings);
    if (view->rebuild || view->bottom != bottom || view->width != width || view->scale_revision != epd_ui_scale_revision() ||
        view->locale_revision != epd_locale_revision() || view->value.hour24 != settings.hour24)
    {
        view->value.hour24 = settings.hour24;
        lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
        lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
        lv_obj_set_size(view->title, LV_MAX(1, width + MARGIN - SETTINGS_TITLE_X), ui_font_settings_title()->line_height);
        time_edit_layout(view, width, top, bottom);
        view->bottom = bottom; view->width = width;
        view->scale_revision = epd_ui_scale_revision(); view->locale_revision = epd_locale_revision(); view->rebuild = false;
    }
    lv_obj_set_state(view->save, LV_STATE_DISABLED, settings.automatic);
}
void settings_time_edit_create(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xf2f2f2), 0);
    lv_obj_t *title = page_title_create(screen, LV_EXT_STR_ID(settings_time), ui_font_settings_title());
    time_edit_view_t *view = lv_malloc_zeroed(sizeof(*view)); current->feature = view; if (!view) return;
    view->title = title; view->value = (epd_date_picker_value_t){.date = {2026,1,1}, .first_year = 2024, .last_year = 2099, .first_weekday = 1, .hour24 = true};
    struct tm local;
    if (system_time_get_local(&local))
    {
        view->value.date = (epd_date_t){local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
        view->value.minute = local.tm_hour * 60 + local.tm_min;
    }
    settings_time_edit_process();
}
bool settings_time_edit_action(int *target)
{
    if (*target != TIME_EDIT_SAVE) return false;
    time_edit_view_t *view = current->feature; if (!view || current->popup) return true;
    struct tm calendar = {.tm_year = view->value.date.year - 1900, .tm_mon = view->value.date.month - 1,
        .tm_mday = view->value.date.day, .tm_hour = view->value.minute / 60, .tm_min = view->value.minute % 60};
    if (!system_time_set_local(&calendar)) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
    else { *target = NAV_BACK; return false; }
    return true;
}
void settings_time_edit_stop(void)
{
    time_edit_view_t *view = current->feature; if (!view) return;
    lv_free(view); current->feature = NULL;
}
