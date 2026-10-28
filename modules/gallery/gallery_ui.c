#include "storage.h"
#include "platform/epd_locale.h"
#include "gallery.h"
#include "platform/epd_i18n.h"
#include "platform/app_content.h"
#include "src/draw/lv_image_decoder_private.h"
#include <string.h>
#include <stdio.h>

EPD_APP_DEFINE("gallery");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

static char launch_path[STORAGE_PATH_MAX], picture_path[STORAGE_PATH_MAX];
static uint32_t launch_session, picture_session;
static rt_err_t launch_error;

typedef struct
{
    lv_obj_t *screen, *breadcrumb, *empty, *rows[GALLERY_ROWS], *labels[GALLERY_ROWS];
    lv_obj_t *previous, *next, *title, *back;
    uint32_t locale_revision, scale_revision;
    lv_area_t scale_area;
    gallery_status_t status_code;
    rt_err_t status_content_error;
    lv_timer_t *timer;
    char path[STORAGE_PATH_MAX], selected[STORAGE_PATH_MAX];
    gallery_entry_t entries[GALLERY_ROWS];
    unsigned first, count, selected_row, depth, offsets[32], start_attempts;
    uint32_t request, request_tick;
    bool started, open_content, rows_current;
    rt_err_t content_error;
    uint32_t content_session;
} browser_t;

typedef struct
{
    lv_obj_t *screen, *image, *status, *title, *back, *name, *viewport;
    uint32_t locale_revision, scale_revision;
    lv_area_t scale_area;
    gallery_status_t status_code;
    lv_timer_t *timer;
    char path[STORAGE_PATH_MAX];
    uint32_t request, request_tick;
    uint32_t card_session;
    gallery_result_t *displayed;
} viewer_t;

static void browser_scan(browser_t *page);
static void viewer_message(gui_app_msg_type_t message, void *parameter);

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static const char *status_text(gallery_status_t status)
{
    switch (status)
    {
    case GALLERY_STATUS_LOADING: return epd_get_str(gallery_loading);
    case GALLERY_STATUS_EMPTY: return epd_get_str(gallery_empty);
    case GALLERY_STATUS_STORAGE: return epd_get_str(gallery_storage);
    case GALLERY_STATUS_REMOVED: return epd_get_str(gallery_removed);
    case GALLERY_STATUS_PATH: return epd_get_str(gallery_path);
    case GALLERY_STATUS_DEPTH: return epd_get_str(gallery_depth);
    case GALLERY_STATUS_PAGE: return epd_get_str(gallery_page);
    case GALLERY_STATUS_UNAVAILABLE: return epd_get_str(gallery_unavailable);
    case GALLERY_STATUS_SERVICE: return epd_get_str(gallery_service);
    case GALLERY_STATUS_MEMORY: return epd_get_str(gallery_memory);
    case GALLERY_STATUS_OPEN: return epd_get_str(gallery_open);
    case GALLERY_STATUS_DECODE: return epd_get_str(gallery_decode);
    default: return "";
    }
}

static bool browser_status(browser_t *page, gallery_status_t status)
{
    page->status_code = status;
    return epd_app_text(page->empty, status == GALLERY_STATUS_CONTENT ?
                 app_content_error(page->status_content_error) : status_text(status));
}
static bool viewer_status(viewer_t *page, gallery_status_t status)
{
    page->status_code = status;
    return epd_app_text(page->status, status_text(status));
}
static void browser_locale(browser_t *page)
{
    if (page->locale_revision == epd_locale_revision()) return;
    bool updated = true;
    updated &= epd_app_text(page->title, epd_get_str(gallery_title));
    updated &= epd_app_text(lv_obj_get_child(page->previous, 0), epd_get_str(gallery_previous));
    updated &= epd_app_text(lv_obj_get_child(page->next, 0), epd_get_str(gallery_next));
    if (!page->path[0])
    {
        updated &= epd_app_text(page->labels[STORAGE_FLASH], epd_get_str(gallery_flash));
        updated &= epd_app_text(page->labels[STORAGE_SD], epd_get_str(gallery_sd));
    }
    updated &= browser_status(page, page->status_code);
    if (page->request && page->status_code == GALLERY_STATUS_LOADING)
        updated &= epd_app_text(page->breadcrumb, status_text(GALLERY_STATUS_LOADING));
    if (updated) page->locale_revision = epd_locale_revision();
}
static void viewer_locale(viewer_t *page)
{
    if (page->locale_revision == epd_locale_revision()) return;
    bool updated = true;
    updated &= epd_app_text(page->title, epd_get_str(gallery_title));
    updated &= viewer_status(page, page->status_code);
    if (updated) page->locale_revision = epd_locale_revision();
}

static void gallery_box(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static void gallery_label(lv_obj_t *object, int x, int y, int width, epd_font_role_t role)
{
    const lv_font_t *font = epd_app_font_role(role);
    lv_obj_set_style_text_font(object, font, 0);
    gallery_box(object, x, y, width, font->line_height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
}

static void gallery_heading(lv_obj_t *screen, lv_obj_t *back, lv_obj_t *title, const lv_area_t *area)
{
    int x = area->x1 + epd_ui_touch(60) + epd_ui_px(16);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_back_button_layout(back, epd_ui_px(24),
        epd_ui_px(81) + epd_app_font_role(EPD_FONT_TITLE)->line_height / 2);
    gallery_label(title, x, epd_ui_px(81), area->x2 - x + 1, EPD_FONT_TITLE);
}

static void browser_scale(browser_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    if (page->scale_revision == epd_ui_scale_revision() && !memcmp(&page->scale_area, &area, sizeof(area))) return;
    int width = area.x2 - area.x1 + 1, gap = epd_ui_px(12), pad = epd_ui_px(20);
    int button = epd_ui_touch(76), bottom = area.y2 + 1 - button;
    int top = area.y1 + epd_app_font_role(EPD_FONT_SMALL)->line_height + gap;
    int row_height = (bottom - gap - top - (GALLERY_ROWS - 1) * gap) / GALLERY_ROWS;
    gallery_heading(page->screen, page->back, page->title, &area);
    gallery_label(page->breadcrumb, area.x1, area.y1, width, EPD_FONT_SMALL);
    gallery_label(page->empty, area.x1, top + (bottom - top) / 2, width, EPD_FONT_BODY);
    for (unsigned i = 0; i < GALLERY_ROWS; ++i)
    {
        gallery_box(page->rows[i], area.x1, top + i * (row_height + gap), width, row_height);
        gallery_label(page->labels[i], pad, (row_height - epd_app_font()->line_height) / 2,
                       width - 2 * pad, EPD_FONT_BODY);
    }
    int button_width = (width - gap) / 2;
    gallery_box(page->previous, area.x1, bottom, button_width, button);
    gallery_box(page->next, area.x2 + 1 - button_width, bottom, button_width, button);
    lv_obj_t *buttons[] = {page->previous, page->next};
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        gallery_label(label, 0, 0, button_width - 2 * pad, EPD_FONT_BODY);
        lv_obj_center(label);
    }
    page->scale_revision = epd_ui_scale_revision();
    page->scale_area = area;
}

static void viewer_image_position(viewer_t *page)
{
    if (!page->displayed) return;
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int width = area.x2 - area.x1 + 1;
    int height = area.y2 - area.y1 + 1 - epd_app_font_role(EPD_FONT_SMALL)->line_height - epd_ui_px(12);
    lv_obj_set_pos(page->image, (width - (int)page->displayed->image.header.w) / 2,
                   (height - (int)page->displayed->image.header.h) / 2);
}

static void viewer_scale(viewer_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    if (page->scale_revision == epd_ui_scale_revision() && !memcmp(&page->scale_area, &area, sizeof(area))) return;
    int width = area.x2 - area.x1 + 1;
    int top = area.y1 + epd_app_font_role(EPD_FONT_SMALL)->line_height + epd_ui_px(12);
    gallery_heading(page->screen, page->back, page->title, &area);
    gallery_label(page->name, area.x1, area.y1, width, EPD_FONT_SMALL);
    gallery_box(page->viewport, area.x1, top, width, area.y2 - top + 1);
    gallery_label(page->status, area.x1, top + (area.y2 - top) / 2, width, EPD_FONT_BODY);
    viewer_image_position(page);
    page->scale_revision = epd_ui_scale_revision();
    page->scale_area = area;
}

static lv_obj_t *prepare(const char *title, lv_event_cb_t back, void *data, lv_obj_t **heading, lv_obj_t **back_button)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    epd_app_object_init(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_input_group(screen);
    epd_app_header(screen);
    *back_button = back ? epd_app_back_button_cb(screen, back, data) : epd_app_back_button(screen);
    *heading = epd_app_label(screen, title, 104, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
    return screen;
}

static void browser_back(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (!page->path[0]) { gui_app_goback(); return; }
    char *slash = strrchr(page->path, '/');
    if (slash) *slash = 0;
    page->first = page->depth ? page->offsets[--page->depth] : 0;
    page->selected_row = 0;
    browser_scan(page);
}

static void row_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    unsigned index;
    for (index = 0; index < page->count; ++index)
        if (page->rows[index] == lv_event_get_target_obj(event)) break;
    if (index == page->count || !page->started || !page->rows_current) return;
    page->selected_row = index;
    if (!page->path[0])
    {
        if (!storage_available(index)) { browser_status(page, GALLERY_STATUS_STORAGE); visible(page->empty, true); return; }
        page->offsets[page->depth++] = 0;
        snprintf(page->path, sizeof(page->path), "%s", storage_root(index));
        page->first = 0;
        browser_scan(page);
        return;
    }
    int n = snprintf(page->selected, sizeof(page->selected), "%s/%s", page->path, page->entries[index].name);
    if (n <= 0 || (size_t)n >= sizeof(page->selected))
    { browser_status(page, GALLERY_STATUS_PATH); visible(page->empty, true); return; }
    if (page->entries[index].directory)
    {
        if (page->depth == 32) { browser_status(page, GALLERY_STATUS_DEPTH); visible(page->empty, true); return; }
        page->offsets[page->depth++] = page->first;
        strcpy(page->path, page->selected);
        page->first = page->selected_row = 0;
        browser_scan(page);
    }
    else
    {
        memcpy(picture_path, page->selected, sizeof(picture_path));
        picture_session = storage_card_session();
        if (gui_app_create_page_for_app_ext("gallery", "picture", viewer_message, NULL, sizeof(viewer_t)) != RT_EOK)
        { browser_status(page, GALLERY_STATUS_PAGE); visible(page->empty, true); }
    }
}

static void row_focused(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    for (unsigned i = 0; i < page->count; ++i)
        if (page->rows[i] == lv_event_get_target_obj(event)) page->selected_row = i;
}

static bool browser_open_content(browser_t *page)
{
    if (page->content_error)
    {
        page->status_content_error = page->content_error;
        browser_status(page, GALLERY_STATUS_CONTENT);
        page->content_error = RT_EOK;
        visible(page->empty, true);
        return true;
    }
    if (!page->open_content || !page->started) return false;
    page->open_content = false;
    if (storage_changing() || !storage_path_available(page->selected) ||
        (!strcmp(storage_path_root(page->selected), STORAGE_SD_ROOT) &&
         !storage_session_valid(STORAGE_SD, page->content_session)))
    {
        browser_status(page, GALLERY_STATUS_UNAVAILABLE);
        visible(page->empty, true);
        return true;
    }
    memcpy(picture_path, page->selected, sizeof(picture_path));
    picture_session = page->content_session;
    if (gui_app_create_page_for_app_ext("gallery", "picture", viewer_message, NULL, sizeof(viewer_t)) != RT_EOK)
    { browser_status(page, GALLERY_STATUS_PAGE); visible(page->empty, true); }
    return true;
}

static void list_page_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (!page->rows_current) return;
    if (lv_event_get_target_obj(event) == page->previous)
        page->first = page->first >= GALLERY_ROWS ? page->first - GALLERY_ROWS : 0;
    else page->first += GALLERY_ROWS;
    page->selected_row = 0;
    browser_scan(page);
}

static void browser_scan_failed(browser_t *page, gallery_status_t status)
{
    page->count = 0;
    page->rows_current = false;
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) visible(page->rows[i], false);
    visible(page->previous, false);
    visible(page->next, false);
    browser_status(page, status);
    visible(page->empty, true);
}

static void browser_scan(browser_t *page)
{
    gallery_cancel_request(page->request);
    page->request = 0;
    visible(page->empty, false);
    page->status_code = GALLERY_STATUS_NONE;
    epd_app_text(page->breadcrumb, page->path);
    if (!page->path[0])
    {
        page->count = STORAGE_COUNT;
        page->rows_current = true;
        epd_app_text(page->labels[STORAGE_FLASH], epd_get_str(gallery_flash));
        epd_app_text(page->labels[STORAGE_SD], epd_get_str(gallery_sd));
        for (unsigned i = 0; i < GALLERY_ROWS; ++i) visible(page->rows[i], i < STORAGE_COUNT);
        visible(page->previous, false);
        visible(page->next, false);
        lv_timer_resume(page->timer);
        return;
    }
    page->rows_current = false;
    page->request_tick = lv_tick_get();
    page->request = gallery_submit(GALLERY_SCAN, page->path, page->first);
    if (!page->request)
        browser_scan_failed(page, GALLERY_STATUS_SERVICE);
    lv_timer_resume(page->timer);
}

static void browser_poll(lv_timer_t *timer)
{
    browser_t *page = lv_timer_get_user_data(timer);
    browser_scale(page);
    browser_locale(page);
    if (!page->started)
    {
        if (page->start_attempts >= 20) return;
        page->started = gallery_start();
        if (page->started)
        {
            browser_scan(page);
            browser_open_content(page);
        }
        else if (++page->start_attempts >= 20)
        {
            browser_status(page, GALLERY_STATUS_SERVICE);
            visible(page->empty, true);
            page->request = 0;
        }
        return;
    }
    if (!page->request) return;
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request))
    {
        if (lv_tick_elaps(page->request_tick) >= 200 && page->status_code != GALLERY_STATUS_LOADING)
        {
            page->status_code = GALLERY_STATUS_LOADING;
            epd_app_text(page->breadcrumb, status_text(GALLERY_STATUS_LOADING));
        }
        return;
    }
    page->request = 0;
    epd_app_text(page->breadcrumb, page->path);
    if (!result)
    {
        browser_scan_failed(page, GALLERY_STATUS_MEMORY);
        return;
    }
    page->count = result->count;
    page->rows_current = !result->error_code;
    memcpy(page->entries, result->entries, sizeof(page->entries));
    for (unsigned i = 0; i < GALLERY_ROWS; ++i)
    {
        visible(page->rows[i], i < page->count);
        if (i < page->count)
        {
            char name[sizeof(page->entries[i].name) + sizeof("  ›")];
            snprintf(name, sizeof(name), "%.*s%s", (int)sizeof(page->entries[i].name) - 1,
                     page->entries[i].name,
                     page->entries[i].directory ? "  ›" : "");
            epd_app_text(page->labels[i], name);
        }
    }
    browser_status(page, result->error_code ? result->error_code : GALLERY_STATUS_EMPTY);
    visible(page->empty, result->error_code || !page->count);
    visible(page->previous, page->first != 0);
    visible(page->next, result->more);
    if (page->count) lv_group_focus_obj(page->rows[page->selected_row < page->count ? page->selected_row : 0]);
    gallery_result_free(result);
}

static void browser_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    browser_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->screen = prepare(epd_get_str(gallery_title), browser_back, page, &page->title, &page->back);
        page->locale_revision = epd_locale_revision();
        page->breadcrumb = epd_app_label(page->screen, "", 32, 170, 620, epd_app_font_role(EPD_FONT_SMALL));
        lv_label_set_long_mode(page->breadcrumb, LV_LABEL_LONG_DOT);
        page->empty = epd_app_centered_label(page->screen, "", 32, 520, 620, epd_app_font());
        for (unsigned i = 0; i < GALLERY_ROWS; ++i)
        {
            page->rows[i] = epd_app_button(page->screen, 32, 224 + i * 104, 620, 92, NULL, row_clicked, page);
            lv_obj_add_event_cb(page->rows[i], row_focused, LV_EVENT_FOCUSED, page);
            page->labels[i] = epd_app_label(page->rows[i], "", 20, 25, 572, epd_app_font());
            lv_label_set_long_mode(page->labels[i], LV_LABEL_LONG_DOT);
            visible(page->rows[i], false);
        }
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, epd_get_str(gallery_previous), list_page_clicked, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, epd_get_str(gallery_next), list_page_clicked, page);
        visible(page->previous, false);
        visible(page->next, false);
        page->scale_revision = ~epd_ui_scale_revision();
        browser_scale(page);
        page->timer = lv_timer_create(browser_poll, 100, page);
        lv_timer_pause(page->timer);
        page->started = gallery_start();
        memcpy(page->selected, launch_path, sizeof(page->selected));
        page->open_content = launch_path[0] != 0;
        page->content_session = launch_session;
        page->content_error = launch_error;
        launch_path[0] = 0;
        launch_error = RT_EOK;
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        bool language_changed = page->locale_revision != epd_locale_revision();
        bool scale_changed = page->scale_revision != epd_ui_scale_revision();
        browser_scale(page);
        browser_locale(page);
        lv_timer_resume(page->timer);
        if (page->started && browser_open_content(page)) return;
        if ((language_changed || scale_changed) && page->started && !page->request)
        {
            lv_timer_resume(page->timer);
            return;
        }
        if (page->started && page->path[0])
        {
            page->rows_current = false;
            page->request_tick = lv_tick_get();
            page->request = gallery_submit(GALLERY_SCAN, page->path, page->first);
            if (!page->request)
                browser_scan_failed(page, GALLERY_STATUS_SERVICE);
            lv_timer_resume(page->timer);
        }
        else if (page->started) browser_scan(page);
        else lv_timer_resume(page->timer);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        gallery_cancel_request(page->request);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        gallery_stop();
    }
}

static void release_image(viewer_t *page)
{
    if (!page->displayed) return;
    lv_image_set_src(page->image, NULL);
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&page->displayed->image);
    lv_image_header_cache_drop(&page->displayed->image);
    gallery_result_free(page->displayed);
    page->displayed = NULL;
}

static void viewer_request(viewer_t *page)
{
    page->request_tick = lv_tick_get();
    page->request = gallery_submit(GALLERY_IMAGE, page->path, 0);
    viewer_status(page, page->request ? GALLERY_STATUS_NONE : GALLERY_STATUS_SERVICE);
    lv_timer_resume(page->timer);
}

static void viewer_poll(lv_timer_t *timer)
{
    viewer_t *page = lv_timer_get_user_data(timer);
    viewer_scale(page);
    viewer_locale(page);
    if (!page->request) return;
    if (storage_changing() || !storage_path_available(page->path) ||
        (!strcmp(storage_path_root(page->path), STORAGE_SD_ROOT) &&
         !storage_session_valid(STORAGE_SD, page->card_session)))
    {
        gallery_cancel_request(page->request);
        page->request = 0;
        release_image(page);
        viewer_status(page, GALLERY_STATUS_UNAVAILABLE);
        return;
    }
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request))
    {
        if (!page->displayed && lv_tick_elaps(page->request_tick) >= 200 && page->status_code != GALLERY_STATUS_LOADING)
            viewer_status(page, GALLERY_STATUS_LOADING);
        return;
    }
    page->request = 0;
    if (!result) { viewer_status(page, GALLERY_STATUS_MEMORY); return; }
    if (result->error_code)
    {
        viewer_status(page, result->error_code);
        gallery_result_free(result);
        return;
    }
    release_image(page);
    page->displayed = result;
    viewer_status(page, GALLERY_STATUS_NONE);
    viewer_image_position(page);
    lv_image_set_src(page->image, &result->image);
}

static void viewer_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    viewer_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        memcpy(page->path, picture_path, sizeof(page->path));
        page->card_session = picture_session;
        page->screen = prepare(epd_get_str(gallery_title), NULL, NULL, &page->title, &page->back);
        page->locale_revision = epd_locale_revision();
        const char *name = strrchr(page->path, '/');
        lv_obj_t *label = epd_app_label(page->screen, name ? name + 1 : page->path,
                                       32, 170, 620, epd_app_font_role(EPD_FONT_SMALL));
        page->name = label;
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        page->viewport = epd_app_panel(page->screen, 32, 218, GALLERY_WIDTH, GALLERY_HEIGHT);
        lv_obj_set_style_border_width(page->viewport, 0, 0);
        lv_obj_set_style_radius(page->viewport, 0, 0);
        page->image = lv_image_create(page->viewport);
        page->status_code = GALLERY_STATUS_NONE;
        page->status = epd_app_centered_label(page->screen, "", 32, 600, 620, epd_app_font());
        page->scale_revision = ~epd_ui_scale_revision();
        viewer_scale(page);
        page->timer = lv_timer_create(viewer_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        bool language_changed = page->locale_revision != epd_locale_revision();
        viewer_scale(page);
        viewer_locale(page);
        lv_timer_resume(page->timer);
        if (storage_changing() || !storage_path_available(page->path) ||
            (!strcmp(storage_path_root(page->path), STORAGE_SD_ROOT) &&
             !storage_session_valid(STORAGE_SD, page->card_session)))
        {
            release_image(page);
            viewer_status(page, GALLERY_STATUS_UNAVAILABLE);
            return;
        }
        if (!page->displayed && !(language_changed && !page->request &&
            page->status_code != GALLERY_STATUS_LOADING && page->status_code != GALLERY_STATUS_NONE))
            viewer_request(page);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        gallery_cancel_request(page->request);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        gallery_cancel_request(page->request);
        release_image(page);
    }
}

int app_main(intent_t intent)
{
    launch_error = app_content_receive(intent, launch_path, sizeof(launch_path));
    if (launch_error == -ENOENT) launch_error = RT_EOK;
    launch_session = storage_card_session();
    return gui_app_regist_msg_handler_ext("gallery", browser_message, NULL, sizeof(browser_t));
}
