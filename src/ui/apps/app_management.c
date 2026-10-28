#include "storage_internal.h"
#include "storage_file_internal.h"
#include "ui_internal.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "ui_popup_style.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/app_sap.h"
#include "platform/app_recovery.h"
#include "storage.h"
#include <dfs_posix.h>
#include <dlmodule.h>
#include <limits.h>
#include <ctype.h>
#include <stdlib.h>

#define APP_ROW_BASE (-10000)
#define APP_CONFIRM (-562)
#define APP_INSTALL_FLASH (-563)
#define APP_INSTALL_SD (-564)
#define APP_PARENT (-565)
#define APP_PATH (-566)
#define APP_SOURCE_PATH_MAX APP_SAP_PATH_MAX

typedef struct
{
    char *name;
    int key;
    bool directory, path_long;
} app_source_entry_t;

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN], name[sizeof(((epd_app_entry_t *)0)->name)];
    storage_volume_t volume;
    int key;
} app_list_entry_t;

typedef struct
{
    app_list_entry_t *entries;
    size_t count;
    int next_key;
    ui_settings_list_t *rows;
    lv_obj_t *body, *title;
    const lv_font_t *body_font, *small_font;
    bool dirty;
    uint32_t ticket;
    uint32_t revision;
    uint32_t scale_revision;
    app_job_result_t progress;
    uint32_t progress_popup_serial;
    app_job_message_t result_message;
    uint32_t result_popup_serial, selected_popup_serial;
    uint32_t volume_popup_serial;
    lv_obj_t *volume_buttons[2];
    bool popup_location;
    storage_volume_t volume;
    char selected[GUI_APP_ID_MAX_LEN];
    app_source_entry_t *sources;
    size_t source_count;
    char path[APP_SOURCE_PATH_MAX], selected_path[APP_SOURCE_PATH_MAX];
    uint32_t card_session;
} app_list_t;

static bool app_source_is_package(const char *name)
{
    const char *suffix = strrchr(name, '.');
    return suffix && strlen(suffix) == 4 &&
        tolower((unsigned char)suffix[1]) == 's' &&
        tolower((unsigned char)suffix[2]) == 'a' &&
        tolower((unsigned char)suffix[3]) == 'p';
}

static bool app_source_path(char *path, const char *directory, const char *name)
{
    int n = snprintf(path, APP_SOURCE_PATH_MAX, "%s/%s", directory, name);
    return n > 0 && (size_t)n < APP_SOURCE_PATH_MAX;
}

static void app_sources_free(app_source_entry_t *entries, size_t count)
{
    for (size_t i = 0; i < count; ++i) lv_free(entries[i].name);
    lv_free(entries);
}

static int app_source_name_compare(const char *a, const char *b)
{
    const unsigned char *an = (const unsigned char *)a, *bn = (const unsigned char *)b;
    while (*an && tolower(*an) == tolower(*bn)) { ++an; ++bn; }
    return tolower(*an) - tolower(*bn);
}

static int app_source_compare(const void *left, const void *right)
{
    const app_source_entry_t *a = left, *b = right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    int order = app_source_name_compare(a->name, b->name);
    return order ? order : strcmp(a->name, b->name);
}

static bool app_source_refresh(app_list_t *list, const char *path)
{
    if (app_installer_busy() || storage_changing() || storage_release_requested()) return false;
    app_source_entry_t *entries = NULL;
    size_t count = 0, capacity = 0;
    int next_key = list->next_key;
    bool ok = true;
    const char *empty = epd_get_str(platform_sap_empty);
    storage_lock();
    uint32_t session = storage_card_session();
    bool available = storage_session_valid(STORAGE_SD, session);
    if (!available || session != list->card_session) path = STORAGE_SD_ROOT;
    bool same_path = !strcmp(list->path, path);
    DIR *dir = available ? opendir(path) : NULL;
    if (!dir) empty = available ? epd_get_str(platform_storage_unavailable) : epd_get_str(platform_insert_card);
    if (dir)
    {
        struct dirent *file;
        while ((file = readdir(dir)) != NULL)
        {
            if (!strcmp(file->d_name, ".") || !strcmp(file->d_name, "..")) continue;
            if (!strcmp(path, STORAGE_SD_ROOT) && !app_source_name_compare(file->d_name, ".portal-sap")) continue;
            char full_path[APP_SOURCE_PATH_MAX];
            struct stat st;
            bool path_long = !app_source_path(full_path, path, file->d_name);
            bool directory = file->d_type == DT_DIR;
            if (path_long && !directory && !app_source_is_package(file->d_name)) continue;
            if (!path_long)
            {
                if (stat(full_path, &st) != 0) continue;
                directory = S_ISDIR(st.st_mode);
                if (!directory && (!S_ISREG(st.st_mode) || !app_source_is_package(file->d_name))) continue;
            }
            if (count == capacity)
            {
                size_t grown = capacity ? capacity * 2 : 8;
                if (grown < capacity || grown > SIZE_MAX / sizeof(*entries)) { ok = false; break; }
                void *memory = lv_realloc(entries, grown * sizeof(*entries));
                if (!memory) { ok = false; break; }
                entries = memory;
                capacity = grown;
            }
            if (next_key == INT_MIN) { ok = false; break; }
            app_source_entry_t *item = &entries[count];
            *item = (app_source_entry_t){.directory=directory, .path_long=path_long, .key=next_key--};
            item->name = lv_malloc(strlen(file->d_name) + 1);
            if (!item->name) { ok = false; break; }
            strcpy(item->name, file->d_name);
            if (same_path && session == list->card_session)
                for (size_t i = 0; i < list->source_count; ++i)
                    if (!strcmp(list->sources[i].name, item->name) && list->sources[i].directory == directory)
                    { item->key = list->sources[i].key; ++next_key; break; }
            ++count;
        }
        closedir(dir);
    }
    if (!storage_session_valid(STORAGE_SD, session))
    {
        app_sources_free(entries, count);
        entries = NULL;
        count = 0;
        empty = epd_get_str(platform_insert_card);
    }
    storage_unlock();
    if (!ok || count > SIZE_MAX / sizeof(ui_settings_list_row_t) - 3)
    { app_sources_free(entries, count); return false; }
    if (count > 1) qsort(entries, count, sizeof(*entries), app_source_compare);
    ui_settings_list_row_t *rows = lv_malloc_zeroed((count + 3) * sizeof(*rows));
    if (!rows) { app_sources_free(entries, count); return false; }
    size_t used = 0;
    rows[used++] = (ui_settings_list_row_t){.key=APP_PATH, .kind=UI_SETTINGS_ROW_INFO,
        .title=epd_get_str(platform_current_folder), .value=path};
    if (strcmp(path, STORAGE_SD_ROOT))
        rows[used++] = (ui_settings_list_row_t){.key=APP_PARENT, .kind=UI_SETTINGS_ROW_LINK,
            .title=epd_get_str(platform_parent_folder), .enabled=true};
    for (size_t i = 0; i < count; ++i)
        rows[used++] = (ui_settings_list_row_t){.key=entries[i].key, .kind=UI_SETTINGS_ROW_LINK,
            .title=entries[i].name, .enabled=!entries[i].path_long,
            .value=entries[i].path_long ? epd_get_str(platform_path_long) :
                   entries[i].directory ? epd_get_str(platform_folder) : epd_get_str(platform_sap_package)};
    if (!count) rows[used++] = (ui_settings_list_row_t){.key=0, .kind=UI_SETTINGS_ROW_INFO, .title=empty};
    ok = ui_settings_list_set_rows(list->rows, rows, used);
    lv_free(rows);
    if (!ok) { app_sources_free(entries, count); return false; }
    app_sources_free(list->sources, list->source_count);
    list->sources = entries;
    list->source_count = count;
    list->next_key = next_key;
    list->card_session = session;
    if (!same_path)
    {
        strcpy(list->path, path);
        ui_settings_list_show_key(list->rows, APP_PATH);
    }
    list->revision = app_catalog_revision();
    list->dirty = false;
    return true;
}

static unsigned app_progress_percent(uint64_t completed, uint64_t total)
{
    if (completed >= total) return 100;
    uint64_t whole = total / 100, remainder = total % 100;
    unsigned low = 0, high = 100;
    while (low < high)
    {
        unsigned percent = (low + high + 1) / 2;
        /* Compare with ceil(total * percent / 100) without overflowing. */
        uint64_t threshold = whole * percent + (remainder * percent + 99) / 100;
        if (completed >= threshold) low = percent;
        else high = percent - 1;
    }
    return low;
}

static void app_progress_text(const app_job_result_t *progress, char *text, size_t capacity)
{
    const char *phase = app_installer_phase_name(progress->phase);
    if (progress->total_bytes)
        snprintf(text, capacity, "%s %u%%", phase,
                 app_progress_percent(progress->completed_bytes, progress->total_bytes));
    else snprintf(text, capacity, "%s", phase);
}

static void app_progress_update(app_list_t *list)
{
    if (!list->ticket || !current->popup || current->popup_serial != list->progress_popup_serial) return;
    char text[96];
    app_progress_text(&list->progress, text, sizeof(text));
    if (strcmp(lv_label_get_text(current->popup_text), text)) text_update(current->popup_text, text);
}

static void app_volume_layout(app_list_t *list)
{
    if (!current->popup || current->popup_serial != list->volume_popup_serial) return;
    lv_obj_update_layout(current->screen);
    int32_t width = LV_MIN(epd_ui_px(576), lv_obj_get_width(current->screen) - 2 * MARGIN);
    int32_t padding = epd_ui_px(24), gap = epd_ui_px(20);
    int32_t header_y = epd_ui_px(32), header_height = 2 * ui_font_settings_body()->line_height;
    int32_t detail_y = header_y + header_height + epd_ui_px(12);
    int32_t detail_height = 2 * ui_font_settings_value()->line_height;
    int32_t choice_y = detail_y + detail_height + gap;
    int32_t button_height = LV_MAX(epd_ui_touch(64), ui_font_settings_value()->line_height + epd_ui_px(24));
    int32_t cancel_y = choice_y + button_height + gap;
    int32_t height = cancel_y + button_height + padding;
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    lv_obj_t *dialog = lv_obj_get_parent(current->popup_button);
    lv_obj_set_pos(dialog, (lv_obj_get_width(current->screen) - width) / 2, LV_MAX(0, (bottom - height) / 2));
    lv_obj_set_size(dialog, width, height);
    if (lv_obj_get_style_text_font(current->popup_text, 0) != ui_font_settings_body())
        lv_obj_set_style_text_font(current->popup_text, ui_font_settings_body(), 0);
    lv_obj_set_pos(current->popup_text, padding, header_y);
    lv_obj_set_size(current->popup_text, width - 2 * padding, header_height);
    if (current->popup_detail)
    {
        if (lv_obj_get_style_text_font(current->popup_detail, 0) != ui_font_settings_value())
            lv_obj_set_style_text_font(current->popup_detail, ui_font_settings_value(), 0);
        lv_obj_set_pos(current->popup_detail, padding, detail_y);
        lv_obj_set_size(current->popup_detail, width - 2 * padding, detail_height);
        if (lv_label_get_long_mode(current->popup_detail) != LV_LABEL_LONG_WRAP)
            lv_label_set_long_mode(current->popup_detail, LV_LABEL_LONG_WRAP);
    }
    int32_t choice_width = (width - 2 * padding - gap) / 2;
    for (unsigned i = 0; i < 3; ++i)
    {
        lv_obj_t *button = i < 2 ? list->volume_buttons[i] : current->popup_button;
        if (lv_obj_get_style_radius(button, 0) != epd_ui_px(20))
            lv_obj_set_style_radius(button, epd_ui_px(20), 0);
        int32_t w = i < 2 ? choice_width : width - 2 * padding;
        lv_obj_set_pos(button, padding + (i == 1 ? choice_width + gap : 0), i < 2 ? choice_y : cancel_y);
        lv_obj_set_size(button, w, button_height);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (lv_obj_get_style_text_font(label, 0) != ui_font_settings_value())
            lv_obj_set_style_text_font(label, ui_font_settings_value(), 0);
        lv_obj_set_size(label, w - epd_ui_px(24), ui_font_settings_value()->line_height);
    }
}

static bool app_list_layout(app_list_t *list)
{
    if (!list->rows || current->popup) return false;
    lv_obj_update_layout(current->screen);
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (lv_obj_get_style_text_font(list->title, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(list->title, ui_font_settings_title(), 0);
    lv_obj_set_pos(list->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(list->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), ui_font_settings_title()->line_height);
    lv_obj_set_pos(list->body, 0, top);
    lv_obj_set_size(list->body, width, LV_MAX(1, bottom - top));
    if (list->scale_revision != epd_ui_scale_revision() ||
        list->body_font != ui_font_settings_body() || list->small_font != ui_font_settings_value())
    {
        if (!ui_settings_list_refresh_fonts(list->rows, ui_font_settings_body(), ui_font_settings_value())) return false;
        list->body_font = ui_font_settings_body();
        list->small_font = ui_font_settings_value();
        list->scale_revision = epd_ui_scale_revision();
    }
    return ui_settings_list_update_layout(list->rows);
}

static void app_list_selected(int key, void *context)
{
    if (current && current == context && current->foreground && !current->popup &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

static void list_refresh(void)
{
    app_list_t *list = current->feature;
    if (!list || !list->rows) return;
    list->dirty = true;
    if (current->popup || app_installer_busy() || storage_changing() || storage_release_requested()) return;
    if (!app_list_layout(list)) return;
    if (current->id == UI_PAGE_APP_INSTALL)
    {
        app_source_refresh(list, list->path);
        return;
    }
    app_list_entry_t *entries = NULL;
    size_t count = 0, capacity = 0;
    int next_key = list->next_key;
    bool ok = true;
    storage_lock();
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT && ok; ++v)
    {
        if (!app_installer_volume_ready(v)) continue;
        if (current->id == UI_PAGE_APP_SETTINGS && !storage_apps_enabled(v)) continue;
        const char *base = storage_app_directory(v);
        DIR *dir = storage_path_available(base) ? opendir(base) : NULL;
        if (!dir) continue;
        struct dirent *file;
        epd_app_entry_t entry;
        while ((file = readdir(dir)) != NULL)
        {
            if (!app_package_read_display(base, file->d_name, &entry)) continue;
            if (current->id == UI_PAGE_APP_SETTINGS && !entry.has_settings) continue;
            if (current->id == UI_PAGE_APP_SETTINGS)
            {
                storage_volume_t selected;
                if (!storage_app_locate(entry.id, &selected) || selected != v) continue;
            }
            if (count == capacity)
            {
                size_t grown = capacity ? capacity * 2 : 8;
                if (grown < capacity || grown > SIZE_MAX / sizeof(*entries)) { ok = false; break; }
                void *memory = lv_realloc(entries, grown * sizeof(*entries));
                if (!memory) { ok = false; break; }
                entries = memory;
                capacity = grown;
            }
            app_list_entry_t *item = &entries[count];
            snprintf(item->id, sizeof(item->id), "%s", entry.id);
            snprintf(item->name, sizeof(item->name), "%s", entry.name);
            item->volume = entry.volume;
            size_t old;
            for (old = 0; old < list->count; ++old)
                if (list->entries[old].volume == item->volume && !strcmp(list->entries[old].id, item->id)) break;
            if (old < list->count) item->key = list->entries[old].key;
            else
            {
                if (next_key == INT_MIN) { ok = false; break; }
                item->key = next_key--;
            }
            ++count;
        }
        closedir(dir);
    }
    storage_unlock();
    if (!ok || count > SIZE_MAX / sizeof(ui_settings_list_row_t)) { lv_free(entries); return; }
    ui_settings_list_row_t *rows = count ? lv_malloc_zeroed(count * sizeof(*rows)) : NULL;
    if (count && !rows) { lv_free(entries); return; }
    for (size_t i = 0; i < count; ++i)
    {
        rows[i] = (ui_settings_list_row_t){.key=entries[i].key, .kind=UI_SETTINGS_ROW_LINK,
            .title=entries[i].name, .enabled=true};
        if (current->id == UI_PAGE_APP_UNINSTALL)
            rows[i].value = entries[i].volume == STORAGE_SD ? epd_get_str(platform_card) : epd_get_str(platform_internal);
    }
    ui_settings_list_row_t empty = {.key=0, .kind=UI_SETTINGS_ROW_INFO,
        .title=epd_get_str(platform_empty)};
    ok = ui_settings_list_set_rows(list->rows, count ? rows : &empty, count ? count : 1);
    lv_free(rows);
    if (!ok) { lv_free(entries); return; }
    lv_free(list->entries);
    list->entries = entries;
    list->count = count;
    list->next_key = next_key;
    list->revision = app_catalog_revision();
    list->dirty = false;
}

void app_list_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, page_title_id(current->id), ui_font_settings_title());
    app_list_t *list = lv_malloc_zeroed(sizeof(*list));
    current->feature = list;
    if (!list) { centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_settings_body()); return; }
    list->title = title;
    list->next_key = APP_ROW_BASE;
    strcpy(list->path, STORAGE_SD_ROOT);
    list->body_font = ui_font_settings_body();
    list->small_font = ui_font_settings_value();
    list->body = lv_obj_create(screen);
    if (!list->body) return;
    epd_obj_init(list->body);
    lv_obj_set_size(list->body, lv_obj_get_width(screen), lv_obj_get_height(screen));
    list->rows = ui_settings_list_create(list->body, list->body_font, list->small_font, app_list_selected, current);
    if (!list->rows) return;
    ui_settings_list_set_group(list->rows, current->group);
}

void app_list_retranslate(void)
{
    app_list_t *list = current->feature;
    if (!list) return;
    app_volume_layout(list);
    list_refresh();
    if (!current->popup) return;
    app_progress_update(list);
    if (list->result_popup_serial == current->popup_serial)
        text_update(current->popup_text, app_installer_message(list->result_message));
    if (list->selected_popup_serial == current->popup_serial && !app_installer_busy() &&
        !storage_changing() && !storage_release_requested())
    {
        epd_app_entry_t entry;
        storage_lock();
        bool found = current->id == UI_PAGE_APP_INSTALL ?
            storage_session_valid(STORAGE_SD, list->card_session) &&
                app_sap_metadata(list->selected_path, &entry) == APP_PACKAGE_OK :
            app_package_read_display(storage_app_directory(list->volume), list->selected, &entry);
        storage_unlock();
        if (found)
        {
            char detail[128];
            if (list->popup_location)
                EPD_FMT_platform_app_location(detail, sizeof(detail), entry.name,
                    list->volume == STORAGE_SD ? epd_get_str(platform_card) : epd_get_str(platform_internal));
            else rt_snprintf(detail, sizeof(detail), "%s", entry.name);
            text_update(current->popup_detail, detail);
        }
    }
}

void app_list_resume(void) { app_list_retranslate(); app_list_process(); }
void app_list_stop(void)
{
    app_list_t *list = current->feature;
    if (!list) return;
    ui_settings_list_destroy(list->rows);
    app_sources_free(list->sources, list->source_count);
    lv_free(list->entries);
    lv_free(list);
    current->feature = NULL;
}

void app_list_restore_focus(void)
{
    if (!current || current->popup || (current->id != UI_PAGE_APP_INSTALL &&
        current->id != UI_PAGE_APP_UNINSTALL && current->id != UI_PAGE_APP_SETTINGS)) return;
    app_list_t *list = current->feature;
    if (list) ui_settings_list_set_group(list->rows, current->group);
}

void app_list_storage_changed(void)
{
    app_list_t *list = current->feature;
    /* A confirmation made for a removed card must not target a replacement card. */
    if (list && !list->ticket) popup_close();
    if (list && current->id == UI_PAGE_APP_INSTALL)
    {
        strcpy(list->path, STORAGE_SD_ROOT);
        list->selected_path[0] = 0;
        list->dirty = true;
    }
    app_list_resume();
}

void app_list_process(void)
{
    if (!current || (current->id != UI_PAGE_APP_INSTALL && current->id != UI_PAGE_APP_UNINSTALL &&
        current->id != UI_PAGE_APP_SETTINGS)) return;
    app_list_t *list = current->feature;
    if (!list) return;
    app_volume_layout(list);
    if (!list->ticket)
    {
        if (current->id == UI_PAGE_APP_INSTALL &&
            (list->card_session != storage_card_session() ||
             ((list->source_count || list->selected_path[0]) &&
              !storage_session_valid(STORAGE_SD, list->card_session))))
        {
            popup_close();
            strcpy(list->path, STORAGE_SD_ROOT);
            list->selected_path[0] = 0;
            list->dirty = true;
        }
        if (!current->popup)
        {
            app_list_restore_focus();
            app_list_layout(list);
            if (list->dirty || list->revision != app_catalog_revision()) list_refresh();
        }
        return;
    }
    app_job_result_t result;
    app_installer_result(&result);
    if (result.ticket != list->ticket) return;
    if (result.state == APP_JOB_RUNNING)
    {
        list->progress = result;
        app_progress_update(list);
        return;
    }
    if (result.state != APP_JOB_DONE && result.state != APP_JOB_FAILED) return;
    list->ticket = 0;
    popup_close();
    list_refresh();
    list->result_message = result.message;
    popup_open(app_installer_message(result.message), NULL);
    list->result_popup_serial = current->popup_serial;
}

static bool open_dynamic_settings(const char *id)
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

static void select_install_volume(const char *name)
{
    popup_open_i18n(LV_EXT_STR_ID(platform_choose_volume), name);
    lv_obj_t *dialog = lv_obj_get_parent(current->popup_button);
    text_bind(lv_obj_get_child(current->popup_button, 0), LV_EXT_STR_ID(platform_cancel));
    lv_obj_t *internal = button_create_i18n(dialog, 48, 184, 222, 64, LV_EXT_STR_ID(platform_internal), APP_INSTALL_FLASH);
    lv_obj_t *card = button_create_i18n(dialog, 306, 184, 222, 64, LV_EXT_STR_ID(platform_card), APP_INSTALL_SD);
    ui_popup_style_button(internal);
    ui_popup_style_button(card);
    lv_obj_set_style_text_font(lv_obj_get_child(internal, 0), ui_font_settings_value(), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(card, 0), ui_font_settings_value(), 0);
    app_list_t *list = current->feature;
    list->volume_buttons[0] = internal;
    list->volume_buttons[1] = card;
    list->volume_popup_serial = current->popup_serial;
    app_volume_layout(list);
    if (!app_installer_volume_ready(STORAGE_FLASH)) lv_obj_add_state(internal, LV_STATE_DISABLED);
    if (!app_installer_volume_ready(STORAGE_SD)) lv_obj_add_state(card, LV_STATE_DISABLED);
    lv_group_focus_obj(current->popup_button);
}

static void app_source_error(app_list_t *list, app_job_message_t message)
{
    list->result_message = message;
    popup_open(app_installer_message(message), NULL);
    list->result_popup_serial = current->popup_serial;
}

/* Caller holds storage_lock through metadata validation and installer handoff. */
static app_job_message_t app_source_metadata(app_list_t *list, const char *path, epd_app_entry_t *entry)
{
    if (!storage_session_valid(STORAGE_SD, list->card_session)) return APP_JOB_MESSAGE_CARD_CHANGED;
    app_package_status_t result = app_sap_metadata(path, entry);
    if (!storage_session_valid(STORAGE_SD, list->card_session)) return APP_JOB_MESSAGE_CARD_CHANGED;
    switch (result)
    {
    case APP_PACKAGE_OK: return APP_JOB_MESSAGE_NONE;
    case APP_PACKAGE_INCOMPATIBLE: return APP_JOB_MESSAGE_INCOMPATIBLE;
    case APP_PACKAGE_IO_ERROR: return APP_JOB_MESSAGE_READ_FAILED;
    case APP_PACKAGE_INTEGRITY_ERROR: return APP_JOB_MESSAGE_VERIFY_FAILED;
    default: return APP_JOB_MESSAGE_BAD_PACKAGE;
    }
}

static void app_source_selected(app_list_t *list, int key)
{
    if (app_installer_busy()) return;
    size_t row;
    for (row = 0; row < list->source_count; ++row)
        if (list->sources[row].key == key) break;
    if (row == list->source_count) return;
    char path[APP_SOURCE_PATH_MAX];
    if (list->sources[row].path_long || !app_source_path(path, list->path, list->sources[row].name))
    { popup_open_i18n(LV_EXT_STR_ID(platform_path_long), NULL); return; }
    if (list->sources[row].directory)
    {
        if (!app_source_refresh(list, path)) popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
        return;
    }
    epd_app_entry_t entry;
    storage_lock();
    app_job_message_t error = app_source_metadata(list, path, &entry);
    bool installed = error == APP_JOB_MESSAGE_NONE && storage_app_locate(entry.id, &list->volume);
    storage_unlock();
    if (error != APP_JOB_MESSAGE_NONE) { app_source_error(list, error); return; }
    strcpy(list->selected_path, path);
    snprintf(list->selected, sizeof(list->selected), "%s", entry.id);
    list->popup_location = installed;
    if (installed)
    {
        char detail[128];
        EPD_FMT_platform_app_location(detail, sizeof(detail), entry.name,
            list->volume == STORAGE_SD ? epd_get_str(platform_card) : epd_get_str(platform_internal));
        popup_confirm_i18n(LV_EXT_STR_ID(platform_update_app), detail, APP_CONFIRM);
    }
    else select_install_volume(entry.name);
    list->selected_popup_serial = current->popup_serial;
}

bool app_list_action(int *target)
{
    app_list_t *list = current->feature;
    if (!list) return false;
    if (list->ticket) return true;
    if (*target == NAV_BACK && current->popup) { popup_close(); return true; }
    bool installing = current->id == UI_PAGE_APP_INSTALL;
    if (app_installer_busy())
        return *target != NAV_BACK || (installing && strcmp(list->path, STORAGE_SD_ROOT));
    if (installing && (*target == NAV_BACK || *target == APP_PARENT) && !current->popup)
    {
        if (!strcmp(list->path, STORAGE_SD_ROOT)) return *target == APP_PARENT;
        char parent[APP_SOURCE_PATH_MAX];
        strcpy(parent, list->path);
        *strrchr(parent, '/') = 0;
        if (storage_changing() || storage_release_requested()) return true;
        if (!storage_session_valid(STORAGE_SD, list->card_session)) app_list_storage_changed();
        else if (!app_source_refresh(list, parent)) popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
        return true;
    }
    if ((*target == APP_CONFIRM || *target == APP_INSTALL_FLASH || *target == APP_INSTALL_SD) && current->popup)
    {
        bool uninstall = current->id == UI_PAGE_APP_UNINSTALL;
        if ((!installing && !uninstall) || current->popup_serial != list->selected_popup_serial ||
            (uninstall && *target != APP_CONFIRM) ||
            (installing && ((*target == APP_CONFIRM) != list->popup_location))) return true;
        if (*target != APP_CONFIRM) list->volume = *target == APP_INSTALL_SD ? STORAGE_SD : STORAGE_FLASH;
        popup_close();
        bool started;
        if (installing)
        {
            epd_app_entry_t entry;
            storage_lock();
            app_job_message_t error = storage_changing() || storage_release_requested() ?
                APP_JOB_MESSAGE_CARD_CHANGED : app_source_metadata(list, list->selected_path, &entry);
            if (error == APP_JOB_MESSAGE_NONE && strcmp(entry.id, list->selected)) error = APP_JOB_MESSAGE_BAD_PACKAGE;
            started = error == APP_JOB_MESSAGE_NONE &&
                app_installer_start_package(list->selected_path, list->volume, &list->ticket);
            storage_unlock();
            if (error != APP_JOB_MESSAGE_NONE) { app_source_error(list, error); return true; }
        }
        else started = app_installer_start_uninstall(list->selected, list->volume, &list->ticket);
        if (!started)
        {
            list->ticket = 0;
            popup_open_i18n(LV_EXT_STR_ID(platform_operation_failed), NULL);
        }
        else
        {
            list->progress = (app_job_result_t){.ticket=list->ticket, .state=APP_JOB_RUNNING,
                                               .phase=APP_JOB_PHASE_PREPARING};
            app_job_result_t result;
            app_installer_result(&result);
            if (result.ticket == list->ticket && result.state == APP_JOB_RUNNING) list->progress = result;
            char text[96];
            app_progress_text(&list->progress, text, sizeof(text));
            popup_open(text, NULL);
            list->progress_popup_serial = current->popup_serial;
            object_visible(current->popup_button, false);
        }
        return true;
    }
    if (*target <= APP_ROW_BASE)
    {
        if (current->popup || storage_changing() || storage_release_requested()) return true;
        if (installing)
        {
            if (!storage_session_valid(STORAGE_SD, list->card_session)) app_list_storage_changed();
            else if (list->dirty) list_refresh();
            else app_source_selected(list, *target);
            return true;
        }
        size_t row;
        for (row = 0; row < list->count; ++row) if (list->entries[row].key == *target) break;
        if (row == list->count) return true;
        if (current->id == UI_PAGE_APP_SETTINGS)
        {
            if (app_recovery_active())
            {
                popup_open_i18n(LV_EXT_STR_ID(platform_recovery_blocked), "");
                text_bind(current->popup_detail, LV_EXT_STR_ID(platform_recovery_manage));
                return true;
            }
            char id[GUI_APP_ID_MAX_LEN];
            snprintf(id, sizeof(id), "%s", list->entries[row].id);
            if (!open_dynamic_settings(id) && current && current->foreground)
                popup_open_i18n(LV_EXT_STR_ID(platform_app_open_failed), NULL);
        }
        else
        {
            snprintf(list->selected, sizeof(list->selected), "%s", list->entries[row].id);
            list->popup_location = false;
            list->volume = list->entries[row].volume;
            popup_confirm_i18n(LV_EXT_STR_ID(platform_remove_app), list->entries[row].name, APP_CONFIRM);
            list->selected_popup_serial = current->popup_serial;
        }
        return true;
    }
    return false;
}
