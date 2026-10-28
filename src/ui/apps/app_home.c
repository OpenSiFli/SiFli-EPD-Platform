#include "storage.h"
#include "storage_internal.h"
#include "epd_memory.h"
#include "bf0_hal.h"
#include "platform/app_image.h"
#include "platform/epd_locale.h"
#include "ui_internal.h"
#include "ui_home_model.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/app_recovery.h"
#include "platform/epd_app.h"
#include "ui_shell.h"
#include "storage_file.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

#define HOME_APP_BASE (-440)
#define HOME_MENU_BASE (-480)
#define HOME_PREVIOUS (-410)
#define HOME_NEXT (-411)
#define HOME_PIN (-412)
#define HOME_SETTINGS (-413)
#define HOME_UNINSTALL (-414)
#define HOME_REMOVE_CONFIRM (-415)
#define HOME_SLOT_COUNT (UI_HOME_GRID_CAPACITY + UI_HOME_DOCK_CAPACITY)
#define HOME_DOCK_PATH STORAGE_FLASH_ROOT "/system/home_dock.bin"
#define HOME_DOCK_MAGIC 0x4B434F44u
#define HOME_DOCK_VERSION 1u
#define HOME_ICON_CACHE_BYTES (256u * 1024u)
#define HOME_ICON_CACHE_RESERVE (1024u * 1024u)

typedef char home_id_capacity_matches_sdk[
    UI_HOME_ID_CAPACITY == GUI_APP_ID_MAX_LEN ? 1 : -1];

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    lv_obj_t *tile, *image, *label;
    lv_image_dsc_t cached_icon;
    bool icon_ready, file_icon;
} home_slot_t;

typedef struct
{
    ui_home_model_t model;
    home_slot_t slots[HOME_SLOT_COUNT];
    lv_obj_t *dock, *menu_icon;
    lv_font_t *name_font, *font_fallback;
    unsigned page, grid_count, font_percent;
    uint32_t revision, locale_revision, selected_session, selected_revision, scale_revision;
    uint32_t storage_revision, card_session;
    size_t cache_bytes;
    lv_timer_t *cache_timer;
    int32_t content_bottom;
    uint32_t ticket, menu_serial, result_serial;
    app_job_message_t result_message;
    epd_app_entry_t selected;
    bool ready;
} home_state_t;

static unsigned saved_home_page;

static void home_layout(home_state_t *home);
static void home_menu_layout(home_state_t *home);

static void home_slot_release(home_state_t *home, home_slot_t *slot)
{
    epd_app_image_release(slot->image);
    if (slot->cached_icon.data)
    {
        lv_draw_wait_for_finish();
        lv_image_set_src(slot->image, NULL);
        lv_image_cache_drop(&slot->cached_icon);
        lv_image_header_cache_drop(&slot->cached_icon);
        home->cache_bytes -= slot->cached_icon.data_size;
        epd_app_free((void *)slot->cached_icon.data);
        memset(&slot->cached_icon, 0, sizeof(slot->cached_icon));
    }
    slot->icon_ready = slot->file_icon = false;
}

static void home_icons_release(home_state_t *home)
{
    for (unsigned i = 0; i < HOME_SLOT_COUNT; ++i) home_slot_release(home, &home->slots[i]);
}

static bool home_sources_changed(const home_state_t *home)
{
    return home->revision != app_catalog_revision() ||
           home->storage_revision != storage_revision() || home->card_session != storage_card_session();
}

/* Runs on the UI thread even while another app owns current. */
static void home_cache_trim(lv_timer_t *timer)
{
    home_state_t *home = lv_timer_get_user_data(timer);
    if (storage_changing() || storage_release_requested() || home_sources_changed(home) ||
        app_installer_busy() || epd_app_psram_available() < HOME_ICON_CACHE_RESERVE)
    {
        home_icons_release(home);
        lv_timer_pause(timer);
    }
}

static void home_slot_retain(home_state_t *home, home_slot_t *slot)
{
    if (!slot->file_icon || slot->cached_icon.data) return;
    const void *source = lv_image_get_src(slot->image);
    if (!source || lv_image_src_get_type(source) != LV_IMAGE_SRC_VARIABLE) return;
    const lv_image_dsc_t *image = source;
    if (!(image->header.flags & LV_IMAGE_FLAGS_EZIP) || !image->data || !image->data_size ||
        image->data_size > HOME_ICON_CACHE_BYTES - home->cache_bytes ||
        epd_app_psram_available() < image->data_size + HOME_ICON_CACHE_RESERVE) return;
    void *copy = epd_app_alloc(image->data_size, EPD_APP_PSRAM);
    if (!copy) return;
    memcpy(copy, image->data, image->data_size);
    slot->cached_icon = *image;
    slot->cached_icon.data = copy;
    slot->cached_icon.header.flags &= ~LV_IMAGE_FLAGS_ALLOCATED;
#ifdef PSRAM_CACHE_WB
    mpu_dcache_clean(copy, image->data_size);
#endif
    home->cache_bytes += image->data_size;
}

static void home_name_draw_extent(lv_event_t *event)
{
    const home_slot_t *slot = lv_event_get_user_data(event);
    if (!slot->label) return;
    /* Use the assigned widths: a paused page may still have its old coordinates. */
    int32_t extra = lv_obj_get_style_width(slot->label, 0) - lv_obj_get_style_width(slot->tile, 0);
    extra = LV_MAX(0, (extra + 1) / 2) + lv_obj_get_ext_draw_size(slot->label);
    lv_event_set_ext_draw_size(event, extra);
}

static void home_font_select(home_state_t *home)
{
    unsigned percent = epd_ui_scale_percent();
    if (home->font_percent == percent) return;
    uint32_t identity;
    lv_font_t *old = home->name_font, *fallback = home->font_fallback;
    home->name_font = ui_font_reader_create(0, epd_ui_px(32), 0, &identity, &home->font_fallback);
    home->font_percent = percent;
    for (unsigned i = 0; i < UI_HOME_GRID_CAPACITY; ++i)
        if (home->slots[i].label)
            lv_obj_set_style_text_font(home->slots[i].label, home->name_font ? home->name_font : ui_font_body(), 0);
    lv_draw_wait_for_finish();
    ui_font_reader_destroy(old);
    ui_font_reader_destroy(fallback);
}

static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("Main", UI_PAGE_HOME);
}
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(platform_title_home), NULL, "Main", app_main, 1);

static home_state_t *home_state(void)
{
    return current && current->id == UI_PAGE_HOME ? current->feature : NULL;
}

static const epd_app_entry_t *home_find(const char *id)
{
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && !strcmp(entry->id, id)) return entry;
    }
    return NULL;
}

static bool home_builtin(const char *id)
{
    const builtin_app_desc_t *item = gui_builtin_app_list_open();
    bool found = false;
    while (item)
    {
        if (!strcmp(item->id, id)) { found = true; break; }
        item = gui_builtin_app_list_get_next(item);
    }
    gui_builtin_app_list_close(item);
    return found;
}

static bool home_icon(lv_obj_t *image, const epd_app_entry_t *entry, unsigned size, bool *file_icon)
{
    char path[256];
    const char *file = NULL;
    const lv_image_dsc_t *fallback = &ui_icon_album;
    if (entry->icon)
    {
        if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_FILE) file = entry->icon;
        else fallback = entry->icon;
    }
    if (entry->icon_file[0] && storage_app_path(path, sizeof(path), entry->id,
                                               STORAGE_APP_CODE, entry->icon_file)) file = path;
    bool ok = epd_app_image_set(image, file, fallback);
    if (file_icon) *file_icon = file && ok;
    lv_obj_set_size(image, size, size);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    return ok;
}

static void home_slot_set(home_slot_t *slot, const epd_app_entry_t *entry, unsigned size)
{
    home_state_t *home = home_state();
    object_visible(slot->tile, entry != NULL);
    if (!entry)
    {
        slot->id[0] = 0;
        home_slot_release(home, slot);
        return;
    }
    if (strcmp(slot->id, entry->id)) home_slot_release(home, slot);
    snprintf(slot->id, sizeof(slot->id), "%s", entry->id);
    if (!slot->icon_ready)
        slot->icon_ready = home_icon(slot->image, entry, size, &slot->file_icon);
    if (slot->label) text_update(slot->label, entry->name);
}

static void home_update(bool reset_focus)
{
    home_state_t *home = home_state();
    if (!home) return;
    home->grid_count = 0;
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && !ui_home_model_contains(&home->model, entry->id)) ++home->grid_count;
    }
    home->page = ui_home_page_clamp(home->page, home->grid_count);
    saved_home_page = home->page;
    unsigned seen = 0, shown = 0;
    for (unsigned i = 0; i < app_catalog_count() && shown < UI_HOME_GRID_CAPACITY; ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (!entry || ui_home_model_contains(&home->model, entry->id)) continue;
        if (seen++ < home->page * UI_HOME_GRID_CAPACITY) continue;
        home_slot_set(&home->slots[shown++], entry, 125);
    }
    while (shown < UI_HOME_GRID_CAPACITY) home_slot_set(&home->slots[shown++], NULL, 125);

    unsigned dock_count = 0;
    for (unsigned i = 0; i < home->model.count; ++i)
        if (home_find(home->model.ids[i])) ++dock_count;
    shown = 0;
    for (unsigned i = 0; i < home->model.count; ++i)
    {
        const epd_app_entry_t *entry = home_find(home->model.ids[i]);
        if (!entry) continue;
        home_slot_t *slot = &home->slots[UI_HOME_GRID_CAPACITY + shown];
        lv_obj_set_x(slot->tile, ui_home_dock_x(shown, dock_count));
        home_slot_set(slot, entry, 100);
        ++shown;
    }
    while (shown < UI_HOME_DOCK_CAPACITY)
        home_slot_set(&home->slots[UI_HOME_GRID_CAPACITY + shown++], NULL, 100);

    unsigned pages = ui_home_page_count(home->grid_count);
    while (lv_obj_get_child_count(current->home_pager) > pages)
        lv_obj_delete(lv_obj_get_child(current->home_pager, -1));
    while (lv_obj_get_child_count(current->home_pager) < pages)
    {
        lv_obj_t *dot = panel_create(current->home_pager, 0, 0, 15, 15);
        if (!dot) break;
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
    }
    object_visible(current->home_pager, pages != 0);
    for (unsigned i = 0; i < lv_obj_get_child_count(current->home_pager); ++i)
    {
        lv_obj_t *dot = lv_obj_get_child(current->home_pager, i);
        unsigned span = WIDTH - 30;
        if (pages - 1 <= span / 35) span = (pages - 1) * 35;
        unsigned offset = pages > 1 ? (uint64_t)i * span / (pages - 1) : 0;
        lv_obj_set_x(dot, (WIDTH - (int)span - 15) / 2 + offset);
        lv_obj_set_style_bg_color(dot, i == home->page ? lv_color_black() : lv_color_hex(0xA6A3A3), 0);
    }
    home_layout(home);
    if (reset_focus || !focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
}

static void home_layout(home_state_t *home)
{
    home_font_select(home);
    int width = lv_obj_get_width(current->screen);
    int bottom = ui_shell_content_bottom();
    int margin = epd_ui_px(13), gap = epd_ui_px(12);
    int dock_h = epd_ui_px(125), dock_y = bottom - epd_ui_px(20) - dock_h;
    int dot_size = epd_ui_px(15), pager_y = dock_y - epd_ui_px(26) - dot_size;
    int top = epd_ui_px(81), pitch = (pager_y - epd_ui_px(18) - top) / 4;
    int cell_w = (width - 2 * margin) / 4;
    const lv_font_t *font = home->name_font ? home->name_font : ui_font_body();
    int side = LV_MIN(epd_ui_px(125), LV_MIN(cell_w - gap, pitch - font->line_height - gap));
    int icon_side = LV_MIN(epd_ui_px(100), side);
    for (unsigned i = 0; i < UI_HOME_GRID_CAPACITY; ++i)
    {
        home_slot_t *slot = &home->slots[i];
        lv_obj_set_pos(slot->tile, margin + (int)(i % 4) * cell_w + (cell_w - side) / 2,
                       top + (int)(i / 4) * pitch);
        lv_obj_set_size(slot->tile, side, side + font->line_height + epd_ui_px(5));
        lv_obj_set_size(slot->image, icon_side, icon_side);
        lv_obj_set_pos(slot->image, (side - icon_side) / 2, (side - icon_side) / 2);
        lv_obj_set_style_text_font(slot->label, font, 0);
        lv_obj_set_size(slot->label, cell_w - epd_ui_px(6), font->line_height + epd_ui_px(5));
        lv_obj_align(slot->label, LV_ALIGN_TOP_MID, 0, side);
        lv_obj_set_style_radius(slot->tile, epd_ui_px(16), 0);
        lv_obj_refresh_ext_draw_size(slot->tile);
    }
    lv_obj_set_pos(home->dock, margin, dock_y);
    lv_obj_set_size(home->dock, width - 2 * margin, dock_h);
    lv_obj_set_style_radius(home->dock, epd_ui_px(25), 0);
    lv_obj_set_style_shadow_width(home->dock, epd_ui_px(35), 0);
    lv_obj_set_style_shadow_ofs_y(home->dock, epd_ui_px(4), 0);
    unsigned count = 0;
    for (unsigned i = UI_HOME_GRID_CAPACITY; i < HOME_SLOT_COUNT; ++i)
        if (home->slots[i].id[0]) ++count;
    int dock_w = width - 2 * margin;
    int dock_side = LV_MIN(epd_ui_px(100), (dock_w - gap * ((int)count + 1)) / (int)LV_MAX(1u, count));
    int dock_gap = (dock_w - dock_side * (int)count) / ((int)count + 1);
    for (unsigned i = 0; i < UI_HOME_DOCK_CAPACITY; ++i)
    {
        home_slot_t *slot = &home->slots[UI_HOME_GRID_CAPACITY + i];
        lv_obj_set_pos(slot->tile, dock_gap + (int)i * (dock_side + dock_gap), (dock_h - dock_side) / 2);
        lv_obj_set_size(slot->tile, dock_side, dock_side);
        lv_obj_set_size(slot->image, dock_side, dock_side);
    }
    lv_obj_set_pos(current->home_pager, 0, pager_y);
    lv_obj_set_size(current->home_pager, width, dot_size);
    unsigned pages = lv_obj_get_child_count(current->home_pager);
    int span = LV_MIN((int)(pages ? pages - 1 : 0) * epd_ui_px(35), width - 2 * margin - dot_size);
    int diameter = pages > 1 ? LV_MIN(dot_size, LV_MAX(1, span / (int)(pages - 1) - 1)) : dot_size;
    for (unsigned i = 0; i < pages; ++i)
    {
        lv_obj_t *dot = lv_obj_get_child(current->home_pager, i);
        int offset = pages > 1 ? (int)((uint64_t)i * span / (pages - 1)) : 0;
        lv_obj_set_pos(dot, (width - span - diameter) / 2 + offset, (dot_size - diameter) / 2);
        lv_obj_set_size(dot, diameter, diameter);
    }
    home_menu_layout(home);
    home->scale_revision = epd_ui_scale_revision();
    home->content_bottom = bottom;
}

static bool home_save(const ui_home_model_t *next)
{
    home_state_t *home = home_state();
    if (!home || !storage_record_save(HOME_DOCK_PATH, HOME_DOCK_MAGIC, HOME_DOCK_VERSION,
                                      next, sizeof(*next))) return false;
    home->model = *next;
    return true;
}

static void home_tile_event(lv_event_t *event)
{
    if (!current || !current->foreground || !home_state() || current->popup ||
        storage_changing() || pending_page != NAV_IDLE) return;
    unsigned slot = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SHORT_CLICKED)
        pending_page = HOME_APP_BASE + (int)slot;
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        pending_page = HOME_MENU_BASE + (int)slot;
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev) lv_indev_wait_release(indev);
    }
}

static void home_gesture(lv_event_t *event)
{
    home_state_t *home = home_state();
    if (!home || !current->foreground || current->popup ||
        lv_event_get_current_target_obj(event) != current->screen) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev);
    if (pending_page != NAV_IDLE) return;
    if (direction == LV_DIR_LEFT && home->page + 1 < ui_home_page_count(home->grid_count))
        pending_page = HOME_NEXT;
    else if (direction == LV_DIR_RIGHT && home->page) pending_page = HOME_PREVIOUS;
}

static void home_popup_outside(lv_event_t *event)
{
    if (current && current->foreground && current->popup &&
        lv_event_get_target_obj(event) == current->popup && pending_page == NAV_IDLE)
        pending_page = POPUP_CLOSE;
}

static void home_menu_delete(lv_event_t *event)
{
    home_state_t *home = lv_event_get_user_data(event);
    home->menu_icon = NULL;
    home->menu_serial = 0;
}

static void home_menu(unsigned slot)
{
    home_state_t *home = home_state();
    if (!home || slot >= HOME_SLOT_COUNT || current->popup || home->ticket) return;
    const epd_app_entry_t *entry = home_find(home->slots[slot].id);
    if (!entry) return;
    home->selected = *entry;
    home->selected_session = storage_card_session();
    home->selected_revision = app_catalog_revision();
    bool pinned = ui_home_model_contains(&home->model, entry->id);
    bool builtin = home_builtin(entry->id);
    ++current->popup_serial;
    current->popup_binding_start = current->text_binding_count;
    current->popup_focus_start = current->focus_count;
    current->popup_previous_focus = lv_group_get_focused(current->group);
    lv_group_remove_all_objs(current->group);
    current->popup = panel_create(current->screen, 0, 0, WIDTH, HEIGHT);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_remove_flag(current->popup, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(current->popup, home_popup_outside, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(current->popup, home_menu_delete, LV_EVENT_DELETE, home);
    home->menu_serial = current->popup_serial;
    lv_obj_t *dialog = panel_create(current->popup, 82, 346, 520, 473);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_radius(dialog, 25, 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x828282), 0);
    home->menu_icon = icon_create(dialog, &ui_icon_album, 220, 12);
    home_icon(home->menu_icon, entry, 80, NULL);
    lv_obj_t *remove = button_create_i18n(dialog, 15, 105, 490, 100,
                                         LV_EXT_STR_ID(platform_home_uninstall), HOME_UNINSTALL);
    lv_obj_t *settings_button = button_create_i18n(dialog, 15, 230, 490, 100,
                                           LV_EXT_STR_ID(platform_home_settings), HOME_SETTINGS);
    lv_obj_t *pin = button_create_i18n(dialog, 15, 355, 490, 100,
        pinned ? LV_EXT_STR_ID(platform_home_unpin) : LV_EXT_STR_ID(platform_home_pin), HOME_PIN);
    lv_obj_t *buttons[] = {remove, settings_button, pin};
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        lv_obj_set_style_radius(buttons[i], 20, 0);
        lv_obj_set_style_bg_color(buttons[i], lv_color_hex(0xEAEAEA), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), lv_color_hex(0x999999), LV_STATE_DISABLED);
        lv_obj_set_style_text_font(lv_obj_get_child(buttons[i], 0), ui_font_title(), 0);
        lv_obj_set_height(lv_obj_get_child(buttons[i], 0), ui_font_title()->line_height);
        lv_obj_center(lv_obj_get_child(buttons[i], 0));
    }
    if (builtin || app_installer_busy()) lv_obj_add_state(remove, LV_STATE_DISABLED);
    if (!entry->has_settings) lv_obj_add_state(settings_button, LV_STATE_DISABLED);
    if (!pinned && home->model.count == UI_HOME_DOCK_CAPACITY) lv_obj_add_state(pin, LV_STATE_DISABLED);
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        if (lv_obj_has_state(buttons[i], LV_STATE_DISABLED))
            lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), lv_color_hex(0x999999), 0);
    current->popup_button = pin;
    home_menu_layout(home);
    focus_restore(current->popup_focus_start);
}

static void home_menu_layout(home_state_t *home)
{
    if (!home->menu_icon || !current->popup || home->menu_serial != current->popup_serial) return;
    lv_obj_t *dialog = lv_obj_get_parent(home->menu_icon);
    int width = LV_MIN(epd_ui_px(520), WIDTH - 2 * MARGIN);
    lv_obj_set_size(dialog, width, epd_ui_px(473));
    lv_obj_center(dialog);
    lv_obj_set_style_radius(dialog, epd_ui_px(25), 0);
    lv_obj_set_size(home->menu_icon, epd_ui_px(80), epd_ui_px(80));
    lv_obj_set_pos(home->menu_icon, (width - epd_ui_px(80)) / 2, epd_ui_px(12));
    for (unsigned i = 0; i < 3; ++i)
    {
        lv_obj_t *button = lv_obj_get_child(dialog, i + 1);
        lv_obj_set_pos(button, epd_ui_px(15), epd_ui_px(105 + (int)i * 125));
        lv_obj_set_size(button, width - epd_ui_px(30), epd_ui_touch(100));
        lv_obj_set_style_radius(button, epd_ui_px(20), 0);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_font(label, ui_font_title(), 0);
        lv_obj_set_size(label, width - epd_ui_px(54), ui_font_title()->line_height);
        lv_obj_center(label);
    }
}

static bool home_selection_valid(home_state_t *home)
{
    const epd_app_entry_t *entry = home_find(home->selected.id);
    return entry && entry->volume == home->selected.volume &&
           home->selected_revision == app_catalog_revision() &&
           (home_builtin(entry->id) || storage_session_valid(entry->volume, home->selected_session));
}

static bool home_open_settings(const char *id)
{
    rt_err_t policy = app_recovery_check_dynamic();
    if (policy != RT_EOK) { rt_set_errno(policy); return false; }
    intent_t intent = intent_init(id);
    if (!intent) return false;
    intent_set_string(intent, "page", "settings");
    int result = intent_runapp(intent);
    intent_deinit(intent);
    if (result != RT_EOK) return false;
    gui_app_exec_now();
    return gui_app_is_actived((char *)id);
}

static void home_refresh(bool restore)
{
    home_state_t *home = home_state();
    if (!home || storage_changing() || storage_release_requested()) return;
    bool changed = home_sources_changed(home);
    if (home->ready && !changed && home->locale_revision == epd_locale_revision())
    {
        bool missing = false;
        for (unsigned i = 0; i < HOME_SLOT_COUNT; ++i)
            if (home->slots[i].id[0] && !home->slots[i].icon_ready) missing = true;
        if (restore && missing) home_update(false);
        else if (home->scale_revision != epd_ui_scale_revision() ||
                 home->content_bottom != ui_shell_content_bottom()) home_layout(home);
        return;
    }
    uint32_t revision = app_catalog_revision();
    uint32_t volume_revision = storage_revision(), session = storage_card_session();
    uint32_t locale_revision = epd_locale_revision();
    /* A package can replace an icon without changing its path. Drop the SDK's
     * same-path shortcut and any owned bytes before reading the new catalog. */
    if (changed) home_icons_release(home);
    bool refreshed = app_catalog_refresh();
    if (current->popup && home->selected_revision != revision && home->menu_serial)
        popup_close();
    /* A failed refresh keeps the old metadata, but never commits a new revision. */
    if (!refreshed && home->ready && !restore) return;
    bool previous_retry = current->locale_retry;
    current->locale_retry = false;
    home_update(false);
    if (refreshed && !current->locale_retry)
    {
        home->revision = revision;
        home->locale_revision = locale_revision;
        home->storage_revision = volume_revision;
        home->card_session = session;
    }
    home->ready = true;
    current->locale_retry |= previous_retry;
}

void home_resume(void)
{
    home_state_t *home = home_state();
    if (home && home->cache_timer) lv_timer_pause(home->cache_timer);
    home_refresh(true);
}

void home_pause(void)
{
    home_state_t *home = home_state();
    if (!home) return;
    popup_close();
    if (!home->cache_timer) home->cache_timer = lv_timer_create(home_cache_trim, 1000, home);
    bool retain = home->cache_timer && home->ready && !home_sources_changed(home) &&
                  !storage_changing() && !storage_release_requested() && !app_installer_busy() &&
                  epd_app_psram_available() >= HOME_ICON_CACHE_RESERVE;
    if (!retain) home_icons_release(home);
    else
    {
        /* The SDK closes EZIP files after opening, but NOR images may still
         * reference mapped flash. Own bounded copies before closing every
         * original decoder so paused pages never depend on a mounted volume. */
        for (unsigned i = 0; i < HOME_SLOT_COUNT; ++i)
        {
            home_slot_t *slot = &home->slots[i];
            home_slot_retain(home, slot);
            epd_app_image_release(slot->image);
            if (slot->cached_icon.data)
            {
                if (lv_image_get_src(slot->image) != &slot->cached_icon)
                    lv_image_set_src(slot->image, &slot->cached_icon);
            }
            else if (slot->file_icon) slot->icon_ready = slot->file_icon = false;
        }
    }
    if (home->cache_timer)
    {
        if (home->cache_bytes) lv_timer_resume(home->cache_timer);
        else lv_timer_pause(home->cache_timer);
    }
}

void home_stop(void)
{
    home_state_t *home = home_state();
    if (!home) return;
    popup_close();
    if (home->cache_timer) lv_timer_delete(home->cache_timer);
    home_icons_release(home);
    lv_draw_wait_for_finish();
    for (unsigned i = 0; i < UI_HOME_GRID_CAPACITY; ++i)
    {
        lv_obj_remove_event_cb(home->slots[i].tile, home_name_draw_extent);
        if (home->slots[i].label) lv_obj_delete(home->slots[i].label);
        current->home_labels[i] = NULL;
    }
    ui_font_reader_destroy(home->name_font);
    ui_font_reader_destroy(home->font_fallback);
    lv_free(home);
    current->feature = NULL;
}

void home_process(void)
{
    home_state_t *home = home_state();
    if (!home || !current->foreground || storage_changing() || !lv_refreshing_done() ||
        pending_page != NAV_IDLE) return;
    if (home->scale_revision != epd_ui_scale_revision() || home->content_bottom != ui_shell_content_bottom())
        home_layout(home);
    if (home->ticket)
    {
        app_job_result_t result;
        app_installer_result(&result);
        if (result.ticket == home->ticket && result.state != APP_JOB_RUNNING)
        {
            home->ticket = 0;
            popup_close();
            if (result.state == APP_JOB_DONE && ui_home_model_contains(&home->model, home->selected.id))
            {
                ui_home_model_t next = home->model;
                ui_home_model_unpin(&next, home->selected.id);
                /* An absent card does not remove pins. Only a successful uninstall does. */
                if (!home_save(&next))
                {
                    home_resume();
                    popup_open_i18n(LV_EXT_STR_ID(platform_home_save_failed), NULL);
                    return;
                }
            }
            home_resume();
            home->result_message = result.message;
            popup_open(app_installer_message(result.message), NULL);
            home->result_serial = current->popup_serial;
        }
    }
    if (current->popup && home->menu_serial && home->selected_revision != app_catalog_revision())
        popup_close();
    if (!current->popup && (!home->ready || home_sources_changed(home) ||
                            home->locale_revision != epd_locale_revision())) home_refresh(false);
    if (current->popup && home->result_serial == current->popup_serial)
        text_update(current->popup_text, app_installer_message(home->result_message));
}

bool home_action(int target)
{
    home_state_t *home = home_state();
    if (!home) return false;
    if (target == NAV_BACK && current->popup) { popup_close(); return true; }
    if (target == HOME_PREVIOUS || target == HOME_NEXT)
    {
        if (target == HOME_NEXT && home->page + 1 < ui_home_page_count(home->grid_count)) ++home->page;
        else if (target == HOME_PREVIOUS && home->page) --home->page;
        home_update(true);
        return true;
    }
    if (target >= HOME_MENU_BASE && target < HOME_MENU_BASE + (int)HOME_SLOT_COUNT)
    {
        home_menu((unsigned)(target - HOME_MENU_BASE));
        return true;
    }
    if (target >= HOME_APP_BASE && target < HOME_APP_BASE + (int)HOME_SLOT_COUNT)
    {
        unsigned slot = (unsigned)(target - HOME_APP_BASE);
        const epd_app_entry_t *entry = home_find(home->slots[slot].id);
        if (entry)
        {
            char id[GUI_APP_ID_MAX_LEN];
            snprintf(id, sizeof(id), "%s", entry->id);
            if (!ui_navigation_run(id) && current && current->foreground)
                popup_open_i18n(LV_EXT_STR_ID(platform_app_open_failed), NULL);
        }
        return true;
    }
    if (target != HOME_PIN && target != HOME_SETTINGS && target != HOME_UNINSTALL &&
        target != HOME_REMOVE_CONFIRM) return false;
    if (!current->popup || home->ticket) return true;
    if (!home_selection_valid(home)) { popup_close(); home_resume(); return true; }
    char id[GUI_APP_ID_MAX_LEN];
    snprintf(id, sizeof(id), "%s", home->selected.id);
    if (target == HOME_PIN)
    {
        ui_home_model_t next = home->model;
        bool changed = ui_home_model_contains(&next, id) ? ui_home_model_unpin(&next, id) :
                                                         ui_home_model_pin(&next, id);
        if (!changed) return true;
        popup_close();
        if (!home_save(&next)) popup_open_i18n(LV_EXT_STR_ID(platform_home_save_failed), NULL);
        else home_update(true);
    }
    else if (target == HOME_SETTINGS)
    {
        if (!home->selected.has_settings) return true;
        popup_close();
        if (!home_open_settings(id) && current && current->foreground)
            popup_open_i18n(LV_EXT_STR_ID(platform_app_open_failed), NULL);
    }
    else if (target == HOME_UNINSTALL)
    {
        if (home_builtin(id) || app_installer_busy()) return true;
        popup_close();
        popup_confirm_i18n(LV_EXT_STR_ID(platform_remove_app), home->selected.name, HOME_REMOVE_CONFIRM);
        lv_obj_add_event_cb(current->popup, home_popup_outside, LV_EVENT_CLICKED, NULL);
    }
    else
    {
        if (home_builtin(id)) return true;
        popup_close();
        if (!app_installer_start_uninstall(id, home->selected.volume, &home->ticket))
            popup_open_i18n(LV_EXT_STR_ID(platform_operation_failed), NULL);
        else
        {
            popup_open_i18n(LV_EXT_STR_ID(platform_uninstalling), NULL);
            object_visible(current->popup_button, false);
        }
    }
    return true;
}

static void home_tile_create(home_state_t *home, unsigned index, lv_obj_t *parent,
                             int x, int y, int size, bool name)
{
    home_slot_t *slot = &home->slots[index];
    slot->tile = button_create(parent, x, y, size, name ? 184 : size, NULL, HOME_APP_BASE + index);
    lv_obj_set_style_border_width(slot->tile, 0, 0);
    lv_obj_set_style_bg_opa(slot->tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(slot->tile, 16, 0);
    lv_obj_remove_flag(slot->tile, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    /* Long presses and swipes must not dispatch the generic CLICKED action. */
    for (uint32_t i = lv_obj_get_event_count(slot->tile); i > 0; --i)
        if (lv_event_dsc_get_user_data(lv_obj_get_event_dsc(slot->tile, i - 1)) ==
            (void *)(intptr_t)(HOME_APP_BASE + (int)index))
            lv_obj_remove_event(slot->tile, i - 1);
    lv_obj_add_event_cb(slot->tile, home_tile_event, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(slot->tile, home_tile_event, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)index);
    slot->image = icon_create(slot->tile, &ui_icon_album, 0, 0);
    lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(slot->image, size, size);
    lv_image_set_inner_align(slot->image, LV_IMAGE_ALIGN_CONTAIN);
    if (name)
    {
        lv_obj_add_flag(slot->tile, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        slot->label = centered_label(slot->tile, "", 0, size, 160,
                                     home->name_font ? home->name_font : ui_font_body());
        lv_obj_align(slot->label, LV_ALIGN_TOP_MID, 0, size);
        lv_obj_set_height(slot->label, 54);
        lv_label_set_long_mode(slot->label, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(slot->tile, home_name_draw_extent, LV_EVENT_REFR_EXT_DRAW_SIZE, slot);
        current->home_tiles[index] = slot->tile;
        current->home_icons[index] = slot->image;
        current->home_labels[index] = slot->label;
    }
}

void home_create(lv_obj_t *screen)
{
    epd_app_header(screen);
    home_state_t *home = lv_malloc_zeroed(sizeof(*home));
    current->feature = home;
    if (!home)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_body());
        return;
    }
    ui_home_model_t record;
    if (storage_record_load(HOME_DOCK_PATH, HOME_DOCK_MAGIC, HOME_DOCK_VERSION, &record, sizeof(record)) &&
        ui_home_model_valid(&record)) home->model = record;
    home->page = saved_home_page;
    home_font_select(home);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(screen, home_gesture, LV_EVENT_GESTURE, NULL);
    static const int columns[] = {25, 195, 365, 534};
    for (unsigned i = 0; i < UI_HOME_GRID_CAPACITY; ++i)
        home_tile_create(home, i, screen, columns[i % 4], 81 + (int)(i / 4) * 225, 125, true);
    current->home_pager = lv_obj_create(screen);
    epd_obj_init(current->home_pager);
    lv_obj_remove_flag(current->home_pager, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(current->home_pager, 0, 1030);
    lv_obj_set_size(current->home_pager, WIDTH, 15);
    home->dock = panel_create(screen, 13, 1071, 660, 125);
    lv_obj_remove_flag(home->dock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(home->dock, 25, 0);
    lv_obj_set_style_border_color(home->dock, lv_color_hex(0x999999), 0);
    lv_obj_set_style_shadow_color(home->dock, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(home->dock, 35, 0);
    lv_obj_set_style_shadow_opa(home->dock, 128, 0);
    lv_obj_set_style_shadow_ofs_y(home->dock, 4, 0);
    for (unsigned i = 0; i < UI_HOME_DOCK_CAPACITY; ++i)
        home_tile_create(home, UI_HOME_GRID_CAPACITY + i, home->dock, 0, 12, 100, false);
    home_resume();
}
