#include "ui_internal.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "platform/epd_lock.h"
#include "platform/app_catalog.h"
#include "storage.h"
#include <limits.h>

#define LOCK_SELECT_BASE (-10000)

typedef struct {
    char id[GUI_APP_ID_MAX_LEN];
    int key;
} lock_choice_t;

typedef struct {
    ui_settings_list_t *list;
    lv_obj_t *body, *title;
    const lv_font_t *body_font, *small_font;
    lock_choice_t *choices;
    size_t count;
    int next_key;
    uint32_t revision;
    uint32_t scale_revision;
    bool dirty;
} lock_settings_view_t;

static bool lock_settings_layout(lock_settings_view_t *view)
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

static bool lock_settings_render(lock_settings_view_t *view)
{
    if (!app_catalog_refresh()) return false;
    size_t count = 1;
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && entry->lockscreen) ++count;
    }
    if (count > SIZE_MAX / sizeof(lock_choice_t) || count > SIZE_MAX / sizeof(ui_settings_list_row_t)) return false;
    lock_choice_t *choices = lv_malloc_zeroed(count * sizeof(*choices));
    ui_settings_list_row_t *rows = lv_malloc_zeroed(count * sizeof(*rows));
    if (!choices || !rows) { lv_free(choices); lv_free(rows); return false; }
    size_t used = 0;
    int next_key = view->next_key;
    bool ok = true;
    /* The built-in choice has the same empty application identity as the policy. */
    for (size_t i = 0; i <= (size_t)app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = i ? app_catalog_get((unsigned)(i - 1)) : NULL;
        if (i && (!entry || !entry->lockscreen)) continue;
        const char *id = entry ? entry->id : "";
        size_t old;
        for (old = 0; old < view->count; ++old) if (!strcmp(view->choices[old].id, id)) break;
        if (old < view->count) choices[used].key = view->choices[old].key;
        else
        {
            if (next_key == INT_MIN) { ok = false; break; }
            choices[used].key = next_key--;
        }
        snprintf(choices[used].id, sizeof(choices[used].id), "%s", id);
        rows[used] = (ui_settings_list_row_t){.key=choices[used].key, .kind=UI_SETTINGS_ROW_CHOICE,
            .title=entry ? entry->name : epd_get_str(shell_default_lock), .enabled=true,
            .checked=!strcmp(id, epd_lock_selected())};
        ++used;
    }
    if (ok) ok = ui_settings_list_set_rows(view->list, rows, used);
    lv_free(rows);
    if (!ok) { lv_free(choices); return false; }
    lv_free(view->choices);
    view->choices = choices;
    view->count = used;
    view->next_key = next_key;
    view->revision = app_catalog_revision();
    return true;
}

void lock_settings_restore_focus(void)
{
    if (!current || current->id != UI_PAGE_LOCK_SETTINGS || current->popup) return;
    lock_settings_view_t *view = current->feature;
    if (view) ui_settings_list_set_group(view->list, current->group);
}

void lock_settings_process(void)
{
    if (!current || current->id != UI_PAGE_LOCK_SETTINGS || current->popup || storage_changing()) return;
    lock_settings_view_t *view = current->feature;
    if (!view || !view->list) return;
    lock_settings_restore_focus();
    if (!lock_settings_layout(view)) return;
    if ((view->dirty || view->revision != app_catalog_revision()) && lock_settings_render(view)) view->dirty = false;
}

void lock_settings_resume(void)
{
    lock_settings_view_t *view = current->feature;
    if (!view) return;
    view->dirty = true;
    lock_settings_process();
}

static void lock_settings_selected(int key, void *context)
{
    if (current && current == context && current->foreground && !current->popup &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

void lock_settings_create(lv_obj_t *screen)
{
    lv_obj_t *title = page_title_create(screen, LV_EXT_STR_ID(shell_lockscreen), ui_font_settings_title());
    lock_settings_view_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_settings_body());
        return;
    }
    view->title = title;
    view->next_key = LOCK_SELECT_BASE;
    view->body_font = ui_font_settings_body();
    view->small_font = ui_font_settings_value();
    view->body = lv_obj_create(screen);
    if (!view->body) return;
    epd_obj_init(view->body);
    lv_obj_set_size(view->body, lv_obj_get_width(screen), lv_obj_get_height(screen));
    view->list = ui_settings_list_create(view->body, view->body_font, view->small_font, lock_settings_selected, current);
    if (!view->list) return;
    ui_settings_list_set_group(view->list, current->group);
    lock_settings_resume();
}

bool lock_settings_action(int *target)
{
    lock_settings_view_t *view = current->feature;
    if (!view || *target > LOCK_SELECT_BASE) return false;
    if (current->popup || storage_changing()) return true;
    for (size_t i = 0; i < view->count; ++i)
        if (view->choices[i].key == *target)
        {
            if (!epd_lock_select(view->choices[i].id))
                popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
            else lock_settings_resume();
            break;
        }
    return true;
}

void lock_settings_stop(void)
{
    lock_settings_view_t *view = current->feature;
    if (!view) return;
    ui_settings_list_destroy(view->list);
    lv_free(view->choices);
    lv_free(view);
    current->feature = NULL;
}
