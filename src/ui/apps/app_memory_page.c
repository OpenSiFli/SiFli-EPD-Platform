#include "storage_internal.h"
#include "ui_internal.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "platform/app_memory_usage.h"
#include "storage.h"

#define MEMORY_REFRESH (-560)

typedef struct
{
    app_memory_snapshot_t snapshot;
    bool valid, dirty;
    ui_settings_list_t *list;
    lv_obj_t *body, *title;
    const lv_font_t *body_font, *small_font;
    uint32_t scale_revision;
} memory_view_t;

static bool memory_layout(memory_view_t *view)
{
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (lv_obj_get_style_text_font(view->title, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), ui_font_settings_title()->line_height);
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
    if (view->scale_revision != epd_ui_scale_revision() ||
        view->body_font != ui_font_settings_body() || view->small_font != ui_font_settings_value())
    {
        if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value())) return false;
        view->body_font = ui_font_settings_body();
        view->small_font = ui_font_settings_value();
        view->scale_revision = epd_ui_scale_revision();
    }
    return ui_settings_list_update_layout(view->list);
}

static bool memory_render(memory_view_t *view)
{
    char values[2][288];
    ui_settings_list_row_t rows[3] = {
        {.key=0, .kind=UI_SETTINGS_ROW_INFO, .title="RAM0"},
        {.key=1, .kind=UI_SETTINGS_ROW_INFO, .title="RAM1"},
        {.key=MEMORY_REFRESH, .kind=UI_SETTINGS_ROW_LINK, .title=epd_get_str(platform_refresh), .enabled=true}
    };
    for (unsigned r = 0; r < 2; ++r)
    {
        const app_ram_usage_t *ram = &view->snapshot.ram[r];
        if (!view->valid)
        {
            rows[r].value = epd_get_str(platform_read_failed);
            continue;
        }
        char usage[96], available[96], unassigned[96] = "", used[24], total[24];
        storage_format_size(ram->used, used, sizeof(used));
        storage_format_size(ram->total, total, sizeof(total));
        EPD_FMT_platform_used_total(usage, sizeof(usage), used, total);
        storage_format_size(ram->available, used, sizeof(used));
        EPD_FMT_platform_allocatable(available, sizeof(available), used);
        if (ram->unassigned)
        {
            storage_format_size(ram->unassigned, used, sizeof(used));
            EPD_FMT_platform_unassigned(unassigned, sizeof(unassigned), used);
        }
        snprintf(values[r], sizeof(values[r]), "%s\n%s%s%s", usage, available,
                 ram->unassigned ? "\n" : "", unassigned);
        rows[r].value = values[r];
    }
    return ui_settings_list_set_rows(view->list, rows, sizeof(rows) / sizeof(rows[0]));
}

void memory_page_restore_focus(void)
{
    if (!current || current->id != UI_PAGE_MEMORY || current->popup) return;
    memory_view_t *view = current->feature;
    if (view) ui_settings_list_set_group(view->list, current->group);
}

void memory_page_process(void)
{
    if (!current || current->id != UI_PAGE_MEMORY || current->popup) return;
    memory_view_t *view = current->feature;
    if (!view || !view->list) return;
    memory_page_restore_focus();
    if (!memory_layout(view)) return;
    if (view->dirty && memory_render(view)) view->dirty = false;
}

void memory_page_resume(void)
{
    memory_view_t *view = current->feature;
    if (!view) return;
    view->valid = app_memory_snapshot(&view->snapshot);
    view->dirty = true;
    memory_page_process();
}

void memory_page_stop(void)
{
    memory_view_t *view = current->feature;
    if (!view) return;
    ui_settings_list_destroy(view->list);
    lv_free(view);
    current->feature = NULL;
}

static void memory_selected(int key, void *context)
{
    if (current && current == context && current->foreground && !current->popup &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

void memory_page_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, LV_EXT_STR_ID(platform_title_memory), ui_font_settings_title());
    memory_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_settings_body());
        return;
    }
    view->title = title;
    view->body_font = ui_font_settings_body();
    view->small_font = ui_font_settings_value();
    view->body = lv_obj_create(screen);
    if (!view->body) return;
    epd_obj_init(view->body);
    lv_obj_set_size(view->body, lv_obj_get_width(screen), lv_obj_get_height(screen));
    view->list = ui_settings_list_create(view->body, view->body_font, view->small_font, memory_selected, current);
    if (!view->list) return;
    ui_settings_list_set_group(view->list, current->group);
}

bool memory_page_action(int *target)
{
    if (*target != MEMORY_REFRESH || !current->feature) return false;
    if (current->popup) return true;
    memory_page_resume();
    return true;
}
