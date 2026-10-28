#include "storage_internal.h"
#include "storage_file_internal.h"
#include "platform/epd_locale.h"
#include "app_card.h"
#include "system_reset_guard.h"
#include "app_catalog.h"
#include "app_installer.h"
#include "epd_app.h"
#include "epd_input.h"
#include "epd_i18n.h"
#include "epd_power.h"
#include "epd_refresh.h"
#include "ui_shell.h"
#include "ui_popup_style.h"
#include "sleep_guard.h"
#include "src/misc/lv_text_private.h"
#include <dfs_posix.h>

static lv_obj_t *modal, *message, *cancel_button, *confirm_button;
static uint32_t prompt_session, answered_session, scanned_session, scanned_revision;
static uint32_t language_revision, scale_revision;
static bool requested, loading;
static sleep_guard_owner_t loading_hold = {.id = "app-card"};
typedef enum { CARD_PROMPT, CARD_EMPTY, CARD_LOADED, CARD_LOADING, CARD_FAILED } card_message_t;
static card_message_t message_kind;

static void layout(void)
{
    lv_obj_t *dialog = lv_obj_get_parent(message);
    const lv_font_t *font = epd_app_font();
    int32_t screen_width = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_height = lv_display_get_vertical_resolution(NULL);
    int32_t padding = epd_ui_px(24), gap = epd_ui_px(16);
    int32_t width = LV_MIN(epd_ui_px(576), screen_width - 2 * padding);
    int32_t button_height = epd_ui_touch(64);
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(message), font, 0, 0,
                     width - 2 * padding, LV_TEXT_FLAG_NONE);
    int32_t text_height = LV_MAX(font->line_height, text_size.y);
    int32_t button_y = padding + text_height + gap;
    int32_t height = button_y + button_height + padding;
    lv_obj_set_size(modal, screen_width, screen_height);
    lv_obj_set_size(dialog, width, height);
    lv_obj_center(dialog);
    ui_popup_style_dialog(dialog);
    lv_obj_set_pos(message, padding, padding);
    lv_obj_set_size(message, width - 2 * padding, text_height);
    lv_obj_set_style_text_font(message, font, 0);
    lv_obj_t *buttons[] = {cancel_button, confirm_button};
    int32_t button_width = (width - 2 * padding - gap) / 2;
    for (unsigned i = 0; i < 2; ++i)
    {
        ui_popup_style_button(buttons[i]);
        lv_obj_set_pos(buttons[i], padding + (int32_t)i * (button_width + gap), button_y);
        lv_obj_set_size(buttons[i], button_width, button_height);
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_size(label, button_width - epd_ui_px(24), font->line_height);
        lv_obj_center(label);
    }
    scale_revision = epd_ui_scale_revision();
}

static void refresh(void)
{
    if (!ui_shell_locked() && !epd_power_transitioning()) epd_refresh_request_interaction();
}

static const char *message_text(void)
{
    switch (message_kind)
    {
    case CARD_EMPTY: return epd_get_str(platform_card_empty);
    case CARD_LOADED: return epd_get_str(platform_card_loaded);
    case CARD_LOADING: return epd_get_str(platform_card_loading);
    case CARD_FAILED: return epd_get_str(platform_card_failed);
    default: return epd_get_str(platform_card_prompt);
    }
}

static void update_text(void)
{
    bool ok = epd_app_text(message, message_text());
    ok = epd_app_text(lv_obj_get_child(cancel_button, 0), epd_get_str(platform_card_cancel)) && ok;
    ok = epd_app_text(lv_obj_get_child(confirm_button, 0), epd_get_str(platform_card_load)) && ok;
    language_revision = ok ? epd_locale_revision() : 0;
    layout();
    refresh();
}

static void dismiss(void)
{
    if (modal) lv_obj_delete(modal);
    modal = message = cancel_button = confirm_button = NULL;
    loading = false;
    sleep_guard_release(&loading_hold);
    epd_input_clear();
    refresh();
}

void app_card_suspend(void)
{
    if (modal && !lv_obj_has_flag(modal, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
        epd_input_clear();
    }
}

static void answer(lv_event_t *event)
{
    bool accept = (uintptr_t)lv_event_get_user_data(event) != 0;
    if (ui_shell_locked() || epd_power_transitioning() || loading) return;
    answered_session = prompt_session;
    if (!accept || !storage_session_valid(STORAGE_SD, prompt_session)) { dismiss(); return; }
    if (!sleep_guard_acquire(&loading_hold) || !storage_card_enable_apps(prompt_session)) { dismiss(); return; }
    /* Gate execution again until migration and journal recovery have finished. */
    app_installer_storage_changed();
    app_installer_process();
    loading = true;
    message_kind = CARD_LOADING;
    update_text();
    lv_obj_t *dialog = lv_obj_get_parent(message);
    for (unsigned i = 1; i < lv_obj_get_child_count(dialog); ++i)
        lv_obj_add_flag(lv_obj_get_child(dialog, i), LV_OBJ_FLAG_HIDDEN);
    epd_input_clear();
}

static bool has_apps(uint32_t session)
{
    bool found = false;
    storage_lock();
    DIR *dir = storage_session_valid(STORAGE_SD, session) ? opendir(STORAGE_SD_APPS) : NULL;
    struct dirent *file;
    epd_app_entry_t entry;
    while (dir && (file = readdir(dir)) != NULL)
        if (app_package_read(STORAGE_SD_APPS, file->d_name, &entry) && !entry.resources_only)
        { found = true; break; }
    if (dir) closedir(dir);
    storage_unlock();
    return found && storage_session_valid(STORAGE_SD, session);
}

void app_card_request(void) { requested = true; }

void app_card_process(void)
{
    bool blocked = ui_shell_locked() || epd_power_transitioning();
    if (blocked) app_card_suspend();
    if (system_reset_blocks_apps())
    {
        if (modal) dismiss();
        requested = false;
        return;
    }
    if (!epd_app_refresh_done()) return;
    uint32_t session = storage_card_session();
    if (modal && !storage_session_valid(STORAGE_SD, prompt_session)) dismiss();
    if (modal && !blocked && (language_revision != epd_locale_revision() ||
                             scale_revision != epd_ui_scale_revision())) update_text();
    if (modal && !blocked && lv_obj_has_flag(modal, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_HIDDEN);
        refresh();
        epd_input_clear();
    }
    if (loading)
    {
        if (app_installer_volume_preparing(STORAGE_SD) || app_installer_busy()) return;
        if (app_installer_volume_ready(STORAGE_SD))
        {
            app_catalog_changed();
            dismiss();
        }
        else
        {
            loading = false;
            sleep_guard_release(&loading_hold);
            message_kind = CARD_FAILED;
            update_text();
            lv_obj_t *dialog = lv_obj_get_parent(message);
            for (unsigned i = 1; i < lv_obj_get_child_count(dialog); ++i)
                lv_obj_remove_flag(lv_obj_get_child(dialog, i), LV_OBJ_FLAG_HIDDEN);
        }
        if (loading || !modal) return;
    }
    if (blocked) return;
    if (modal || storage_changing() || app_installer_busy()) return;
    if (!storage_available(STORAGE_SD)) { requested = false; return; }
    if (!requested && (answered_session == session || storage_apps_enabled(STORAGE_SD))) return;
    uint32_t revision = app_catalog_revision();
    if (!requested && scanned_session == session && scanned_revision == revision) return;
    scanned_session = session;
    scanned_revision = revision;
    bool found = has_apps(session);
    if (!storage_session_valid(STORAGE_SD, session)) return;
    bool enabled = storage_apps_enabled(STORAGE_SD) && app_installer_volume_ready(STORAGE_SD);
    if (!found && !requested) return;
    requested = false;
    prompt_session = session;
    /* System overlay survives app_fwk navigation and owns its input scope. */
    modal = epd_app_panel(lv_layer_top(), 0, 0, 684, 1216);
    lv_obj_set_style_bg_opa(modal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    epd_app_input_group(modal);
    lv_obj_t *dialog = epd_app_panel(modal, 54, 438, 576, 282);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    message_kind = enabled ? CARD_LOADED : found ? CARD_PROMPT : CARD_EMPTY;
    message = epd_app_centered_label(dialog, message_text(),
                                    24, 44, 528, epd_app_font());
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(message, 116);
    lv_obj_t *cancel = epd_app_button(dialog, 48, 184, 222, 64, epd_get_str(platform_card_cancel), answer, NULL);
    lv_obj_t *confirm = epd_app_button(dialog, 306, 184, 222, 64, epd_get_str(platform_card_load), answer, (void *)1);
    cancel_button = cancel;
    confirm_button = confirm;
    update_text();
    if (!found || enabled) lv_obj_add_state(confirm, LV_STATE_DISABLED);
    epd_app_input_back(modal, cancel);
    lv_group_focus_obj(cancel);
    epd_input_clear();
}
