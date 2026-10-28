#include "ui_internal.h"
#include "app_wallpaper.h"
#include "app_files.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "wallpaper.h"
#include "storage.h"

#define WALLPAPER_DEFAULT (-14000)
#define WALLPAPER_CHOOSE (-14001)
#define WALLPAPER_CURRENT (-14002)
#define WALLPAPER_STATUS (-14003)

typedef struct
{
    lv_obj_t *title, *body;
    ui_settings_list_t *list;
    lv_timer_t *timer;
    uint32_t revision, locale, scale;
    int32_t bottom;
    bool valid, busy;
    rt_err_t error;
} wallpaper_page_t;

static wallpaper_page_t *page(void)
{
    return current && current->id == UI_PAGE_WALLPAPER ? current->feature : NULL;
}

void wallpaper_page_restore_focus(void)
{
    wallpaper_page_t *view = page();
    if (view && !current->popup) ui_settings_list_set_group(view->list, current->group);
}

static void render(void)
{
    wallpaper_page_t *view = page();
    if (!view || !view->list || current->popup || !lv_refreshing_done()) return;
    wallpaper_status_t status;
    wallpaper_get_status(&status);
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (view->valid && view->revision == status.revision && view->busy == status.busy &&
        view->locale == locale && view->scale == scale && view->bottom == bottom) return;
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32);
    lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), ui_font_settings_title()->line_height);
    text_update(view->title, epd_get_str(wallpaper_title));
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
    if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value())) return;
    bool enabled = status.ready && !status.busy;
    ui_settings_list_row_t rows[] = {
        {.key=WALLPAPER_DEFAULT, .kind=UI_SETTINGS_ROW_CHOICE, .title=epd_get_str(wallpaper_default),
         .enabled=enabled, .checked=!status.custom},
        {.key=WALLPAPER_CURRENT, .kind=UI_SETTINGS_ROW_INFO, .title=epd_get_str(wallpaper_current),
         .value=status.custom ? epd_get_str(wallpaper_custom) : epd_get_str(wallpaper_default), .enabled=true},
        {.key=WALLPAPER_CHOOSE, .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(wallpaper_choose), .enabled=enabled},
        {.key=WALLPAPER_STATUS, .kind=UI_SETTINGS_ROW_INFO, .title=epd_get_str(wallpaper_status), .enabled=true}
    };
    size_t count = 3;
    if (status.busy || !status.ready)
    { rows[count++].value = epd_get_str(wallpaper_processing); }
    else if (status.error || view->error)
    { rows[count++].value = epd_get_str(wallpaper_failed); }
    if (!ui_settings_list_set_rows(view->list, rows, count)) return;
    ui_settings_list_set_group(view->list, current->group);
    view->revision = status.revision;
    view->busy = status.busy;
    view->locale = locale;
    view->scale = scale;
    view->bottom = bottom;
    view->valid = true;
}

static void selected(int key, void *context)
{
    if (current == context && current->foreground && !current->popup && !ui_shell_modal_visible() &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

static bool image_selected(const char *path, void *context)
{
    (void)context;
    rt_err_t error = wallpaper_import(path);
    if (error != RT_EOK)
    {
        popup_open_i18n(LV_EXT_STR_ID(wallpaper_failed), NULL);
        return false;
    }
    return true;
}

static void poll(lv_timer_t *timer)
{
    if (current == lv_timer_get_user_data(timer) && current->foreground) render();
}

void wallpaper_page_create(lv_obj_t *screen)
{
    wallpaper_page_t *view = lv_malloc_zeroed(sizeof(*view));
    if (!view) return;
    current->feature = view;
    view->title = page_title_create(screen, LV_EXT_STR_ID(wallpaper_title), ui_font_settings_title());
    view->body = lv_obj_create(screen);
    epd_obj_init(view->body);
    view->list = ui_settings_list_create(view->body, ui_font_settings_body(), ui_font_settings_value(), selected, current);
    view->timer = lv_timer_create(poll, 150, current);
    if (!wallpaper_init()) view->error = -RT_EIO;
    render();
}

void wallpaper_page_resume(void)
{
    wallpaper_page_t *view = page();
    if (!view) return;
    view->valid = false;
    if (view->timer) lv_timer_resume(view->timer);
    render();
}

void wallpaper_page_pause(void)
{
    wallpaper_page_t *view = page();
    if (view && view->timer) lv_timer_pause(view->timer);
}

void wallpaper_page_stop(void)
{
    if (current) files_picker_cancel(current->screen);
    wallpaper_page_t *view = page();
    if (!view) return;
    if (view->timer) lv_timer_delete(view->timer);
    if (view->list) ui_settings_list_destroy(view->list);
    lv_free(view);
    current->feature = NULL;
}

bool wallpaper_page_action(int *target)
{
    wallpaper_page_t *view = page();
    if (!view || current->popup) return false;
    if (*target != WALLPAPER_DEFAULT && *target != WALLPAPER_CHOOSE) return false;
    if (*target == WALLPAPER_DEFAULT) view->error = wallpaper_use_default();
    else view->error = files_picker_open(image_selected, NULL) ? RT_EOK : -RT_EIO;
    view->valid = false;
    if (view->error) popup_open_i18n(LV_EXT_STR_ID(wallpaper_failed), NULL);
    render();
    return true;
}
