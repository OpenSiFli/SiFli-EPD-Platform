#include "storage_internal.h"
#include "ui_internal.h"
#include "app_files.h"
#include "file_page_index.h"
#include "epd_memory.h"
#include "ui_shell.h"
#include "ui_settings_list.h"
#include "storage.h"
#include "platform/app_content.h"
#include "platform/app_catalog.h"
#include "platform/epd_file_picker.h"
#include <rtm.h>
#include <dfs_posix.h>
#include <time.h>
#include <ctype.h>

#define FILE_ROWS FILE_PAGE_ROWS
#define FILE_ROW_BASE (-520)
#define FILE_PREVIOUS (-530)
#define FILE_NEXT (-531)

typedef struct files_view
{
    char path[STORAGE_PATH_MAX];
    char names[FILE_ROWS][256];
    bool directories[FILE_ROWS], readable[FILE_ROWS];
    rt_err_t popup_error;
    uint32_t error_popup_serial;
    unsigned first, count, page_rows;
    bool more;
    bool choosing;
    char selected[STORAGE_PATH_MAX];
    app_content_handler_t handlers[FILE_ROWS];
    unsigned handler_first;
    uint32_t card_session;
    lv_obj_t *rows[FILE_ROWS], *labels[FILE_ROWS], *details[FILE_ROWS];
    lv_obj_t *title, *breadcrumb, *empty, *previous, *next;
    lv_obj_t *volumes_body;
    ui_settings_list_t *volumes;
    bool picker, pick_json;
    char pick_extensions[96];
    files_picker_callback_t pick;
    void *pick_context;
    lv_obj_t *pick_owner;
    lv_timer_t *pick_timer;
    uint32_t storage_revision;
    char indexed_path[STORAGE_PATH_MAX];
    uint32_t indexed_revision, indexed_session;
    char indexed_extensions[96];
    unsigned indexed_rows;
    bool indexed_picker, indexed_json, paused, page_failed;
    DIR *cursor;
    file_page_cache_t *cache;
    struct files_view *next_view;
} files_view_t;

static files_view_t *active_picker;
static files_view_t *file_views;

static bool files_private_entry(const char *path, const char *name)
{
    if (strcmp(path, STORAGE_SD_ROOT) && strcmp(path, STORAGE_FLASH_ROOT)) return false;
    const char *reserved = ".portal-sap";
    while (*name && tolower((unsigned char)*name) == *reserved) { ++name; ++reserved; }
    return !*name && !*reserved;
}

static const lv_font_t *files_title_font(void)
{ return current->id == UI_PAGE_FILES ? ui_font_title() : ui_font_settings_title(); }
static const lv_font_t *files_body_font(void)
{ return current->id == UI_PAGE_FILES ? ui_font_body() : ui_font_settings_body(); }
static const lv_font_t *files_value_font(void)
{ return current->id == UI_PAGE_FILES ? ui_font_small() : ui_font_settings_value(); }
static int32_t files_title_x(void)
{ return current->id == UI_PAGE_FILES ? PAGE_TITLE_X : SETTINGS_TITLE_X; }
static int32_t files_title_y(void)
{ return current->id == UI_PAGE_FILES ? PAGE_TITLE_Y : SETTINGS_TITLE_Y; }

static void volume_selected(int key, void *context)
{
    if (current == context && current->foreground && !current->popup &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

void files_restore_focus(void)
{
    if (!current || current->popup ||
        (current->id != UI_PAGE_FILES && current->id != UI_PAGE_STORAGE &&
         current->id != UI_PAGE_FILE_PICKER)) return;
    files_view_t *view = current->feature;
    if (view && !view->path[0] && !view->choosing)
        ui_settings_list_set_group(view->volumes, current->group);
}

static void picker_owner_deleted(lv_event_t *event)
{
    files_view_t *view = lv_event_get_user_data(event);
    view->pick_owner = NULL;
    view->pick = NULL;
    view->pick_context = NULL;
}

static void picker_detach(files_view_t *view)
{
    if (view->pick_owner)
        lv_obj_remove_event_cb_with_user_data(view->pick_owner, picker_owner_deleted, view);
    view->pick_owner = NULL;
    view->pick = NULL;
    view->pick_context = NULL;
}

void files_picker_cancel(lv_obj_t *owner)
{
    if (active_picker && active_picker->pick_owner == owner) picker_detach(active_picker);
}

static bool picker_image(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (!extension || strlen(extension) >= 6) return false;
    char lower[6];
    unsigned i = 0;
    do { lower[i] = (char)tolower((unsigned char)extension[i]); } while (extension[i++]);
    return !strcmp(lower, ".png") || !strcmp(lower, ".jpg") || !strcmp(lower, ".jpeg");
}

static bool picker_accepts(const files_view_t *view, const char *name)
{
    if(view->pick_extensions[0]){
        const char *ext=strrchr(name,'.');if(!ext)return false;
        for(const char *p=view->pick_extensions;*p;){const char *end=strchr(p,';');size_t n=end?(size_t)(end-p):strlen(p);
            if(strlen(ext)==n){size_t i=0;while(i<n&&tolower((unsigned char)ext[i])==tolower((unsigned char)p[i]))++i;if(i==n)return true;}
            if(!end)break;p=end+1;
        }return false;
    }
    if (!view->pick_json) return picker_image(name);
    const char *extension = strrchr(name, '.');
    if (!extension || strlen(extension) != 5) return false;
    return extension[0] == '.' && tolower((unsigned char)extension[1]) == 'j' &&
        tolower((unsigned char)extension[2]) == 's' && tolower((unsigned char)extension[3]) == 'o' &&
        tolower((unsigned char)extension[4]) == 'n';
}

/* All view/cursor ownership changes run on the UI thread. */
static void files_track(files_view_t *view)
{ view->next_view = file_views; file_views = view; }
static void files_cursor_close(files_view_t *view)
{
    if (view->cursor) {
        storage_lock(); closedir(view->cursor); storage_unlock(); view->cursor = NULL;
    }
    if (view->cache) {
        view->cache->next_row = 0; view->cache->pending = false; view->cache->eof = false;
    }
}
static void files_cache_reset(files_view_t *view)
{
    files_cursor_close(view);
    if (view->cache) memset(view->cache, 0, sizeof(*view->cache));
}
static void files_dispose(files_view_t *view)
{
    files_cursor_close(view);
    files_view_t **link = &file_views;
    while (*link && *link != view) link = &(*link)->next_view;
    if (*link) *link = view->next_view;
    epd_app_free(view->cache); view->cache = NULL;
}
void files_pause(void)
{
    files_view_t *view = current->feature;
    if (view) { view->paused = true; files_cursor_close(view); }
}
void files_release_storage(void)
{
    for (files_view_t *view = file_views; view; view = view->next_view) files_cursor_close(view);
}
static storage_volume_t files_volume(const files_view_t *view)
{ return !strcmp(storage_path_root(view->path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH; }
/* Caller holds storage_lock, including on cache hits and before each I/O. */
static bool files_cursor_valid(const files_view_t *view)
{
    return !view->paused && !storage_changing() && !storage_release_requested() &&
        storage_session_valid(files_volume(view), view->indexed_session) &&
        view->indexed_revision == storage_directory_revision(view->path);
}
static bool files_cache_prepare(files_view_t *view)
{
    if (view->paused) return false;
    storage_lock();
    storage_volume_t volume = files_volume(view);
    uint32_t session = volume == STORAGE_SD ? storage_card_session() : 0;
    uint32_t revision = storage_directory_revision(view->path);
    bool available = !storage_changing() && !storage_release_requested() && storage_available(volume);
    storage_unlock();
    if (!available) { files_cursor_close(view); return false; }
    if (strcmp(view->indexed_path, view->path) || view->indexed_session != session ||
        view->indexed_revision != revision || view->indexed_rows != view->page_rows ||
        view->indexed_picker != view->picker || view->indexed_json != view->pick_json ||
        strcmp(view->indexed_extensions, view->pick_extensions)) {
        files_cache_reset(view); view->first = 0;
        snprintf(view->indexed_path, sizeof(view->indexed_path), "%s", view->path);
        strcpy(view->indexed_extensions, view->pick_extensions);
        view->indexed_session = session; view->indexed_revision = revision;
        view->indexed_rows = view->page_rows; view->indexed_picker = view->picker; view->indexed_json = view->pick_json;
    }
    if (view->picker) view->card_session = session;
    if (!view->cache) {
        view->cache = epd_app_alloc(sizeof(*view->cache), EPD_APP_PSRAM);
        if (!view->cache) return false;
        memset(view->cache, 0, sizeof(*view->cache));
    }
    return view->page_rows && view->page_rows <= FILE_PAGE_ROWS;
}
static bool files_stat_entry(files_view_t *view, file_page_entry_t *entry)
{
    char path[STORAGE_PATH_MAX]; struct stat info;
    int length = snprintf(path, sizeof(path), "%s/%s", view->path, entry->name);
    if (length <= 0 || (size_t)length >= sizeof(path)) return false;
    storage_lock();
    bool valid = files_cursor_valid(view) && stat(path, &info) == 0;
    storage_unlock();
    if (!valid) return false;
    entry->directory = S_ISDIR(info.st_mode); entry->size = (uint64_t)info.st_size;
    return true;
}
/* 1: accepted entry, 0: EOF, -1: unavailable/read/stat failure. */
static int files_read_entry(files_view_t *view, file_page_entry_t *out)
{
    for (;;) {
        storage_lock();
        if (!files_cursor_valid(view)) { storage_unlock(); return -1; }
        rt_set_errno(0);
        struct dirent *entry = readdir(view->cursor);
        int error = rt_get_errno();
        if (entry) snprintf(out->name, sizeof(out->name), "%s", entry->d_name);
        storage_unlock();
        if (!entry) return error ? -1 : 0;
        if (!strcmp(out->name, ".") || !strcmp(out->name, "..") || files_private_entry(view->path, out->name)) continue;
        if (!files_stat_entry(view, out)) return -1;
        if (view->picker && !out->directory && !picker_accepts(view, out->name)) continue;
        return 1;
    }
}
static int files_take_entry(files_view_t *view, file_page_entry_t *out)
{
    file_page_cache_t *cache = view->cache;
    if (cache->pending) {
        *out = cache->lookahead; cache->pending = false;
        if (!files_stat_entry(view, out)) return -1;
        ++cache->next_row; return 1;
    }
    if (cache->eof) return 0;
    int result = files_read_entry(view, out);
    if (result > 0) ++cache->next_row;
    if (!result) cache->eof = true;
    return result;
}
static file_page_t *files_load_page(files_view_t *view)
{
    if (!files_cache_prepare(view)) return NULL;
    file_page_cache_t *cache = view->cache;
    storage_lock(); bool valid = files_cursor_valid(view); storage_unlock();
    if (!valid) { files_cache_reset(view); return NULL; }
    file_page_t *page = file_page_find(cache, view->first);
    if (page) {
        /* File writes do not change directory membership. Refresh only the
         * displayed entries, preserving the cursor and cached row positions. */
        for (unsigned i = 0; i < page->count; ++i)
            if (!files_stat_entry(view, &page->entries[i])) goto failed;
        storage_lock(); valid = files_cursor_valid(view); storage_unlock();
        if (!valid) goto failed;
        return page;
    }
    /* A cold/backward miss or a paused cursor needs at most one rescan from
     * the beginning. Retain the preceding 15 pages for subsequent back steps. */
    if (!view->cursor || cache->next_row > view->first) {
        files_cursor_close(view);
        storage_lock();
        if (files_cursor_valid(view)) view->cursor = opendir(view->path);
        storage_unlock();
        if (!view->cursor) return NULL;
    }
    for (;;) {
        unsigned first = cache->next_row, rows = view->page_rows;
        if (first < view->first && view->first - first < rows) rows = view->first - first;
        page = file_page_begin(cache, first);
        while (page->count < rows) {
            int result = files_take_entry(view, &page->entries[page->count]);
            if (result < 0) goto failed;
            if (!result) break;
            ++page->count;
        }
        if (!cache->pending && !cache->eof) {
            int result = files_read_entry(view, &cache->lookahead);
            if (result < 0) goto failed;
            cache->pending = result > 0; cache->eof = result == 0;
        }
        page->more = cache->pending;
        storage_lock(); valid = files_cursor_valid(view); storage_unlock();
        if (!valid) goto failed;
        page->valid = true;
        if (first == view->first || !page->more) {
            /* A request past EOF returns the final page, never an invented row. */
            view->first = first;
            if (!page->more) files_cursor_close(view);
            return page;
        }
    }
failed:
    files_cache_reset(view);
    return NULL;
}
static void files_render_directory(files_view_t *view)
{
    text_update(view->breadcrumb, view->path);
    view->count = 0; view->more = false;
    file_page_t *page = files_load_page(view);
    view->page_failed = page == NULL;
    if (!page) {
        text_update(view->empty, epd_get_str(platform_operation_failed));
        return;
    }
    view->count = page->count; view->more = page->more;
    for (unsigned i = 0; i < page->count; ++i) {
        const file_page_entry_t *entry = &page->entries[i]; char size[32];
        strcpy(view->names[i], entry->name); view->directories[i] = entry->directory; view->readable[i] = true;
        if (entry->directory) strcpy(size, epd_get_str(platform_folder));
        else storage_format_size(entry->size, size, sizeof(size));
        text_update(view->labels[i], entry->name); text_update(view->details[i], size);
    }
}

static void files_layout(files_view_t *view)
{
    int32_t width = lv_obj_get_width(current->screen) - 2 * MARGIN;
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int32_t gap = epd_ui_px(10), padding = epd_ui_px(10);
    int32_t top = LV_MAX(PAGE_CONTENT_Y, files_title_y() + files_title_font()->line_height + epd_ui_px(16));
    int32_t row_top = top + files_value_font()->line_height + epd_ui_px(20);
    int32_t row_height = LV_MAX(epd_ui_touch(94),
        files_body_font()->line_height + ui_font_caption()->line_height + 2 * padding + gap);
    int32_t footer_height = LV_MAX(epd_ui_touch(76), files_body_font()->line_height + epd_ui_px(24));
    int32_t footer_y = bottom - footer_height - epd_ui_px(8);
    unsigned capacity = (unsigned)LV_MAX(1, (footer_y - row_top) / (row_height + gap));
    capacity = LV_MIN(capacity, FILE_ROWS);
    lv_obj_t *focused = lv_group_get_focused(current->group);
    for (unsigned i = capacity; i < view->count; ++i)
        if (focused == view->rows[i] && (view->choosing || view->path[0]))
        {
            unsigned shift = i - capacity + 1;
            if (view->choosing) view->handler_first += shift;
            else view->first += shift;
            lv_group_focus_obj(view->rows[capacity - 1]);
            break;
        }
    view->page_rows = capacity;
    lv_obj_set_style_text_font(view->title, files_title_font(), 0);
    lv_obj_set_pos(view->title, files_title_x(), files_title_y());
    lv_obj_set_size(view->title, LV_MAX(1, width + MARGIN - files_title_x()), files_title_font()->line_height);
    int32_t volumes_top = files_title_y() + files_title_font()->line_height + epd_ui_px(32);
    lv_obj_set_pos(view->volumes_body, 0, volumes_top);
    lv_obj_set_size(view->volumes_body, lv_obj_get_width(current->screen), LV_MAX(1, bottom - volumes_top));
    if (!ui_settings_list_refresh_fonts(view->volumes, files_body_font(), files_value_font()) ||
        !ui_settings_list_update_layout(view->volumes)) current->locale_retry = true;
    lv_obj_set_style_text_font(view->breadcrumb, files_value_font(), 0);
    lv_obj_set_pos(view->breadcrumb, MARGIN, top);
    lv_obj_set_size(view->breadcrumb, width, files_value_font()->line_height);
    for (unsigned i = 0; i < FILE_ROWS; ++i)
    {
        lv_obj_set_pos(view->rows[i], MARGIN, row_top + i * (row_height + gap));
        lv_obj_set_size(view->rows[i], width, row_height);
        lv_obj_set_style_radius(view->rows[i], epd_ui_px(8), 0);
        lv_obj_set_style_text_font(view->labels[i], files_body_font(), 0);
        lv_obj_set_pos(view->labels[i], epd_ui_px(20), padding);
        lv_obj_set_size(view->labels[i], width - epd_ui_px(40), files_body_font()->line_height);
        lv_obj_set_style_text_font(view->details[i], ui_font_caption(), 0);
        lv_obj_set_pos(view->details[i], epd_ui_px(20), padding + files_body_font()->line_height + gap);
        lv_obj_set_size(view->details[i], width - epd_ui_px(40), ui_font_caption()->line_height);
    }
    lv_obj_set_style_text_font(view->empty, files_body_font(), 0);
    lv_obj_set_pos(view->empty, MARGIN, row_top + LV_MAX(0, (footer_y - row_top - files_body_font()->line_height) / 2));
    lv_obj_set_size(view->empty, width, files_body_font()->line_height);
    lv_obj_t *buttons[] = {view->previous, view->next};
    int32_t button_width = (width - epd_ui_px(36)) / 2;
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_set_pos(buttons[i], MARGIN + i * (width - button_width), footer_y);
        lv_obj_set_size(buttons[i], button_width, footer_height);
        lv_obj_set_style_radius(buttons[i], epd_ui_px(8), 0);
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        lv_obj_set_style_text_font(label, files_body_font(), 0);
        lv_obj_set_size(label, button_width - epd_ui_px(24), files_body_font()->line_height);
        lv_obj_center(label);
    }
}

static void content_error_popup(rt_err_t error)
{
    files_view_t *view = current->feature;
    if (current->popup) return;
    popup_open(app_content_error(error), NULL);
    view->popup_error = error;
    view->error_popup_serial = current->popup_serial;
}

static void files_render(void)
{
    files_view_t *view = current->feature;
    if (!view || !view->volumes) return;
    if (current->popup) { current->locale_retry = true; return; }
    bool root = !view->path[0] && !view->choosing;
    if (!root) ui_settings_list_set_group(view->volumes, NULL);
    object_visible(view->volumes_body, root);
    object_visible(view->breadcrumb, !root);
    files_layout(view);
    view->count = 0;
    view->more = false;
    view->page_failed = false;
    if (root || view->choosing) files_cursor_close(view);
    text_update(view->empty, epd_get_str(platform_empty));
    if (view->choosing)
    {
        unsigned total = 0;
        rt_err_t result = app_content_query(view->selected, view->handler_first,
                                            view->handlers, view->page_rows, &total);
        if (!strcmp(storage_path_root(view->selected), STORAGE_SD_ROOT) &&
            !storage_session_valid(STORAGE_SD, view->card_session)) result = -RT_EIO;
        if (result == RT_EOK && view->handler_first && view->handler_first >= total)
        {
            view->handler_first = 0;
            result = app_content_query(view->selected, 0, view->handlers, view->page_rows, &total);
        }
        text_update(view->breadcrumb, epd_get_str(platform_choose_app));
        text_update(view->empty, result != RT_EOK ? app_content_error(result) : epd_get_str(platform_no_handler));
        if (result == RT_EOK)
        {
            view->count = total - view->handler_first;
            if (view->count > view->page_rows) view->count = view->page_rows;
            view->more = total - view->handler_first > view->count;
            for (unsigned i = 0; i < view->count; ++i)
            {
                text_update(view->labels[i], view->handlers[i].name);
                text_update(view->details[i], view->handlers[i].id);
            }
        }
    }
    else if (!view->path[0])
    {
        ui_settings_list_row_t rows[STORAGE_COUNT] = {0};
        char values[STORAGE_COUNT][96];
        for (unsigned i = 0; i < STORAGE_COUNT; ++i)
        {
            storage_info_t info;
            char total[24], used[24];
            if (storage_info(i, &info))
            {
                storage_format_size(info.total, total, sizeof(total));
                storage_format_size(info.used, used, sizeof(used));
                EPD_FMT_platform_used_total(values[i], sizeof(values[i]), used, total);
            }
            else snprintf(values[i], sizeof(values[i]), "%s", i == STORAGE_SD ? epd_get_str(platform_card_unmounted) : epd_get_str(platform_unmounted));
            rows[i] = (ui_settings_list_row_t){
                .key=FILE_ROW_BASE + i, .kind=UI_SETTINGS_ROW_LINK,
                .title=i == STORAGE_SD ? epd_get_str(platform_card) : epd_get_str(platform_internal_space),
                .value=values[i], .enabled=!view->pick_json || i == STORAGE_SD
            };
        }
        if (!ui_settings_list_set_rows(view->volumes, rows, STORAGE_COUNT)) current->locale_retry = true;
        view->count = STORAGE_COUNT;
    }
    else
    {
        files_render_directory(view);
    }
    for (unsigned i = 0; i < FILE_ROWS; ++i) object_visible(view->rows[i], !root && i < view->count);
    object_visible(view->empty, !view->count);
    object_visible(view->previous, view->choosing ? view->handler_first != 0 : view->path[0] && view->first);
    text_update(lv_obj_get_child(view->next, 0), view->page_failed ? epd_get_str(platform_refresh) : epd_get_str(platform_next));
    object_visible(view->next, view->more || view->page_failed);
    files_restore_focus();
}

void files_retranslate(void)
{
    files_view_t *view = current->feature;
    if (!view) return;
    if (view->choosing && !app_catalog_refresh()) current->locale_retry = true;
    files_render();
    if (current->popup && view->error_popup_serial == current->popup_serial)
        text_update(current->popup_text, app_content_error(view->popup_error));
}

void files_resume(void) { files_view_t *view = current->feature; if (view) view->paused = false; files_retranslate(); }
void files_storage_changed(void)
{
    files_view_t *view = current->feature;
    if (view) view->choosing = false;
    if (view && view->path[0] && !strcmp(storage_path_root(view->path), STORAGE_SD_ROOT))
    {
        snprintf(view->path, sizeof(view->path), "%s", STORAGE_SD_ROOT);
        view->first = 0;
        if (current->popup) popup_close();
    }
    files_render();
}
void storage_page_resume(void) { files_view_t *view = current->feature; if (view) view->paused = false; files_render(); }
void files_stop(void)
{
    files_view_t *view = current->feature;
    if (!view) return;
    files_dispose(view);
    picker_detach(view);
    if (view->pick_timer) lv_timer_delete(view->pick_timer);
    if (active_picker == view) active_picker = NULL;
    ui_settings_list_destroy(view->volumes);
    lv_free(view);
    current->feature = NULL;
}

static void picker_poll(lv_timer_t *timer)
{
    files_view_t *view = lv_timer_get_user_data(timer);
    if (!current || !current->foreground || current->feature != view || !lv_refreshing_done()) return;
    if (view->storage_revision != storage_revision())
    {
        view->storage_revision = storage_revision();
        files_storage_changed();
    }
}

static bool picker_open_for(lv_obj_t *owner, files_picker_callback_t callback, void *context)
{
    if (!callback || !owner || owner!=lv_screen_active() || active_picker || storage_changing()) return false;
    if (!ui_navigation_open(UI_PAGE_FILE_PICKER)) return false;
    files_view_t *view = current->feature;
    if (!view) { ui_navigation_back(); return false; }
    if (!lv_obj_add_event_cb(owner, picker_owner_deleted, LV_EVENT_DELETE, view))
    { ui_navigation_back(); return false; }
    view->pick_owner = owner;
    view->pick = callback;
    view->pick_context = context;
    view->storage_revision = storage_revision();
    view->pick_timer = lv_timer_create(picker_poll, 150, view);
    if (!view->pick_timer) { picker_detach(view); ui_navigation_back(); return false; }
    active_picker = view;
    return true;
}
bool files_picker_open(files_picker_callback_t callback, void *context)
{
    if(!current||!current->foreground||current->popup||current->id==UI_PAGE_FILE_PICKER)return false;
    return picker_open_for(current->screen,callback,context);
}
bool epd_file_picker_open(lv_obj_t *owner,const char *extensions,epd_file_selected_t callback,void *context)
{
    if(rt_interrupt_get_nest()||!extensions||!*extensions||strlen(extensions)>=96)return false;
    bool start=true;
    for(const char *p=extensions;*p;++p){
        if(start){if(*p!='.')return false;start=false;}
        else if(*p==';'){if(p[-1]=='.')return false;start=true;}
        else if(!isalnum((unsigned char)*p))return false;
    }
    if(start||extensions[strlen(extensions)-1]=='.'||!picker_open_for(owner,callback,context))return false;
    strcpy(active_picker->pick_extensions,extensions);
    for(unsigned i=0;i<current->text_binding_count;++i)
        if(current->text_bindings[i].label==active_picker->title)current->text_bindings[i].id=LV_EXT_STR_ID(platform_choose_file);
    text_update(active_picker->title,epd_get_str(platform_choose_file));files_render();return true;
}
RTM_EXPORT(epd_file_picker_open);
void epd_file_picker_cancel(lv_obj_t *owner){files_picker_cancel(owner);}
RTM_EXPORT(epd_file_picker_cancel);

bool files_picker_open_json(files_picker_callback_t callback, void *context)
{
    if (!files_picker_open(callback, context)) return false;
    files_view_t *view = current->feature;
    view->pick_json = true;
    /* The title's binding must survive locale changes while the picker is open. */
    for (unsigned i = 0; i < current->text_binding_count; ++i)
        if (current->text_bindings[i].label == view->title)
            current->text_bindings[i].id = LV_EXT_STR_ID(ai_import);
    text_update(view->title, epd_get_str(ai_import));
    files_render();
    return true;
}

void files_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, page_title_id(current->id), files_title_font());
    files_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view) { centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, files_body_font()); return; }
    files_track(view);
    view->picker = current->id == UI_PAGE_FILE_PICKER;
    view->title = title;
    view->volumes_body = lv_obj_create(screen);
    if (!view->volumes_body) return;
    epd_obj_init(view->volumes_body);
    view->volumes = ui_settings_list_create(view->volumes_body, files_body_font(), files_value_font(), volume_selected, current);
    if (!view->volumes) return;
    view->breadcrumb = label_create(screen, "", MARGIN, 174, CONTENT_WIDTH, files_value_font());
    for (unsigned i = 0; i < FILE_ROWS; ++i)
    {
        view->rows[i] = button_create(screen, MARGIN, 228 + i * 104, CONTENT_WIDTH, 94, NULL, FILE_ROW_BASE + i);
        view->labels[i] = label_create(view->rows[i], "", 20, 10, CONTENT_WIDTH - 40, files_body_font());
        view->details[i] = label_create(view->rows[i], "", 20, 51, CONTENT_WIDTH - 40, ui_font_caption());
    }
    view->empty = centered_label(screen, "", MARGIN, 514, CONTENT_WIDTH, files_body_font());
    view->previous = button_create_i18n(screen, MARGIN, 1100, 292, 76, LV_EXT_STR_ID(platform_previous), FILE_PREVIOUS);
    view->next = button_create_i18n(screen, 360, 1100, 292, 76, LV_EXT_STR_ID(platform_next), FILE_NEXT);
}
void storage_page_create(lv_obj_t *screen) { files_create(screen); }

bool files_action(int *target)
{
    files_view_t *view = current->feature;
    if (!view) return false;
    if (*target == NAV_BACK && current->popup) { popup_close(); return true; }
    if (view->picker)
    {
        if (*target == NAV_BACK && !view->path[0]) picker_detach(view);
        if (*target != NAV_BACK && !view->pick) return true;
        if (*target != NAV_BACK && (storage_changing() || storage_release_requested())) return true;
        if (*target != NAV_BACK && view->path[0] && !strcmp(storage_path_root(view->path), STORAGE_SD_ROOT) &&
            !storage_session_valid(STORAGE_SD, view->card_session))
        { files_storage_changed(); return true; }
    }
    if (view->choosing)
    {
        if (*target == NAV_BACK) view->choosing = false;
        else if (*target == FILE_PREVIOUS && view->handler_first)
            view->handler_first -= LV_MIN(view->handler_first, view->page_rows);
        else if (*target == FILE_NEXT && view->more) view->handler_first += view->count;
        else if (*target >= FILE_ROW_BASE && *target < FILE_ROW_BASE + (int)view->count)
        {
            unsigned row = *target - FILE_ROW_BASE;
            rt_err_t result = !strcmp(storage_path_root(view->selected), STORAGE_SD_ROOT) &&
                              !storage_session_valid(STORAGE_SD, view->card_session) ? -RT_EIO :
                              app_content_open(view->handlers[row].id, view->selected);
            if (result != RT_EOK) { content_error_popup(result); return true; }
            view->choosing = false;
        }
        else return false;
        files_render();
        focus_restore(0);
        return true;
    }
    if (*target == NAV_BACK && view->path[0])
    {
        char *slash = strrchr(view->path, '/');
        if (slash) *slash = 0;
        view->first = 0;
    }
    else if (*target == FILE_PREVIOUS && view->first) view->first -= LV_MIN(view->first, view->page_rows);
    else if (*target == FILE_NEXT && view->page_failed) { /* Retry the same page. */ }
    else if (*target == FILE_NEXT && view->more) view->first += view->count;
    else if (*target >= FILE_ROW_BASE && *target < FILE_ROW_BASE + (int)view->count)
    {
        unsigned i = *target - FILE_ROW_BASE;
        if (!view->path[0])
        {
            if (view->pick_json && i != STORAGE_SD) return true;
            if (!storage_available(i)) { popup_open_i18n(LV_EXT_STR_ID(platform_storage_unavailable), NULL); return true; }
            snprintf(view->path, sizeof(view->path), "%s", storage_root(i));
        }
        else if (view->directories[i])
        {
            size_t n = strlen(view->path);
            if (n + strlen(view->names[i]) + 2 > sizeof(view->path))
            { popup_open_i18n(LV_EXT_STR_ID(platform_path_long), NULL); return true; }
            snprintf(view->path + n, sizeof(view->path) - n, "/%s", view->names[i]);
        }
        else
        {
            int n = snprintf(view->selected, sizeof(view->selected), "%s/%s", view->path, view->names[i]);
            if (n <= 0 || (size_t)n >= sizeof(view->selected))
            { popup_open_i18n(LV_EXT_STR_ID(platform_path_long), NULL); return true; }
            if (view->picker)
            {
                if (!view->readable[i] || !picker_accepts(view,view->names[i])) return true;
                if (view->pick(view->selected, view->pick_context))
                {
                    picker_detach(view);
                    if (!ui_navigation_back())
                        popup_open_i18n(LV_EXT_STR_ID(platform_back_failed), NULL);
                }
                return true;
            }
            unsigned total;
            view->card_session = storage_card_session();
            rt_err_t result = app_content_query(view->selected, 0, view->handlers, FILE_ROWS, &total);
            if (result != RT_EOK) content_error_popup(result);
            else if (!total)
            {
                char detail[32];
                if (view->readable[i])
                {
                    struct stat st;
                    storage_lock();
                    bool valid = stat(view->selected, &st) == 0;
                    storage_unlock();
                    if (valid) storage_format_size(st.st_size, detail, sizeof(detail));
                    else rt_snprintf(detail, sizeof(detail), "%s", epd_get_str(platform_unreadable));
                    popup_open(view->names[i], detail);
                    if (!valid) text_bind(current->popup_detail, LV_EXT_STR_ID(platform_unreadable));
                }
                else
                {
                    popup_open(view->names[i], "");
                    text_bind(current->popup_detail, LV_EXT_STR_ID(platform_unreadable));
                }
            }
            else if (total == 1)
            {
                result = app_content_open(view->handlers[0].id, view->selected);
                if (result != RT_EOK) content_error_popup(result);
            }
            else
            {
                view->choosing = true;
                view->handler_first = 0;
                files_render();
                focus_restore(0);
            }
            return true;
        }
        view->first = 0;
    }
    else return false;
    files_render();
    focus_restore(0);
    return true;
}

static int app_main(intent_t intent)
{
    (void)intent;
    return ui_navigation_start("files", UI_PAGE_FILES);
}
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(platform_app_files), NULL, "files", app_main, 1);
