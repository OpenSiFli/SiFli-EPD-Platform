#include "storage_internal.h"
#include "system_time_internal.h"
#include "platform/epd_locale_internal.h"
#include "ui_settings_internal.h"
#include "ui_internal.h"
#include "platform/epd_keyboard.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "platform/app_card.h"
#include "platform/app_installer.h"
#include "storage.h"
#include "network.h"
#include "bt_pan.h"
#include "system_time.h"
#include "system_info.h"
#include "reading_font.h"
#include "device_identity.h"
#include "settings_wireless.h"
#include "frontlight.h"
#include "usb_card_reader.h"
#include "system_reset.h"
#include "boards/board_service.h"

#define LOAD_CARD_APPS (-198)
#define LANGUAGE_CHINESE (-220)
#define LANGUAGE_ENGLISH (-221)
#define TIME_AUTOMATIC (-4000)
#define TIME_HOUR24 (-4001)
#define USB_CARD_READER (-4002)
#define RESET_FACTORY (-4100)
#define RESET_NETWORK (-4101)
#define RESET_CARD (-4102)
#define RESET_CONFIRM (-4103)
#define RESET_RETRY (-4104)
#define RESET_FINISH (-4105)
#define RESET_FINISH_CONFIRM (-4106)
#define RESET_DISMISS (-4107)
#define RESET_STATUS (-4108)
#define OPTION_BASE (-5000)
#define TIMEZONE_BASE (-7000)
#define DEVICE_RENAME (-7200)
#define DEVICE_NAME_SAVE (-7201)
#define DEVICE_NAME_CANCEL (-7202)

typedef struct
{
    lv_obj_t *body, *title;
    ui_settings_list_t *list;
    ui_setting_id_t option;
    int32_t bottom;
    uint32_t poll_tick;
    system_info_t info;
    bool info_valid;
    bool initial_position_pending;
    lv_obj_t *device_name, *rename, *name_editor, *name_input, *name_keyboard;
    system_reset_target_t reset_target;
    uint32_t reset_revision, reset_locale, reset_scale, reset_popup_serial;
    rt_err_t reset_error;
    int reset_confirmation;
    bool reset_valid, reset_prepare_failed, reset_start_failed;
} settings_page_t;
static ui_setting_id_t option_to_open;
static void device_name_layout(settings_page_t *view);
static void device_name_edit(settings_page_t *view, bool open);

static bool is_settings_list(ui_page_id_t id)
{
    switch (id)
    {
    case UI_PAGE_SETTINGS: case UI_PAGE_DISPLAY_SETTINGS: case UI_PAGE_TEXT_SETTINGS:
    case UI_PAGE_NETWORK_SETTINGS: case UI_PAGE_APP_MANAGEMENT:
    case UI_PAGE_ABOUT: case UI_PAGE_MORE_SETTINGS: case UI_PAGE_TIME_SETTINGS:
    case UI_PAGE_TIMEZONE_SETTINGS: case UI_PAGE_STATUS_INFO: case UI_PAGE_LOCK_OPTIONS:
    case UI_PAGE_NETWORK_STATUS: case UI_PAGE_SETTING_OPTIONS:
    case UI_PAGE_FRONTLIGHT_MODE:
    case UI_PAGE_USB_SETTINGS: case UI_PAGE_RESET_SETTINGS:
        return true;
    default: return false;
    }
}

static settings_page_t *settings_page(void)
{
    return current && is_settings_list(current->id) ? current->feature : NULL;
}

static void list_selected(int key, void *context)
{
    if (!current || current != context || !current->foreground || current->popup ||
        (storage_changing() && current->id != UI_PAGE_RESET_SETTINGS) || pending_page != NAV_IDLE) return;
    pending_page = key;
}

static void settings_layout(settings_page_t *view)
{
    int32_t bottom = ui_shell_content_bottom();
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = LV_MAX(SETTINGS_CONTENT_Y, SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32));
    if (view->title)
    {
        if (lv_obj_get_style_text_font(view->title, 0) != ui_font_settings_title())
            lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
        lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
        lv_obj_set_size(view->title, width - MARGIN - SETTINGS_TITLE_X, ui_font_settings_title()->line_height);
    }
    if (view->device_name)
    {
        int32_t name_y = top + epd_ui_px(26);
        if (lv_obj_get_style_text_font(view->device_name, 0) != ui_font_settings_title())
            lv_obj_set_style_text_font(view->device_name, ui_font_settings_title(), 0);
        lv_obj_set_pos(view->device_name, MARGIN, name_y);
        lv_obj_set_width(view->device_name, width - 2 * MARGIN);
        lv_obj_set_height(view->device_name, LV_SIZE_CONTENT);
        lv_obj_update_layout(view->device_name);
        int32_t rename_y = LV_MAX(epd_ui_px(328), name_y + lv_obj_get_height(view->device_name) + epd_ui_px(40));
        lv_obj_t *label = lv_obj_get_child(view->rename, 0);
        if (lv_obj_get_style_text_font(label, 0) != ui_font_settings_value())
            lv_obj_set_style_text_font(label, ui_font_settings_value(), 0);
        text_update(label, epd_get_str(settings_rename));
        lv_point_t text_size;
        lv_text_get_size(&text_size, epd_get_str(settings_rename), ui_font_settings_value(), 0, 0,
                         width - 2 * MARGIN, LV_TEXT_FLAG_NONE);
        int32_t button_width = LV_MIN(width - 2 * MARGIN, LV_MAX(epd_ui_px(185), text_size.x + epd_ui_px(48)));
        int32_t button_height = LV_MAX(epd_ui_touch(55), ui_font_settings_value()->line_height + epd_ui_px(16));
        lv_obj_set_pos(view->rename, (width - button_width) / 2, rename_y);
        lv_obj_set_size(view->rename, button_width, button_height);
        lv_obj_set_size(label, button_width - epd_ui_px(24), ui_font_settings_value()->line_height);
        if (lv_obj_get_style_align(label, 0) != LV_ALIGN_CENTER)
            lv_obj_center(label);
        top = LV_MAX(epd_ui_px(454), rename_y + button_height + epd_ui_px(71));
    }
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
    lv_obj_update_layout(view->body);
    view->bottom = bottom;
    if (view->name_editor) device_name_layout(view);
}

static void device_name_event(lv_event_t *event)
{
    if (!current || !current->foreground || current->popup || storage_changing() || pending_page != NAV_IDLE) return;
    if (lv_event_get_code(event) == LV_EVENT_READY) pending_page = DEVICE_NAME_SAVE;
    else if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        pending_page = (int)(intptr_t)lv_event_get_user_data(event);
}

static lv_obj_t *device_name_button(lv_obj_t *parent, const char *text, int action)
{
    lv_obj_t *button = lv_button_create(parent);
    epd_obj_init(button);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -3, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(button, device_name_event, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    lv_obj_t *label = label_create(button, text, 0, 0, 1, ui_font_settings_value());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_group_add_obj(current->group, button);
    return button;
}

static void device_name_layout(settings_page_t *view)
{
    int32_t width = lv_obj_get_width(current->screen), bottom = ui_shell_content_bottom();
    int32_t top = LV_MAX(SETTINGS_CONTENT_Y, SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32));
    lv_obj_set_pos(view->name_editor, 0, top);
    lv_obj_set_size(view->name_editor, width, LV_MAX(1, bottom - top));
    int32_t height = LV_MAX(epd_ui_touch(72), ui_font_settings_body()->line_height + epd_ui_px(24));
    lv_obj_set_style_text_font(view->name_input, ui_font_settings_body(), 0);
    lv_obj_set_pos(view->name_input, MARGIN, 0);
    lv_obj_set_size(view->name_input, width - 2 * MARGIN, height);
    lv_textarea_set_placeholder_text(view->name_input, epd_get_str(settings_device_name));
    int32_t y = height + epd_ui_px(24), button_width = (width - 2 * MARGIN - epd_ui_px(20)) / 2;
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *button = lv_obj_get_child(view->name_editor, i + 1);
        lv_obj_set_pos(button, MARGIN + i * (button_width + epd_ui_px(20)), y);
        lv_obj_set_size(button, button_width, height);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        text_update(label, i ? epd_get_str(settings_save) : epd_get_str(platform_cancel));
        lv_obj_set_style_text_font(label, ui_font_settings_value(), 0);
        lv_obj_set_size(label, button_width - epd_ui_px(24), ui_font_settings_value()->line_height);
        lv_obj_center(label);
    }
    epd_keyboard_refresh(view->name_keyboard);
    int32_t available = LV_MAX(1, bottom - top - y - height - epd_ui_px(32));
    int32_t keyboard_height = LV_MIN(epd_keyboard_preferred_height(view->name_keyboard), available);
    lv_obj_set_size(view->name_keyboard, width, keyboard_height);
    lv_obj_align(view->name_keyboard, LV_ALIGN_BOTTOM_MID, 0, -epd_ui_px(16));
    text_update(view->title, epd_get_str(settings_device_name));
}

static void device_name_edit(settings_page_t *view, bool open)
{
    if (open == (view->name_editor != NULL)) return;
    if (open)
    {
        char name[DEVICE_IDENTITY_NAME_CAPACITY];
        if (device_identity_get_name(name, sizeof(name)) != RT_EOK) return;
        ui_settings_list_set_group(view->list, NULL);
        lv_group_remove_obj(view->rename);
        object_visible(view->body, false);
        object_visible(view->device_name, false);
        object_visible(view->rename, false);
        view->name_editor = lv_obj_create(current->screen);
        epd_obj_init(view->name_editor);
        view->name_input = lv_textarea_create(view->name_editor);
        lv_textarea_set_one_line(view->name_input, true);
        lv_textarea_set_max_length(view->name_input, DEVICE_IDENTITY_NAME_MAX);
        lv_textarea_set_text(view->name_input, name);
        lv_obj_set_style_border_width(view->name_input, 2, 0);
        lv_obj_set_style_border_color(view->name_input, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(view->name_input, 0, LV_STATE_FOCUSED);
        lv_group_add_obj(current->group, view->name_input);
        device_name_button(view->name_editor, epd_get_str(platform_cancel), DEVICE_NAME_CANCEL);
        device_name_button(view->name_editor, epd_get_str(settings_save), DEVICE_NAME_SAVE);
        view->name_keyboard = epd_keyboard_create(view->name_editor, view->name_input, NULL);
        lv_obj_add_event_cb(view->name_keyboard, device_name_event, LV_EVENT_READY, NULL);
        lv_group_focus_obj(view->name_input);
        device_name_layout(view);
    }
    else
    {
        lv_obj_delete(view->name_editor);
        view->name_editor = view->name_input = view->name_keyboard = NULL;
        object_visible(view->body, true);
        object_visible(view->device_name, true);
        object_visible(view->rename, true);
        text_update(view->title, epd_get_str(settings_about));
        lv_group_add_obj(current->group, view->rename);
        ui_settings_list_set_group(view->list, current->group);
        lv_group_focus_obj(view->rename);
    }
}

static void settings_list_create(lv_obj_t *screen)
{
    settings_page_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_settings_body());
        return;
    }
    view->option = option_to_open;
    view->initial_position_pending = current->id == UI_PAGE_TIMEZONE_SETTINGS ||
                                     current->id == UI_PAGE_SETTING_OPTIONS;
    view->title = current->id == UI_PAGE_SETTING_OPTIONS ?
        label_create(screen, ui_settings_label(view->option), SETTINGS_TITLE_X, SETTINGS_TITLE_Y,
                     WIDTH - MARGIN - SETTINGS_TITLE_X, ui_font_settings_title()) :
        page_title_create(screen, current->id == UI_PAGE_RESET_SETTINGS ?
                          LV_EXT_STR_ID(settings_reset) : page_title_id(current->id), ui_font_settings_title());
    view->body = lv_obj_create(screen);
    epd_obj_init(view->body);
    if (current->id == UI_PAGE_ABOUT)
    {
        char name[DEVICE_IDENTITY_NAME_CAPACITY] = "";
        device_identity_get_name(name, sizeof(name));
        view->device_name = centered_label(screen, name, 0, 0, 1, ui_font_settings_title());
        lv_label_set_long_mode(view->device_name, LV_LABEL_LONG_WRAP);
        view->rename = device_name_button(screen, epd_get_str(settings_rename), DEVICE_RENAME);
        lv_obj_set_style_border_width(view->rename, 0, 0);
        lv_obj_set_style_bg_color(view->rename, lv_color_hex(0xD8D8D8), 0);
        lv_obj_set_style_radius(view->rename, LV_RADIUS_CIRCLE, 0);
    }
    settings_layout(view);
    view->list = ui_settings_list_create(view->body, ui_font_settings_body(), ui_font_settings_value(), list_selected, current);
    if (!view->list)
    {
        centered_label_i18n(view->body, LV_EXT_STR_ID(platform_no_memory), 0, 80,
                            lv_obj_get_width(view->body), ui_font_settings_body());
        return;
    }
    ui_settings_list_set_group(view->list, current->group);
}

#define LINK(key_, text_, icon_) \
    {.key=(key_), .kind=UI_SETTINGS_ROW_LINK, .title=(text_), .icon=(icon_), .enabled=true}
#define DISABLED(key_, text_, icon_) \
    {.key=(key_), .kind=UI_SETTINGS_ROW_LINK, .title=(text_), .icon=(icon_), .enabled=false}
#define INFO(key_, title_, value_) \
    {.key=(key_), .kind=UI_SETTINGS_ROW_INFO, .title=(title_), .value=(value_), .enabled=true}
#define SWITCH_ROW(key_, title_, checked_) \
    {.key=(key_), .kind=UI_SETTINGS_ROW_SWITCH, .title=(title_), .checked=(checked_), .enabled=true}
#define CHOICE_ROW(key_, title_, checked_) \
    {.key=(key_), .kind=UI_SETTINGS_ROW_CHOICE, .title=(title_), .checked=(checked_), .enabled=true}
#define SHOW(rows_) settings_rows(view, (rows_), sizeof(rows_) / sizeof((rows_)[0]))

static void settings_rows(settings_page_t *view, const ui_settings_list_row_t *rows, size_t count)
{
    if (!ui_settings_list_set_rows(view->list, rows, count))
    { current->locale_retry = true; return; }
    if (view->initial_position_pending)
    {
        for (size_t i = 0; i < count; ++i)
            if (rows[i].checked)
            {
                if (!ui_settings_list_show_key(view->list, rows[i].key))
                { current->locale_retry = true; return; }
                break;
            }
        view->initial_position_pending = false;
    }
}

static bool reset_can_finish(const system_reset_status_t *state)
{
    return state->kind == SYSTEM_RESET_FACTORY && state->factory_pending && state->internal_done && state->card_pending &&
        (state->phase == SYSTEM_RESET_WAIT_CARD || state->phase == SYSTEM_RESET_FAILED);
}

static void reset_rows(settings_page_t *view)
{
    system_reset_status_t state;
    system_reset_get_status(&state);
    if (view->reset_valid && view->reset_revision != state.revision)
    {
        view->reset_error = RT_EOK;
        if (state.phase != SYSTEM_RESET_IDLE)
            view->reset_prepare_failed = view->reset_start_failed = false;
    }
    bool idle = state.phase == SYSTEM_RESET_IDLE && !state.factory_pending && !view->reset_error;
    ui_settings_list_row_t rows[6] = {
        LINK(RESET_FACTORY, epd_get_str(settings_reset_factory), NULL),
        LINK(RESET_NETWORK, epd_get_str(settings_reset_network), NULL),
        LINK(RESET_CARD, epd_get_str(settings_reset_card), NULL)
    };
    for (unsigned i = 0; i < 3; ++i) rows[i].enabled = idle;
    size_t count = 3;
    const char *message = NULL;
    if (view->reset_error) message = epd_get_str(settings_reset_failed);
    else switch (state.phase)
    {
    case SYSTEM_RESET_PREPARING: message = epd_get_str(settings_reset_preparing); break;
    case SYSTEM_RESET_RUNNING: message = epd_get_str(settings_reset_running); break;
    case SYSTEM_RESET_WAIT_CARD: message = epd_get_str(settings_reset_wait_card); break;
    case SYSTEM_RESET_FAILED: message = epd_get_str(settings_reset_failed); break;
    case SYSTEM_RESET_DONE:
        message = state.card_cancelled ? epd_get_str(settings_reset_ended) : epd_get_str(settings_reset_done); break;
    default: break;
    }
    if (reset_can_finish(&state)) message = epd_get_str(settings_reset_partial);
    if (message) rows[count++] = (ui_settings_list_row_t)INFO(RESET_STATUS, epd_get_str(settings_status), message);
    bool failed = state.phase == SYSTEM_RESET_FAILED || state.phase == SYSTEM_RESET_WAIT_CARD || view->reset_error;
    if (failed)
    {
        rows[count++] = (ui_settings_list_row_t)LINK(RESET_RETRY,
            state.card_pending ? epd_get_str(settings_reset_retry_card) : epd_get_str(settings_reset_retry), NULL);
        if (reset_can_finish(&state))
            rows[count++] = (ui_settings_list_row_t)LINK(RESET_FINISH, epd_get_str(settings_reset_finish), NULL);
        else if (!state.factory_pending && (state.kind != SYSTEM_RESET_FACTORY ||
                 state.phase == SYSTEM_RESET_IDLE || state.phase == SYSTEM_RESET_FAILED))
            rows[count++] = (ui_settings_list_row_t)LINK(RESET_DISMISS, epd_get_str(settings_reset_back), NULL);
    }
    else if (state.phase == SYSTEM_RESET_DONE)
        rows[count++] = (ui_settings_list_row_t)LINK(RESET_DISMISS,
            state.kind == SYSTEM_RESET_FACTORY ? epd_get_str(settings_reset_restart) : epd_get_str(settings_reset_done), NULL);
    if (!ui_settings_list_set_rows(view->list, rows, count))
    { current->locale_retry = true; return; }
    view->reset_revision = state.revision;
    view->reset_locale = epd_locale_revision();
    view->reset_scale = epd_ui_scale_revision();
    view->reset_valid = true;
}

static void reset_prepare(settings_page_t *view, system_reset_kind_t kind)
{
    view->reset_confirmation = 0;
    view->reset_start_failed = false;
    view->reset_error = system_reset_prepare(kind, &view->reset_target);
    view->reset_prepare_failed = view->reset_error != RT_EOK;
    if (view->reset_prepare_failed)
    {
        view->reset_target.kind = kind;
        settings_resume();
        return;
    }
    char capacity[32], message[640];
    if (kind == SYSTEM_RESET_CARD)
    {
        storage_format_size(view->reset_target.card_bytes, capacity, sizeof(capacity));
        EPD_FMT_settings_reset_card_confirm(message, sizeof(message), capacity);
        popup_confirm(message, NULL, RESET_CONFIRM);
    }
    else popup_confirm_i18n(kind == SYSTEM_RESET_NETWORK ? LV_EXT_STR_ID(settings_reset_network_confirm) :
        view->reset_target.include_card ? LV_EXT_STR_ID(settings_reset_factory_card_confirm) :
        LV_EXT_STR_ID(settings_reset_factory_confirm), NULL, RESET_CONFIRM);
    view->reset_confirmation = RESET_CONFIRM;
    view->reset_popup_serial = current->popup_serial;
}

static bool reset_action(settings_page_t *view, int target)
{
    if (!view || !current->foreground) return true;
    system_reset_status_t state;
    system_reset_get_status(&state);
    if (current->popup)
    {
        if (target == POPUP_CLOSE || target == NAV_BACK)
        {
            view->reset_confirmation = 0;
            popup_close();
            settings_resume();
            return true;
        }
        if (!view->reset_confirmation || target != view->reset_confirmation || current->popup_serial != view->reset_popup_serial)
            return true;
        view->reset_confirmation = 0;
        popup_close();
        if (target == RESET_CONFIRM && state.phase == SYSTEM_RESET_IDLE && !state.factory_pending)
        {
            view->reset_error = system_reset_start(&view->reset_target);
            view->reset_start_failed = view->reset_error != RT_EOK;
        }
        else if (target == RESET_FINISH_CONFIRM && reset_can_finish(&state))
            view->reset_error = system_reset_finish_without_card();
        settings_resume();
        return true;
    }
    if (target == RESET_FACTORY || target == RESET_NETWORK || target == RESET_CARD)
    {
        if (state.phase == SYSTEM_RESET_IDLE && !state.factory_pending && !view->reset_error && !storage_changing())
            reset_prepare(view, target == RESET_FACTORY ? SYSTEM_RESET_FACTORY :
                          target == RESET_NETWORK ? SYSTEM_RESET_NETWORK : SYSTEM_RESET_CARD);
        return true;
    }
    if (target == RESET_RETRY)
    {
        if (view->reset_prepare_failed && state.phase == SYSTEM_RESET_IDLE)
            reset_prepare(view, view->reset_target.kind);
        else
        {
            if (view->reset_start_failed && state.phase == SYSTEM_RESET_IDLE)
            {
                view->reset_error = system_reset_start(&view->reset_target);
                view->reset_start_failed = view->reset_error != RT_EOK;
            }
            else if (state.phase == SYSTEM_RESET_FAILED || state.phase == SYSTEM_RESET_WAIT_CARD)
                view->reset_error = system_reset_retry();
            settings_resume();
        }
        return true;
    }
    if (target == RESET_FINISH)
    {
        if (reset_can_finish(&state))
        {
            popup_confirm_i18n(LV_EXT_STR_ID(settings_reset_finish_confirm), NULL, RESET_FINISH_CONFIRM);
            view->reset_confirmation = RESET_FINISH_CONFIRM;
            view->reset_popup_serial = current->popup_serial;
        }
        return true;
    }
    if (target == RESET_DISMISS)
    {
        if (state.phase == SYSTEM_RESET_DONE || (!state.factory_pending &&
            (state.phase == SYSTEM_RESET_IDLE || state.phase == SYSTEM_RESET_FAILED)))
        {
            system_reset_dismiss();
            view->reset_error = RT_EOK;
            view->reset_prepare_failed = view->reset_start_failed = false;
            settings_resume();
        }
        return true;
    }
    if (target == RESET_CONFIRM || target == RESET_FINISH_CONFIRM) return true;
    return false;
}

static ui_settings_list_row_t setting_item(ui_setting_id_t id, char *value, size_t size)
{
    const ui_setting_t *setting = ui_settings_item(id);
    ui_settings_list_row_t row = {.key=SETTING_CHANGE_BASE + id, .title=ui_settings_label(id), .enabled=true};
    row.kind = setting->is_switch ? UI_SETTINGS_ROW_SWITCH : UI_SETTINGS_ROW_LINK;
    row.checked = ui_settings_enabled(id);
    if (!setting->is_switch)
    {
        snprintf(value, size, "%s", ui_settings_value(id));
        row.value = value;
    }
    return row;
}

static void timezone_text(int minutes, char *text, size_t size)
{
    unsigned absolute = minutes < 0 ? (unsigned)-minutes : (unsigned)minutes;
    snprintf(text, size, "UTC%c%02u:%02u", minutes < 0 ? '-' : '+', absolute / 60, absolute % 60);
}

static void timezone_rows(settings_page_t *view)
{
    system_time_settings_t settings;
    system_time_get_settings(&settings);
    unsigned count = (840 + 720) / 15 + 1;
    bool extra = settings.timezone_minutes % 15 != 0;
    if (extra) ++count;
    ui_settings_list_row_t *rows = lv_malloc_zeroed(count * sizeof(*rows));
    char (*names)[16] = lv_malloc(count * sizeof(*names));
    if (!rows || !names)
    { lv_free(rows); lv_free(names); current->locale_retry = true; return; }
    for (unsigned i = 0; i < count; ++i)
    {
        int minutes = extra && i == count - 1 ? settings.timezone_minutes : -720 + (int)i * 15;
        timezone_text(minutes, names[i], sizeof(names[i]));
        rows[i] = (ui_settings_list_row_t)CHOICE_ROW(TIMEZONE_BASE + minutes + 720, names[i], minutes == settings.timezone_minutes);
    }
    settings_rows(view, rows, count);
    lv_free(names); lv_free(rows);
}

static const char *frontlight_mode_name(frontlight_mode_t mode)
{
    if (mode == FRONTLIGHT_COOL) return epd_get_str(settings_frontlight_cool);
    if (mode == FRONTLIGHT_WARM) return epd_get_str(settings_frontlight_warm);
    return epd_get_str(settings_frontlight_mixed);
}

static void option_rows(settings_page_t *view)
{
    const ui_setting_t *setting = ui_settings_item(view->option);
    text_update(view->title, ui_settings_label(view->option));
    ui_settings_list_row_t *rows = lv_malloc_zeroed(setting->option_count * sizeof(*rows));
    char (*names)[64] = lv_malloc(setting->option_count * sizeof(*names));
    if (!rows || !names)
    { lv_free(rows); lv_free(names); current->locale_retry = true; return; }
    for (unsigned i = 0; i < setting->option_count; ++i)
    {
        snprintf(names[i], sizeof(names[i]), "%s", ui_settings_option(view->option, i));
        rows[i] = (ui_settings_list_row_t)CHOICE_ROW(OPTION_BASE + (int)i, names[i], i == ui_settings_index(view->option));
        if (view->option == UI_SETTING_NAVIGATION && i == 1)
            rows[i].value = epd_get_str(settings_config_navigation_help);
    }
    settings_rows(view, rows, setting->option_count);
    lv_free(names); lv_free(rows);
}

static void information_rows(settings_page_t *view)
{
    system_info_t info;
    if (system_info_read(&info) || !view->info_valid)
    { view->info = info; view->info_valid = true; }
    else info = view->info;
    if (current->id == UI_PAGE_ABOUT)
    {
        char rtos[32];
        snprintf(rtos, sizeof(rtos), "%ld.%ld.%ld", RT_VERSION, RT_SUBVERSION, RT_REVISION);
        ui_settings_list_row_t rows[] = {
            INFO(-1, epd_get_str(settings_product), info.product),
            INFO(-2, epd_get_str(settings_model), info.model),
            INFO(-3, epd_get_str(settings_ui_version), info.ui_version),
            INFO(-4, epd_get_str(settings_rtos_version), rtos),
            INFO(-5, epd_get_str(settings_sdk_version), info.sdk[0] ? info.sdk : "--"),
            INFO(-6, epd_get_str(settings_build_date), info.build_date),
            LINK(UI_PAGE_STATUS_INFO, epd_get_str(settings_status), NULL)
        };
        for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        {
            if (rows[i].kind == UI_SETTINGS_ROW_INFO) rows[i].kind = UI_SETTINGS_ROW_INLINE_INFO;
            rows[i].no_separator = true;
        }
        SHOW(rows);
    }
    else
    {
        char addresses[SYSTEM_INFO_ADDRESS_CAPACITY * (IPADDR_STRLEN_MAX + 1) + 1];
        addresses[0] = 0;
        for (unsigned i = 0; i < info.address_count; ++i)
        {
            if (i) strcat(addresses, "\n");
            strcat(addresses, info.addresses[i]);
        }
        char uptime[96];
        EPD_FMT_settings_uptime_value(uptime, sizeof(uptime), (unsigned)(info.uptime_minutes / 1440),
                                     (unsigned)(info.uptime_minutes / 60 % 24), (unsigned)(info.uptime_minutes % 60));
        ui_settings_list_row_t rows[] = {
            INFO(-1, epd_get_str(settings_ip), addresses[0] ? addresses : "--"),
            INFO(-2, epd_get_str(settings_wifi_mac), info.mac[0] ? info.mac : "--"),
            INFO(-3, epd_get_str(settings_bt_addr), info.bt[0] ? info.bt : "--"),
            INFO(-4, epd_get_str(settings_uptime), uptime)
        };
        SHOW(rows);
    }
}

void settings_resume(void)
{
    if (current && settings_wireless_handles(current->id)) { settings_wireless_resume(); return; }
    settings_page_t *view = settings_page();
    if (!view || !view->list) return;
    if (current->popup) { current->locale_retry = true; return; }
    if (view->name_editor) { settings_layout(view); return; }
    settings_layout(view);
    if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value()))
        current->locale_retry = true;
    switch (current->id)
    {
    case UI_PAGE_SETTINGS:
    {
        ui_settings_list_row_t rows[] = {
            LINK(UI_PAGE_NETWORK_SETTINGS, epd_get_str(settings_wireless), &ui_icon_wifi_entry),
            LINK(UI_PAGE_DISPLAY_SETTINGS, epd_get_str(settings_display), &ui_icon_settings_display),
            LINK(UI_PAGE_LOCK_OPTIONS, epd_get_str(settings_lock_wallpaper), &ui_icon_settings_lock),
            LINK(UI_PAGE_SOUND, epd_get_str(settings_sound), &ui_icon_volume),
            LINK(UI_PAGE_APP_MANAGEMENT, epd_get_str(settings_apps), &ui_icon_settings_apps),
            LINK(UI_PAGE_MORE_SETTINGS, epd_get_str(settings_more), &ui_icon_settings_more),
            DISABLED(-2, epd_get_str(settings_update), &ui_icon_settings_update),
            DISABLED(-3, epd_get_str(settings_manual), &ui_icon_settings_manual),
            LINK(UI_PAGE_ABOUT, epd_get_str(settings_about), &ui_icon_settings_about)
        };
        for (unsigned i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i)
        {
            bool last = i + 1 == sizeof(rows)/sizeof(rows[0]);
            rows[i].min_height = epd_ui_touch(last ? 60 : 110);
            rows[i].icon_size = epd_ui_px(60);
            rows[i].no_chevron = true;
            rows[i].bottom_gap = epd_ui_px(last ? 0 : 50);
        }
        SHOW(rows); break;
    }
    case UI_PAGE_DISPLAY_SETTINGS:
    {
        char refresh[64], brightness[16], navigation[64];
        frontlight_snapshot_t light;
        frontlight_get(&light);
        if (light.output_valid) snprintf(brightness, sizeof(brightness), "%u%%", (unsigned)light.output_percent);
        else strcpy(brightness, "--");
        ui_settings_list_row_t rows[] = {
            {.key=UI_PAGE_FRONTLIGHT, .kind=UI_SETTINGS_ROW_LINK,
             .title=epd_get_str(settings_frontlight), .value=brightness, .enabled=light.available},
            {.key=UI_PAGE_FRONTLIGHT_MODE, .kind=UI_SETTINGS_ROW_LINK,
             .title=epd_get_str(settings_frontlight_mode), .value=frontlight_mode_name(light.mode),
             .enabled=light.available && light.mode_available && light.preference_valid},
            {.key=UI_PAGE_UI_SCALE, .kind=UI_SETTINGS_ROW_LINK,
             .title=ui_settings_label(UI_SETTING_UI_SCALE), .value=ui_settings_value(UI_SETTING_UI_SCALE), .enabled=true},
            setting_item(UI_SETTING_FULL_REFRESH, refresh, sizeof(refresh)),
            setting_item(UI_SETTING_NAVIGATION, navigation, sizeof(navigation))
        };
        SHOW(rows); break;
    }
    case UI_PAGE_FRONTLIGHT_MODE:
    {
        frontlight_snapshot_t light;
        frontlight_get(&light);
        const frontlight_mode_t modes[] = {FRONTLIGHT_COOL, FRONTLIGHT_WARM, FRONTLIGHT_MIXED};
        ui_settings_list_row_t rows[3];
        for (unsigned i = 0; i < 3; ++i)
        {
            rows[i] = (ui_settings_list_row_t)CHOICE_ROW(OPTION_BASE + modes[i],
                frontlight_mode_name(modes[i]), light.mode == modes[i]);
            rows[i].enabled = light.available && light.mode_available && light.preference_valid &&
                              !light.locked && !light.suspended;
        }
        SHOW(rows); break;
    }
    case UI_PAGE_TEXT_SETTINGS:
    {
        ui_settings_list_row_t rows[UI_SETTING_ENCODING - UI_SETTING_FONT + 2];
        char values[UI_SETTING_ENCODING - UI_SETTING_FONT + 2][64];
        rows[0] = setting_item(UI_SETTING_TOUCH, values[0], sizeof(values[0]));
        for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
            rows[i - UI_SETTING_FONT + 1] = setting_item(i, values[i - UI_SETTING_FONT + 1], sizeof(values[0]));
        reading_font_info_t font;
        bool found = reading_font_get(reading_font_selected(), &font);
        rows[1] = (ui_settings_list_row_t){.key=UI_PAGE_READING_FONTS, .kind=UI_SETTINGS_ROW_LINK,
            .title=ui_settings_label(UI_SETTING_FONT), .value=found && font.available ? font.name : "MiSans",
            .enabled=true};
        SHOW(rows); break;
    }
    case UI_PAGE_NETWORK_SETTINGS:
    {
        ui_settings_list_row_t rows[] = {
            LINK(UI_PAGE_WIFI_SETTINGS, "WLAN", &ui_icon_wifi_entry),
            LINK(UI_PAGE_BLUETOOTH_SETTINGS, ui_settings_label(UI_SETTING_BLUETOOTH), &ui_icon_bt_entry),
            LINK(UI_PAGE_USB_SETTINGS, "USB", &ui_icon_usb),
            LINK(UI_PAGE_NETWORK_STATUS, epd_get_str(settings_network), &ui_icon_settings_network)
        };
        for (unsigned i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i)
        {
            rows[i].no_chevron = true;
            bool last = i + 1 == sizeof(rows)/sizeof(rows[0]);
            rows[i].min_height = epd_ui_touch(last ? 60 : 110);
            rows[i].icon_size = epd_ui_px(60);
            rows[i].bottom_gap = epd_ui_px(last ? 0 : 50);
        }
        SHOW(rows); break;
    }
    case UI_PAGE_NETWORK_STATUS:
    {
        network_snapshot_t network;
        network_snapshot(&network);
        ui_settings_list_row_t rows[] = {
            INFO(-2, epd_get_str(settings_active_network), !network.ready ? epd_get_str(settings_no_network) :
                 network.source == NETWORK_WIFI ? "WLAN" : "Bluetooth PAN")
        };
        SHOW(rows); break;
    }
    case UI_PAGE_USB_SETTINGS:
    {
        usb_card_reader_status_t usb;
        usb_card_reader_status(&usb);
        const char *state = !usb.supported ? epd_get_str(settings_usb_unavailable) :
            usb.error ? epd_get_str(platform_setting_failed) :
            usb.busy ? epd_get_str(settings_usb_preparing) :
            usb.ready ? epd_get_str(settings_usb_shared) :
            usb.enabled ? epd_get_str(settings_usb_waiting) :
            !usb.connected ? epd_get_str(settings_no_network) :
            !storage_available(STORAGE_SD) ? epd_get_str(platform_insert_card) : epd_get_str(settings_usb_local);
        ui_settings_list_row_t rows[] = {
            {.key=USB_CARD_READER, .kind=UI_SETTINGS_ROW_SWITCH,
             .title=epd_get_str(settings_usb_card_reader), .checked=usb.enabled,
             .enabled=usb.supported && !usb.busy && (usb.enabled || (usb.connected && storage_available(STORAGE_SD)))},
            INFO(-1, epd_get_str(settings_status), state)
        };
        SHOW(rows); break;
    }
    case UI_PAGE_APP_MANAGEMENT:
    {
        ui_settings_list_row_t rows[] = {
            LINK(UI_PAGE_APP_INSTALL, epd_get_str(platform_title_install), NULL),
            LINK(UI_PAGE_APP_UNINSTALL, epd_get_str(platform_title_uninstall), NULL),
            LINK(UI_PAGE_APP_SETTINGS, epd_get_str(platform_title_app_settings), NULL),
            LINK(LOAD_CARD_APPS, epd_get_str(platform_load_card_apps), NULL)
        };
        SHOW(rows); break;
    }
    case UI_PAGE_MORE_SETTINGS:
    {
        ui_settings_list_row_t rows[] = {
            LINK(UI_PAGE_AI_SETTINGS, epd_get_str(ai_title), NULL),
            LINK(UI_PAGE_SPEECH_SETTINGS, epd_get_str(speech_title), NULL),
            LINK(UI_PAGE_LANGUAGE_SETTINGS, epd_get_str(platform_title_language), NULL),
            LINK(UI_PAGE_MEMORY, epd_get_str(platform_title_memory), NULL),
            LINK(UI_PAGE_TIME_SETTINGS, epd_get_str(settings_time), NULL),
            LINK(UI_PAGE_STORAGE, epd_get_str(platform_title_storage), NULL),
            LINK(UI_PAGE_RESET_SETTINGS, epd_get_str(settings_reset), NULL)
        };
        SHOW(rows); break;
    }
    case UI_PAGE_LOCK_OPTIONS:
    {
        char timeout[64], sleep[64];
        ui_settings_list_row_t rows[] = {
            LINK(UI_PAGE_LOCK_SETTINGS, epd_get_str(shell_lockscreen), NULL),
            LINK(UI_PAGE_WALLPAPER, epd_get_str(wallpaper_title), NULL),
            setting_item(UI_SETTING_TIMEOUT, timeout, sizeof(timeout)),
            setting_item(UI_SETTING_SYSTEM_SLEEP, sleep, sizeof(sleep))
        };
        SHOW(rows); break;
    }
    case UI_PAGE_TIME_SETTINGS:
    {
        system_time_settings_t settings;
        system_time_get_settings(&settings);
        char time[48], zone[16];
        system_time_format(system_time_get_utc(), epd_locale_get(), true, time, sizeof(time));
        timezone_text(settings.timezone_minutes, zone, sizeof(zone));
        ui_settings_list_row_t rows[] = {
            SWITCH_ROW(TIME_AUTOMATIC, epd_get_str(settings_time_auto), settings.automatic),
            {.key=UI_PAGE_TIME_EDIT, .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(settings_time), .value=time, .enabled=!settings.automatic},
            {.key=UI_PAGE_TIMEZONE_SETTINGS, .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(settings_timezone), .value=zone, .enabled=true},
            SWITCH_ROW(TIME_HOUR24, epd_get_str(settings_hour24), settings.hour24)
        };
        SHOW(rows); break;
    }
    case UI_PAGE_RESET_SETTINGS: reset_rows(view); break;
    case UI_PAGE_TIMEZONE_SETTINGS: timezone_rows(view); break;
    case UI_PAGE_SETTING_OPTIONS: option_rows(view); break;
    case UI_PAGE_ABOUT: case UI_PAGE_STATUS_INFO: information_rows(view); break;
    default: break;
    }
    view->poll_tick = lv_tick_get();
}

bool settings_action(int *request)
{
    if (settings_wireless_handles(current->id)) return settings_wireless_action(request);
    int target = *request;
    if (current->id == UI_PAGE_RESET_SETTINGS) return reset_action(settings_page(), target);
    if (current->popup) return false;
    if (current->id == UI_PAGE_ABOUT)
    {
        settings_page_t *view = settings_page();
        if (target == DEVICE_RENAME) { device_name_edit(view, true); return true; }
        if (view->name_editor)
        {
            if (target == DEVICE_NAME_CANCEL || target == NAV_BACK) device_name_edit(view, false);
            else if (target == DEVICE_NAME_SAVE)
            {
                const char *name = lv_textarea_get_text(view->name_input);
                rt_err_t result = device_identity_set_name(name);
                if (result != RT_EOK)
                    popup_open_i18n(result == -RT_EINVAL ? LV_EXT_STR_ID(settings_name_invalid) :
                                   LV_EXT_STR_ID(platform_setting_failed), NULL);
                else
                {
                    rt_err_t bluetooth = btpan_set_local_name(name);
                    text_update(view->device_name, name);
                    device_name_edit(view, false);
                    settings_resume();
                    if (bluetooth != RT_EOK && bluetooth != -RT_ENOSYS)
                        popup_open_i18n(LV_EXT_STR_ID(settings_name_bluetooth_failed), NULL);
                }
            }
            return true;
        }
    }
    ui_page_id_t page = current->id;
    bool ok;
    if (page == UI_PAGE_USB_SETTINGS && target == USB_CARD_READER)
    {
        usb_card_reader_status_t usb;
        usb_card_reader_status(&usb);
        ok = !app_installer_busy() && !board_power_pending() &&
             usb_card_reader_set_enabled(!usb.enabled);
        settings_resume();
        if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
        return true;
    }
    if (page == UI_PAGE_LANGUAGE_SETTINGS && (target == LANGUAGE_CHINESE || target == LANGUAGE_ENGLISH))
    {
        if (!epd_locale_set(target == LANGUAGE_CHINESE ? "zh_cn" : "en_us"))
            popup_open_i18n(LV_EXT_STR_ID(platform_language_save_failed), NULL);
        else { language_settings_resume(); page_retranslate(); }
        return true;
    }
    if (page == UI_PAGE_APP_MANAGEMENT && target == LOAD_CARD_APPS)
    {
        if (!storage_available(STORAGE_SD)) popup_open_i18n(LV_EXT_STR_ID(platform_insert_card), NULL);
        else app_card_request();
        return true;
    }
    if (page == UI_PAGE_TIME_SETTINGS && (target == TIME_AUTOMATIC || target == TIME_HOUR24))
    {
        system_time_settings_t settings;
        system_time_get_settings(&settings);
        ok = target == TIME_AUTOMATIC ? system_time_set_automatic(!settings.automatic) : system_time_set_hour24(!settings.hour24);
    }
    else if (page == UI_PAGE_TIMEZONE_SETTINGS && target >= TIMEZONE_BASE && target <= TIMEZONE_BASE + 1560)
        ok = system_time_set_timezone(target - TIMEZONE_BASE - 720);
    else if (page == UI_PAGE_FRONTLIGHT_MODE && target >= OPTION_BASE && target < OPTION_BASE + FRONTLIGHT_MODE_COUNT)
        ok = frontlight_set_mode((frontlight_mode_t)(target - OPTION_BASE));
    else if (page == UI_PAGE_SETTING_OPTIONS && settings_page() && target >= OPTION_BASE &&
             target < OPTION_BASE + (int)ui_settings_item(settings_page()->option)->option_count)
        ok = ui_settings_set(settings_page()->option, target - OPTION_BASE);
    else if (target >= SETTING_CHANGE_BASE && target < SETTING_CHANGE_BASE + UI_SETTING_COUNT)
    {
        ui_setting_id_t id = target - SETTING_CHANGE_BASE;
        bool allowed = (page == UI_PAGE_TEXT_SETTINGS && (id == UI_SETTING_TOUCH || (id >= UI_SETTING_FONT && id <= UI_SETTING_ENCODING))) ||
                       (page == UI_PAGE_DISPLAY_SETTINGS && (id == UI_SETTING_FULL_REFRESH || id == UI_SETTING_NAVIGATION)) ||
                       (page == UI_PAGE_LOCK_OPTIONS && (id == UI_SETTING_TIMEOUT || id == UI_SETTING_SYSTEM_SLEEP)) ||
                       (page == UI_PAGE_BLUETOOTH_SETTINGS && id == UI_SETTING_BLUETOOTH);
        if (!allowed) return true;
        if (id == UI_SETTING_FONT) { *request = UI_PAGE_READING_FONTS; return false; }
        if (!ui_settings_item(id)->is_switch)
        { option_to_open = id; *request = UI_PAGE_SETTING_OPTIONS; return false; }
        ok = ui_settings_cycle(id);
    }
    else return false;
    settings_resume();
    if (!ok) popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
    return true;
}

void settings_process(void)
{
    if (!current || !current->foreground) return;
    if (settings_wireless_handles(current->id)) { settings_wireless_process(); return; }
    app_list_process();
    lock_settings_process();
    memory_page_process();
    settings_time_edit_process();
    frontlight_page_process();
    sound_page_process();
    scale_page_process();
    settings_page_t *view = settings_page();
    if (!view || !view->list || current->popup) return;
    if (view->bottom != ui_shell_content_bottom()) settings_layout(view);
    if (current->id == UI_PAGE_RESET_SETTINGS)
    {
        system_reset_status_t state;
        system_reset_get_status(&state);
        if (lv_refreshing_done() && (!view->reset_valid || view->reset_revision != state.revision ||
            view->reset_locale != epd_locale_revision() || view->reset_scale != epd_ui_scale_revision()))
            settings_resume();
        return;
    }
    if (lv_tick_elaps(view->poll_tick) >= 1000 && lv_refreshing_done() &&
        (current->id == UI_PAGE_TIME_SETTINGS || current->id == UI_PAGE_STATUS_INFO ||
         current->id == UI_PAGE_NETWORK_STATUS || current->id == UI_PAGE_BLUETOOTH_SETTINGS ||
         current->id == UI_PAGE_USB_SETTINGS || current->id == UI_PAGE_TEXT_SETTINGS ||
         current->id == UI_PAGE_DISPLAY_SETTINGS)) settings_resume();
}

void settings_stop(void)
{
    if (current && settings_wireless_handles(current->id)) { settings_wireless_stop(); return; }
    settings_page_t *view = settings_page();
    if (!view) return;
    if (view->list) ui_settings_list_destroy(view->list);
    lv_free(view);
    current->feature = NULL;
}

void settings_restore_focus(void)
{
    if (current && settings_wireless_handles(current->id)) { settings_wireless_restore_focus(); return; }
    settings_page_t *view = settings_page();
    if (!view) return;
    if (view->name_editor)
    {
        lv_group_add_obj(current->group, view->name_input);
        for (unsigned i = 1; i <= 2; ++i)
            lv_group_add_obj(current->group, lv_obj_get_child(view->name_editor, i));
    }
    else
    {
        if (view->rename) lv_group_add_obj(current->group, view->rename);
        if (view->list) ui_settings_list_set_group(view->list, current->group);
    }
}

static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("settings", UI_PAGE_SETTINGS);
}
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(platform_app_settings), &ui_icon_settings_app, "settings", app_main, 1);

void settings_create(lv_obj_t *screen) { settings_list_create(screen); }
void display_settings_create(lv_obj_t *screen) { settings_list_create(screen); }
void frontlight_mode_create(lv_obj_t *screen) { settings_list_create(screen); }
void network_settings_create(lv_obj_t *screen) { settings_list_create(screen); }
void wifi_settings_create(lv_obj_t *screen) { settings_wireless_create(screen); }
void app_management_create(lv_obj_t *screen) { settings_list_create(screen); }
void text_settings_create(lv_obj_t *screen) { settings_list_create(screen); }
void about_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_more_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_time_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_timezone_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_status_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_lock_options_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_network_status_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_bluetooth_create(lv_obj_t *screen) { settings_wireless_create(screen); }
void settings_usb_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_reset_create(lv_obj_t *screen) { settings_list_create(screen); }
void settings_options_create(lv_obj_t *screen) { settings_list_create(screen); }

void language_settings_resume(void)
{
    bool english = strcmp(epd_locale_get(), "en_us") == 0;
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *option = current->language_options[i];
        if (!option) continue;
        lv_obj_set_pos(option, MARGIN, epd_ui_px(194) + (int)i * epd_ui_px(108));
        lv_obj_set_size(option, CONTENT_WIDTH, epd_ui_touch(80));
        lv_obj_t *label = lv_obj_get_child(option, 0);
        lv_obj_set_style_text_font(label, ui_font_settings_body(), 0);
        lv_obj_set_size(label, CONTENT_WIDTH - epd_ui_px(96), ui_font_settings_body()->line_height);
        lv_obj_set_pos(label, epd_ui_px(72), (epd_ui_touch(80) - ui_font_settings_body()->line_height) / 2);
        lv_obj_t *radio = lv_obj_get_child(option, 1);
        lv_obj_set_size(radio, epd_ui_px(24), epd_ui_px(24));
        lv_obj_set_pos(radio, epd_ui_px(24), (epd_ui_touch(80) - epd_ui_px(24)) / 2);
        if ((i == 1) == english) lv_obj_add_state(option, LV_STATE_CHECKED);
        else lv_obj_remove_state(option, LV_STATE_CHECKED);
    }
}

void language_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, LV_EXT_STR_ID(platform_title_language), ui_font_settings_title());
    static const char *const names[] = {"简体中文", "English"};
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *row = button_create(screen, MARGIN, 194 + (int)i * 108,
                                     CONTENT_WIDTH, 80, NULL, i ? LANGUAGE_ENGLISH : LANGUAGE_CHINESE);
        current->language_options[i] = row;
        label_create(row, names[i], 72, 20, CONTENT_WIDTH - 96, ui_font_settings_body());
        lv_obj_t *radio = panel_create(row, 24, 28, 24, 24);
        lv_obj_remove_flag(radio, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(radio, LV_OBJ_FLAG_STATE_TRICKLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_STATE_TRICKLE);
        lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(radio, lv_color_black(), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(radio, lv_color_black(), 0);
    }
    language_settings_resume();
}
