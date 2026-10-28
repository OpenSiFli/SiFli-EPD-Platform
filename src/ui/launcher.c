#include "app_service_internal.h"
#include "storage_internal.h"
#include "platform/epd_locale_internal.h"
#include "ui_internal.h"
#include "apps/app_wallpaper.h"
#include "storage.h"
#include "usb_card_reader.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/epd_app.h"
#include "platform/epd_input.h"
#include "platform/epd_refresh.h"
#include "platform/app_card.h"
#include "ui_shell.h"
#include "ui_popup_style.h"
#include "system_reset_guard.h"
#include "boards/board_service.h"
#include "portal.h"
#include "reading_font.h"
#include "apps/reading_fonts.h"

static bool page_input_ready(uint32_t key);
static bool page_input_move(const epd_input_event_t *events, unsigned count);
static bool page_input_key(uint32_t key);
static const epd_input_ops_t page_input_ops = {page_input_ready, page_input_move, page_input_key};

/* Only the foreground context is bound between callbacks; app_fwk owns all contexts. */
page_context_t *current;
launcher_status_t status;
static lv_style_t epd_style;
int pending_page = NAV_IDLE;
static bool initialized;
static unsigned saved_focus[UI_PAGE_COUNT];
static void page_lifecycle(ui_page_id_t id, gui_app_msg_type_t message, void *memory);

lv_ext_str_id_t page_title_id(ui_page_id_t page)
{
    switch (page)
    {
    case UI_PAGE_HOME: return LV_EXT_STR_ID(platform_title_home);
    case UI_PAGE_FILES: return LV_EXT_STR_ID(platform_app_files);
    case UI_PAGE_PORTAL: return LV_EXT_STR_ID(platform_app_portal);
    case UI_PAGE_PORTAL_WEB: return LV_EXT_STR_ID(portal_web_title);
    case UI_PAGE_SETTINGS: return LV_EXT_STR_ID(platform_app_settings);
    case UI_PAGE_TEXT_SETTINGS: return LV_EXT_STR_ID(platform_title_fonts);
    case UI_PAGE_READING_FONTS: return LV_EXT_STR_ID(reading_fonts_title);
    case UI_PAGE_ABOUT: return LV_EXT_STR_ID(settings_about);
    case UI_PAGE_LOCK: return LV_EXT_STR_ID(platform_title_lock);
    case UI_PAGE_DISPLAY_SETTINGS: return LV_EXT_STR_ID(settings_display);
    case UI_PAGE_NETWORK_SETTINGS: return LV_EXT_STR_ID(settings_wireless);
    case UI_PAGE_STORAGE: return LV_EXT_STR_ID(platform_title_storage);
    case UI_PAGE_APP_MANAGEMENT: return LV_EXT_STR_ID(platform_title_apps);
    case UI_PAGE_WIFI_SETTINGS: return LV_EXT_STR_ID(platform_title_wifi);
    case UI_PAGE_APP_INSTALL: return LV_EXT_STR_ID(platform_title_install);
    case UI_PAGE_APP_UNINSTALL: return LV_EXT_STR_ID(platform_title_uninstall);
    case UI_PAGE_APP_SETTINGS: return LV_EXT_STR_ID(platform_title_app_settings);
    case UI_PAGE_MEMORY: return LV_EXT_STR_ID(platform_title_memory);
    case UI_PAGE_LANGUAGE_SETTINGS: return LV_EXT_STR_ID(platform_title_language);
    case UI_PAGE_LOCK_SETTINGS: return LV_EXT_STR_ID(shell_lockscreen);
    case UI_PAGE_MORE_SETTINGS: return LV_EXT_STR_ID(settings_more);
    case UI_PAGE_TIME_SETTINGS:
    case UI_PAGE_TIME_EDIT: return LV_EXT_STR_ID(settings_time);
    case UI_PAGE_TIMEZONE_SETTINGS: return LV_EXT_STR_ID(settings_timezone);
    case UI_PAGE_STATUS_INFO: return LV_EXT_STR_ID(settings_status);
    case UI_PAGE_LOCK_OPTIONS: return LV_EXT_STR_ID(settings_lock_wallpaper);
    case UI_PAGE_WALLPAPER: return LV_EXT_STR_ID(wallpaper_title);
    case UI_PAGE_FILE_PICKER: return LV_EXT_STR_ID(wallpaper_choose);
    case UI_PAGE_AI_SETTINGS: return LV_EXT_STR_ID(ai_title);
    case UI_PAGE_SPEECH_SETTINGS: return LV_EXT_STR_ID(speech_title);
    case UI_PAGE_AI_SELECT: return LV_EXT_STR_ID(ai_select);
    case UI_PAGE_NETWORK_STATUS: return LV_EXT_STR_ID(settings_network);
    case UI_PAGE_BLUETOOTH_SETTINGS: return LV_EXT_STR_ID(platform_setting_bluetooth);
    case UI_PAGE_USB_SETTINGS: return LV_EXT_STR_ID(settings_usb);
    case UI_PAGE_FRONTLIGHT: return LV_EXT_STR_ID(settings_frontlight);
    case UI_PAGE_FRONTLIGHT_MODE: return LV_EXT_STR_ID(settings_frontlight_mode);
    case UI_PAGE_SOUND: return LV_EXT_STR_ID(settings_sound);
    case UI_PAGE_SOUND_VOLUME: return sound_volume_title_id();
    case UI_PAGE_SOUND_TONE: return sound_tone_title_id();
    case UI_PAGE_UI_SCALE: return LV_EXT_STR_ID(settings_config_scale);
    case UI_PAGE_RESET_SETTINGS: return LV_EXT_STR_ID(settings_reset);
    default: return LV_EXT_STR_ID(platform_title_home);
    }
}

void epd_obj_init(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &epd_style, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

typedef struct
{
    char *text;
} label_text_t;

static void label_text_delete(lv_event_t *event)
{
    label_text_t *saved = lv_event_get_user_data(event);
    lv_free(saved->text);
    lv_free(saved);
}

bool text_update(lv_obj_t *label, const char *text)
{
    if (!label || !text) return true;

    label_text_t *saved = NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(label); ++i)
    {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == label_text_delete)
        {
            saved = lv_event_dsc_get_user_data(event);
            break;
        }
    }
    /* LV_LABEL_LONG_DOT can modify the label's copy of the original text. */
    if (saved && saved->text && strcmp(saved->text, text) == 0) return true;

    char *copy = lv_strdup(text);
    if (!copy) { if (current) current->locale_retry = true; return false; }
    if (!saved)
    {
        saved = lv_malloc(sizeof(*saved));
        if (!saved)
        {
            lv_free(copy);
            if (current) current->locale_retry = true;
            return false;
        }
        saved->text = NULL;
        if (!lv_obj_add_event_cb(label, label_text_delete, LV_EVENT_DELETE, saved))
        {
            lv_free(copy);
            lv_free(saved);
            if (current) current->locale_retry = true;
            return false;
        }
    }
    lv_label_set_text(label, copy);
    lv_free(saved->text);
    saved->text = copy;
    return true;
}

lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    /* Configure geometry with no text so font and size changes do not repeatedly
     * measure the default label or the business string. */
    lv_label_set_text(label, "");
    epd_obj_init(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x222222), 0);
    lv_obj_set_pos(label, x, y);
    if (width > 0)
    {
        lv_obj_set_width(label, width);
        lv_obj_set_height(label, font->line_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    text_update(label, text);
    return label;
}

lv_obj_t *text_bind(lv_obj_t *label, lv_ext_str_id_t id)
{
    if (!label) return NULL;
    for (unsigned i = 0; i < current->text_binding_count; ++i)
    {
        if (current->text_bindings[i].label == label)
        {
            current->text_bindings[i].id = id;
            text_update(label, LV_EXT_STR_GET(id));
            return label;
        }
    }
    if (current->text_binding_count >= TEXT_BINDING_CAPACITY)
    {
        current->locale_retry = true;
        return NULL;
    }
    text_binding_t *binding = &current->text_bindings[current->text_binding_count++];
    binding->label = label;
    binding->id = id;
    text_update(label, LV_EXT_STR_GET(id));
    return label;
}

lv_obj_t *label_create_i18n(lv_obj_t *parent, lv_ext_str_id_t id, int x, int y,
                          int width, const lv_font_t *font)
{
    return text_bind(label_create(parent, "", x, y, width, font), id);
}

lv_obj_t *centered_label_i18n(lv_obj_t *parent, lv_ext_str_id_t id, int x, int y,
                            int width, const lv_font_t *font)
{
    return text_bind(centered_label(parent, "", x, y, width, font), id);
}

lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *icon = lv_image_create(parent);
    epd_obj_init(icon);
    lv_image_set_src(icon, src);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    epd_obj_init(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    return panel;
}

unsigned launcher_focus_index(void)
{
    if (!current || !current->group) return 0;
    lv_obj_t *focused = lv_group_get_focused(current->group);
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (current->focus_items[i] == focused) return i;
    return 0;
}

static void clicked(lv_event_t *event)
{
    if (storage_changing()) return;
    if (!current || !current->foreground ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen) return;
    if (pending_page != NAV_IDLE) return;
    int target = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target_obj(event);
    lv_group_focus_obj(button);
    pending_page = target;
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *text, int target)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    /* Use an inset outline so focus never changes the content coordinates. */
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_group_add_obj(current->group, button);
    LV_ASSERT(current->focus_count < FOCUS_CAPACITY);
    current->focus_items[current->focus_count++] = button;
    if (text)
    {
        lv_obj_t *label = label_create(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    return button;
}

lv_obj_t *button_create_i18n(lv_obj_t *parent, int x, int y, int width, int height,
                           lv_ext_str_id_t id, int target)
{
    lv_obj_t *button = button_create(parent, x, y, width, height, "", target);
    if (!text_bind(lv_obj_get_child(button, 0), id)) return NULL;
    return button;
}

lv_obj_t *icon_button_create(lv_obj_t *parent, int x, int y,
                                    const lv_image_dsc_t *src, int target)
{
    lv_obj_t *button = button_create(parent, x, y, ICON_BUTTON_SIZE, ICON_BUTTON_SIZE, NULL, target);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = icon_create(button, src, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);
    return button;
}

static bool page_settings_style(ui_page_id_t page)
{
    return page != UI_PAGE_HOME && page != UI_PAGE_LOCK && page != UI_PAGE_FILES &&
           page != UI_PAGE_PORTAL && page != UI_PAGE_PORTAL_WEB;
}

static void page_header_layout(void)
{
    bool settings = page_settings_style(current->id);
    const lv_font_t *font = settings ? ui_font_settings_title() : ui_font_title();
    int32_t x = settings ? SETTINGS_TITLE_X : PAGE_TITLE_X;
    int32_t y = settings ? SETTINGS_TITLE_Y : PAGE_TITLE_Y;
    if (current->page_title)
    {
        lv_obj_set_style_text_font(current->page_title, font, 0);
        lv_obj_set_pos(current->page_title, x, y);
        lv_obj_set_size(current->page_title, lv_obj_get_width(current->screen) - MARGIN - x, font->line_height);
    }
    if (current->back_button)
    {
        epd_app_back_button_layout(current->back_button, epd_ui_px(24),
                                  y + font->line_height / 2);
    }
}

lv_obj_t *page_title_create(lv_obj_t *screen, lv_ext_str_id_t id, const lv_font_t *font)
{
    lv_obj_t *title = label_create_i18n(screen, id, PAGE_TITLE_X, PAGE_TITLE_Y, WIDTH - MARGIN - PAGE_TITLE_X, font);
    if (current && current->screen == screen) current->page_title = title;
    if (current && current->screen == screen) page_header_layout();
    return title;
}

void image_update(lv_obj_t *icon, const lv_image_dsc_t *src)
{
    if (lv_image_get_src(icon) != src) lv_image_set_src(icon, src);
}

void launcher_set_status(const launcher_status_t *new_status)
{
    status = *new_status;
    status.time_text[sizeof(status.time_text) - 1] = '\0';
    status.clock_text[sizeof(status.clock_text) - 1] = '\0';
    status.recent_book[sizeof(status.recent_book) - 1] = '\0';
    status.weather[sizeof(status.weather) - 1] = '\0';
    ui_shell_status_changed();
}

void epd_app_header(lv_obj_t *screen)
{
    ui_shell_page_mode(screen, false);
}

void epd_app_header_refresh(lv_obj_t *screen)
{
    if (screen == lv_screen_active()) ui_shell_status_changed();
}

lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font)
{
    lv_obj_t *label = label_create(parent, text, x, y, width, font);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

void object_visible(lv_obj_t *object, bool visible)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

bool focus_eligible(lv_obj_t *object)
{
    return epd_input_focusable(object);
}

void focus_restore(unsigned index)
{
    if (index < current->focus_count && focus_eligible(current->focus_items[index]))
    {
        lv_group_focus_obj(current->focus_items[index]);
        return;
    }
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (focus_eligible(current->focus_items[i]))
        {
            lv_group_focus_obj(current->focus_items[i]);
            return;
        }
}

void popup_close(void)
{
    if (!current->popup) return;
    current->text_binding_count = current->popup_binding_start;
    lv_obj_delete(current->popup);
    current->popup = NULL;
    current->popup_text = current->popup_button = current->popup_cancel = current->popup_detail = NULL;
    while (current->focus_count > current->popup_focus_start)
        current->focus_items[--current->focus_count] = NULL;
    for (unsigned i = 0; i < current->focus_count; ++i) lv_group_add_obj(current->group, current->focus_items[i]);
    settings_restore_focus();
    app_list_restore_focus();
    memory_page_restore_focus();
    files_restore_focus();
    wallpaper_page_restore_focus();
    ui_ai_settings_restore_focus();
    ui_speech_settings_restore_focus();
    lock_settings_restore_focus();
    sound_page_restore_focus();
    reading_fonts_restore_focus();
    if (focus_eligible(current->popup_previous_focus)) lv_group_focus_obj(current->popup_previous_focus);
    else focus_restore(0);
    current->popup_previous_focus = NULL;
}

static void popup_outside_clicked(lv_event_t *event)
{
    if (current && current->popup &&
        lv_event_get_target(event) == lv_event_get_current_target(event) &&
        current->popup_button && !lv_obj_has_flag(current->popup_button, LV_OBJ_FLAG_HIDDEN))
        pending_page = POPUP_CLOSE;
}

static void popup_layout(void)
{
    if (!current->popup || !current->popup_text || !current->popup_button) return;
    lv_obj_t *dialog = lv_obj_get_parent(current->popup_button);
    ui_popup_style_dialog(dialog);
    int32_t width = LV_MIN(epd_ui_px(576), lv_obj_get_width(current->screen) - 2 * MARGIN);
    int32_t padding = epd_ui_px(24), gap = epd_ui_px(16);
    int32_t button_height = epd_ui_touch(64);
    lv_obj_set_style_text_font(current->popup_text, ui_font_body(), 0);
    lv_obj_set_pos(current->popup_text, padding, padding);
    lv_obj_set_width(current->popup_text, width - 2 * padding);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(current->popup_text), ui_font_body(), 0, 0,
                     width - 2 * padding, LV_TEXT_FLAG_NONE);
    int32_t text_height = LV_MAX(size.y, ui_font_body()->line_height);
    int32_t bottom = ui_shell_content_bottom();
    text_height = LV_MIN(text_height, LV_MAX(ui_font_body()->line_height,
        bottom - 2 * MARGIN - 2 * padding - button_height - 3 * gap - 2 * ui_font_small()->line_height));
    lv_obj_set_height(current->popup_text, text_height);
    int32_t y = padding + text_height + gap;
    if (current->popup_detail)
    {
        lv_obj_set_style_text_font(current->popup_detail, ui_font_small(), 0);
        lv_obj_set_pos(current->popup_detail, padding, y);
        lv_obj_set_size(current->popup_detail, width - 2 * padding, 2 * ui_font_small()->line_height);
        y += 2 * ui_font_small()->line_height + gap;
    }
    int32_t button_width = width - 2 * padding;
    if (current->popup_cancel) button_width = (button_width - gap) / 2;
    lv_obj_t *buttons[] = {current->popup_button, current->popup_cancel};
    for (unsigned i = 0; i < 2; ++i)
    {
        if (!buttons[i]) continue;
        ui_popup_style_button(buttons[i]);
        lv_obj_set_pos(buttons[i], padding + (i == 0 && current->popup_cancel ? button_width + gap : 0), y);
        lv_obj_set_size(buttons[i], button_width, button_height);
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        lv_obj_set_style_text_font(label, ui_font_body(), 0);
        lv_obj_set_size(label, button_width - epd_ui_px(24), ui_font_body()->line_height);
        lv_obj_center(label);
    }
    int32_t height = y + button_height + padding;
    lv_obj_set_size(dialog, width, height);
    lv_obj_set_pos(dialog, (lv_obj_get_width(current->screen) - width) / 2, (bottom - height) / 2);
}

void popup_open(const char *text, const char *detail)
{
    if (!current || ui_shell_locked() || current->popup) return;
    ++current->popup_serial;
    current->popup_binding_start = current->text_binding_count;
    current->popup_focus_start = current->focus_count;
    current->popup_previous_focus = lv_group_get_focused(current->group);
    lv_group_remove_all_objs(current->group);
    current->popup = panel_create(lv_screen_active(), 0, 0, WIDTH, HEIGHT);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_color(current->popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(current->popup, popup_outside_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dialog = panel_create(current->popup, 54, 438, 576, 282);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    ui_popup_style_dialog(dialog);
    current->popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    if (detail) current->popup_detail = centered_label(dialog, detail, 24, 140, 528, ui_font_small());
    lv_obj_set_height(current->popup_text, detail ? 88 : 96);
    lv_label_set_long_mode(current->popup_text, LV_LABEL_LONG_WRAP);
    current->popup_button = button_create_i18n(dialog, 108, 184, 360, 64, LV_EXT_STR_ID(platform_ok), POPUP_CLOSE);
    popup_layout();
    lv_group_focus_obj(current->popup_button);
}

void popup_confirm(const char *text, const char *detail, int target)
{
    if (!current || ui_shell_locked() || current->popup) return;
    popup_open(text, detail);
    lv_obj_t *button = current->popup_button;
    lv_obj_set_x(button, 306);
    lv_obj_set_width(button, 222);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_width(label, 198);
    lv_obj_center(label);
    lv_obj_remove_event_cb(button, clicked);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    lv_obj_t *cancel = button_create_i18n(lv_obj_get_parent(button), 48, 184, 222, 64, LV_EXT_STR_ID(platform_cancel), POPUP_CLOSE);
    current->popup_cancel = cancel;
    popup_layout();
    lv_group_focus_obj(cancel);
}

void popup_open_i18n(lv_ext_str_id_t id, const char *detail)
{
    if (!current || ui_shell_locked() || current->popup) return;
    popup_open("", detail);
    text_bind(current->popup_text, id);
    popup_layout();
}

void popup_confirm_i18n(lv_ext_str_id_t id, const char *detail, int target)
{
    if (!current || ui_shell_locked() || current->popup) return;
    popup_confirm("", detail, target);
    text_bind(current->popup_text, id);
    popup_layout();
}

void page_retranslate(void)
{
    if (!current || !current->foreground) return;
    uint32_t revision = epd_locale_revision();
    uint32_t scale = epd_ui_scale_revision();
    if (current->locale_revision == revision && current->scale_revision == scale && !current->locale_retry) return;
    current->locale_retry = false;
    if (current->scale_revision != scale)
    {
        lv_obj_set_style_text_font(current->screen, ui_font_body(), 0);
        page_header_layout();
    }
    for (unsigned i = 0; i < current->text_binding_count; ++i)
    {
        const text_binding_t *binding = &current->text_bindings[i];
        text_update(binding->label, LV_EXT_STR_GET(binding->id));
    }
    popup_layout();
    switch (current->id)
    {
    case UI_PAGE_SETTINGS:
    case UI_PAGE_TEXT_SETTINGS:
    case UI_PAGE_DISPLAY_SETTINGS:
    case UI_PAGE_NETWORK_SETTINGS:
    case UI_PAGE_WIFI_SETTINGS:
    case UI_PAGE_ABOUT:
    case UI_PAGE_MORE_SETTINGS:
    case UI_PAGE_TIME_SETTINGS:
    case UI_PAGE_TIMEZONE_SETTINGS:
    case UI_PAGE_STATUS_INFO:
    case UI_PAGE_LOCK_OPTIONS:
    case UI_PAGE_NETWORK_STATUS:
    case UI_PAGE_BLUETOOTH_SETTINGS:
    case UI_PAGE_USB_SETTINGS:
    case UI_PAGE_SETTING_OPTIONS:
    case UI_PAGE_APP_MANAGEMENT: settings_resume(); break;
    case UI_PAGE_LANGUAGE_SETTINGS: language_settings_resume(); break;
    case UI_PAGE_LOCK_SETTINGS: lock_settings_resume(); break;
    case UI_PAGE_SOUND:
    case UI_PAGE_SOUND_VOLUME: sound_page_resume(); break;
    case UI_PAGE_FILES:
    case UI_PAGE_FILE_PICKER:
    case UI_PAGE_STORAGE: files_retranslate(); break;
    case UI_PAGE_APP_INSTALL:
    case UI_PAGE_APP_UNINSTALL:
    case UI_PAGE_APP_SETTINGS: app_list_retranslate(); break;
    case UI_PAGE_MEMORY: memory_page_resume(); break;
    case UI_PAGE_UI_SCALE: scale_page_process(); break;
    case UI_PAGE_PORTAL:
    case UI_PAGE_PORTAL_WEB: portal_page_resume(); break;
    default: break;
    }
    if (!current->locale_retry)
    { current->locale_revision = revision; current->scale_revision = scale; }
}

static void page_create(void)
{
    lv_obj_t *screen = current->screen;
    ui_page_id_t id = current->id;
    epd_obj_init(screen);
    lv_obj_set_size(screen, WIDTH, HEIGHT);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, ui_font_body(), 0);
    if (id != UI_PAGE_LOCK) epd_app_header(screen);
    else ui_shell_page_mode(screen, true);
    ui_page_operations(id)->create(screen);
    if (id != UI_PAGE_HOME && id != UI_PAGE_LOCK)
        current->back_button = icon_button_create(screen, MARGIN,
                           BACK_BUTTON_Y,
                           &ui_icon_back, NAV_BACK);
    page_header_layout();
}

static void page_lifecycle(ui_page_id_t id, gui_app_msg_type_t message, void *memory)
{
    page_context_t *context = memory;
    page_context_t *previous = current;
    current = context;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
        context->id = id;
        context->screen = lv_screen_active();
        context->saved_focus = saved_focus[id];
        context->group = epd_app_input_group(context->screen);
        LV_ASSERT_MALLOC(context->group);
        epd_input_set_ops(context->group, &page_input_ops);
        page_create();
        if (!context->locale_retry)
        {
            context->locale_revision = epd_locale_revision();
            context->scale_revision = epd_ui_scale_revision();
        }
        context->initial_focus_pending = true;
        current = previous;
        break;
    case GUI_APP_MSG_ONRESUME:
        context->foreground = true;
        if (ui_page_operations(id)->resume) ui_page_operations(id)->resume();
        page_retranslate();
        if (context->initial_focus_pending)
        {
            context->initial_focus_pending = false;
            focus_restore(context->saved_focus);
        }
        if (context->focus_count && !lv_group_get_focused(context->group))
            lv_group_focus_obj(context->focus_items[context->saved_focus < context->focus_count ?
                                                   context->saved_focus : 0]);
        break;
    case GUI_APP_MSG_ONPAUSE:
        context->saved_focus = launcher_focus_index();
        saved_focus[id] = context->saved_focus;
        if (ui_page_operations(id)->pause) ui_page_operations(id)->pause();
        context->foreground = false;
        /* Page polling is foreground-only; independent services keep running. */
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        if (ui_page_operations(id)->stop) ui_page_operations(id)->stop();
        context->foreground = false;
        /* Screen deletion releases the input group, dialogs and framework context. */
        current = previous == context ? NULL : previous;
        break;
    default:
        current = previous;
        break;
    }
}

bool launcher_init(void)
{
    if (initialized) return true;
    if (!ui_font_init()) return false;
    if (!epd_locale_init()) return false;
    /* Use static, flat styles without the default theme's interaction effects. */
    lv_display_set_theme(NULL, NULL);
    lv_style_init(&epd_style);
    lv_style_set_anim_duration(&epd_style, 0);
    lv_style_set_bg_grad_dir(&epd_style, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad(&epd_style, NULL);
    lv_style_set_shadow_width(&epd_style, 0);
    ui_ai_settings_init();
    initialized = ui_navigation_init(page_lifecycle, sizeof(page_context_t));
    if (initialized)
    {
        ui_shell_init();
        (void)reading_font_init();
    }
    return initialized;
}

ui_page_id_t launcher_current_page(void)
{
    return current ? current->id : UI_PAGE_COUNT;
}

void launcher_open(ui_page_id_t page)
{
    if (system_reset_blocks_apps() && page != UI_PAGE_RESET_SETTINGS) return;
    if (!current) return;
    if (initialized && pending_page == NAV_IDLE && page >= UI_PAGE_HOME && page < UI_PAGE_COUNT)
        pending_page = page;
}

static bool page_input_ready(uint32_t key)
{
    (void)key;
    return initialized && current && current->foreground && pending_page == NAV_IDLE;
}

static bool page_input_move(const epd_input_event_t *events, unsigned count)
{
    (void)events;
    (void)count;
    return launcher_current_page() == UI_PAGE_LOCK;
}

static bool page_input_key(uint32_t key)
{
    if (launcher_current_page() == UI_PAGE_LOCK)
    {
        return true;
    }
    if (key == LV_KEY_ESC)
    {
        if (current->popup) pending_page = POPUP_CLOSE;
        else if (launcher_current_page() != UI_PAGE_HOME) pending_page = NAV_BACK;
        return true;
    }
    return false;
}

void launcher_process(void)
{
    if (!initialized) return;
    (void)reading_font_init();
    if (board_power_pending())
    {
        app_service_maintenance_process();
        if (lv_refreshing_done()) board_power_process();
        ui_shell_process();
        return;
    }
    app_installer_process();
    portal_process();
    app_catalog_process();
    home_process();
    lock_process();
    ui_shell_process();
    if (!current || !current->foreground) return;
    if (pending_page == NAV_IDLE)
    {
        page_retranslate();
        settings_process();
        portal_page_process();
    }
    if (pending_page == NAV_IDLE) return;
    int target = pending_page;
    pending_page = NAV_IDLE;
    const ui_page_ops_t *ops = ui_page_operations(launcher_current_page());
    if (ops->action && ops->action(&target)) return;
    if (target == POPUP_CLOSE)
    {
        popup_close();
        return;
    }
    if (system_reset_blocks_apps() && target != UI_PAGE_RESET_SETTINGS) return;
    bool navigated = target == NAV_BACK ? ui_navigation_back() :
                     ui_navigation_open((ui_page_id_t)target);
    if (!navigated)
    {
        if (current && current->foreground)
            popup_open_i18n(target == NAV_BACK ? LV_EXT_STR_ID(platform_back_failed) : LV_EXT_STR_ID(platform_page_open_failed), NULL);
    }
}

bool launcher_storage_process(void)
{
    static uint32_t observed_revision;
    static uint32_t releasing_id;
    static bool release_acknowledged;
    static bool release_saved;
    app_card_process();
    uint32_t request_id = storage_release_request_id();
    if (request_id)
    {
        if (!lv_refreshing_done()) return false;
        if (releasing_id != request_id)
        {
            files_release_storage();
            reading_fonts_release_storage();
        }
        /* Finish cancelling font work before the persistence barrier and
         * release TF files held by previews and paused readers. */
        if (!reading_font_release_storage()) return false;
        if (releasing_id != request_id)
        {
            releasing_id = request_id;
            release_acknowledged = false;
            release_saved = false;
            pending_page = NAV_IDLE;
            ui_shell_storage_changed();
            if (current && current->id == UI_PAGE_HOME) home_pause();
            app_catalog_changed();
        }
        if (!release_acknowledged)
        {
            if (!release_saved && storage_release_for_export())
            {
                /* Intentional handoff keeps the TF session usable through saves
                 * and module exit. Physical removal cannot preserve access. */
                if (!app_service_flush())
                {
                    if (!storage_release_timed_out()) return false;
                    (void)usb_card_reader_set_enabled(false);
                    popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
                    return false;
                }
                release_saved = true;
            }
            if (!app_catalog_release_storage()) return storage_release_timed_out();
            release_acknowledged = storage_release_complete(releasing_id) == RT_EOK;
        }
        return false;
    }
    releasing_id = 0;
    release_acknowledged = false;
    if (storage_changing()) return false;
    uint32_t next_revision = storage_revision();
    if (next_revision != observed_revision)
    {
        observed_revision = next_revision;
        reading_font_storage_changed();
        app_installer_storage_changed();
        app_catalog_changed();
        if (current && current->foreground)
        {
            switch (current->id)
            {
            case UI_PAGE_FILES: files_storage_changed(); break;
            case UI_PAGE_STORAGE: storage_page_resume(); break;
            case UI_PAGE_APP_INSTALL:
            case UI_PAGE_APP_UNINSTALL:
            case UI_PAGE_APP_SETTINGS: app_list_storage_changed(); break;
            default: break;
            }
            if (!focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
        }
    }
    return true;
}
