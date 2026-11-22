#include "storage.h"
#include "storage_file.h"
#include "platform/epd_locale.h"
#include "gallery.h"
#include "platform/epd_i18n.h"
#include "platform/app_content.h"
#include "../common/reader/reader_view_touch_ui.h"
#include "src/draw/lv_image_decoder_private.h"
#include <string.h>
#include <stdio.h>

EPD_APP_DEFINE("gallery");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)
#define PHOTO_ROOT STORAGE_SD_ROOT

static char launch_path[STORAGE_PATH_MAX], picture_path[STORAGE_PATH_MAX], picture_root[STORAGE_PATH_MAX];
static uint32_t picture_session;
static bool picture_recursive;
static rt_err_t launch_error;

typedef struct {
    lv_obj_t *screen, *empty, *rows[GALLERY_ROWS], *labels[GALLERY_ROWS], *images[GALLERY_ROWS];
    lv_obj_t *previous, *next, *counter, *photos, *albums, *title, *back;
    gallery_entry_t entries[GALLERY_ROWS];
    gallery_result_t *thumbs[GALLERY_ROWS];
    char path[STORAGE_PATH_MAX], anchor[STORAGE_PATH_MAX], albums_anchor[STORAGE_PATH_MAX];
    unsigned first, albums_first, count, total, directories, thumb_next, thumb_row;
    uint32_t request, revision, catalog_revision, card_session, locale_revision, scale_revision;
    lv_area_t area;
    gallery_job_kind_t job;
    gallery_status_t status;
    bool started, photos_mode, album_open, current, complete;
} browser_t;

typedef struct {
    lv_obj_t *screen, *image, *status;
    char path[STORAGE_PATH_MAX], root[STORAGE_PATH_MAX];
    char previous_path[STORAGE_PATH_MAX], next_path[STORAGE_PATH_MAX];
    uint32_t request, work_serial, card_session, locale_revision;
    uint32_t catalog_revision, catalog_generation;
    int direction;
    reader_view_touch_t touch;
    gallery_result_t *displayed;
    bool recursive, unavailable, locating, complete;
    gallery_status_t status_code;
} viewer_t;

static browser_t *browser_active;
static viewer_t *viewer_active;

static void browser_scan(browser_t *page);
static void viewer_message(gui_app_msg_type_t message, void *parameter);

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static const char *status_text(gallery_status_t status)
{
    switch (status) {
    case GALLERY_STATUS_LOADING: return epd_get_str(gallery_loading);
    case GALLERY_STATUS_EMPTY: return epd_get_str(gallery_empty);
    case GALLERY_STATUS_STORAGE: return epd_get_str(gallery_storage);
    case GALLERY_STATUS_REMOVED: return epd_get_str(gallery_removed);
    case GALLERY_STATUS_PATH: return epd_get_str(gallery_path);
    case GALLERY_STATUS_PAGE: return epd_get_str(gallery_page);
    case GALLERY_STATUS_UNAVAILABLE: return epd_get_str(gallery_unavailable);
    case GALLERY_STATUS_SERVICE: return epd_get_str(gallery_service);
    case GALLERY_STATUS_MEMORY: return epd_get_str(gallery_memory);
    case GALLERY_STATUS_OPEN: return epd_get_str(gallery_open);
    case GALLERY_STATUS_SCAN: return epd_get_str(gallery_scan);
    case GALLERY_STATUS_DECODE: return epd_get_str(gallery_decode);
    default: return "";
    }
}

static void box(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static void label_box(lv_obj_t *object, int x, int y, int width, epd_font_role_t role)
{
    const lv_font_t *font = epd_app_font_role(role);
    lv_obj_set_style_text_font(object, font, 0);
    box(object, x, y, width, font->line_height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
}

static void button_text(lv_obj_t *button)
{
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, epd_app_font(), 0);
    lv_obj_set_size(label, lv_obj_get_style_width(button, 0) - epd_ui_px(16), epd_app_font()->line_height);
    lv_obj_center(label);
}

static void tile_layout(browser_t *page, unsigned row, int cell)
{
    int caption = epd_app_font()->line_height;
    bool album = row < page->count && page->entries[row].directory;
    box(page->images[row], 3, 3, cell - 6,
        album ? cell - caption - epd_ui_px(20) : cell - 6);
    label_box(page->labels[row], epd_ui_px(8),
        album ? cell - caption - epd_ui_px(8) : (cell - caption) / 2,
        cell - epd_ui_px(16), EPD_FONT_BODY);
}

static void heading(lv_obj_t *screen, lv_obj_t *back, lv_obj_t *title, const lv_area_t *area)
{
    int x = area->x1 + epd_ui_touch(60) + epd_ui_px(16);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_back_button_layout(back, epd_ui_px(24),
        epd_ui_px(81) + epd_app_font_role(EPD_FONT_TITLE)->line_height / 2);
    label_box(title, x, epd_ui_px(81), area->x2 - x + 1, EPD_FONT_TITLE);
}

static lv_obj_t *prepare(lv_event_cb_t back, void *data, lv_obj_t **title, lv_obj_t **button)
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
    *button = back ? epd_app_back_button_cb(screen, back, data) : epd_app_back_button(screen);
    *title = epd_app_label(screen, epd_get_str(gallery_title), 104, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
    return screen;
}

static void drop_image(lv_obj_t *image, gallery_result_t **slot)
{
    if (!*slot) return;
    lv_image_set_src(image, NULL);
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&(*slot)->image);
    lv_image_header_cache_drop(&(*slot)->image);
    gallery_result_free(*slot);
    *slot = NULL;
}

static void browser_status(browser_t *page, gallery_status_t status)
{
    page->status = status;
    if (page->count && status != GALLERY_STATUS_NONE && status != GALLERY_STATUS_LOADING) {
        epd_app_text(page->counter, status_text(status));
        visible(page->empty, false);
        return;
    }
    epd_app_text(page->empty, status_text(status));
    visible(page->empty, status != GALLERY_STATUS_NONE);
}

static void browser_scale(browser_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    if (page->scale_revision == epd_ui_scale_revision() && !memcmp(&page->area, &area, sizeof(area))) return;
    int width = area.x2 - area.x1 + 1, gap = epd_ui_px(12), tab = epd_ui_touch(68);
    int cell = (width - 2 * gap) / 3;
    int top = area.y1 + tab + epd_ui_px(24);
    int available = (area.y2 - top - epd_ui_touch(80) - 2 * gap) / 3;
    if (cell > available) cell = available;
    int left = area.x1 + (width - (cell * 3 + gap * 2)) / 2;
    heading(page->screen, page->back, page->title, &area);
    box(page->photos, area.x1, area.y1, (width - gap) / 2, tab);
    box(page->albums, area.x1 + (width + gap) / 2, area.y1, (width - gap) / 2, tab);
    button_text(page->photos);
    button_text(page->albums);
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) {
        box(page->rows[i], left + (i % 3) * (cell + gap), top + (i / 3) * (cell + gap), cell, cell);
        tile_layout(page, i, cell);
    }
    label_box(page->empty, area.x1, top + cell, width, EPD_FONT_BODY);
    int bottom = area.y2 - epd_ui_touch(72) + 1;
    box(page->previous, area.x1, bottom, epd_ui_touch(96), epd_ui_touch(72));
    box(page->next, area.x2 - epd_ui_touch(96) + 1, bottom, epd_ui_touch(96), epd_ui_touch(72));
    button_text(page->previous);
    button_text(page->next);
    label_box(page->counter, area.x1 + epd_ui_touch(104), bottom + epd_ui_px(12), width - epd_ui_touch(208), EPD_FONT_BODY);
    page->area = area;
    page->scale_revision = epd_ui_scale_revision();
}

static void browser_text(browser_t *page)
{
    epd_app_text(lv_obj_get_child(page->photos, 0), epd_get_str(gallery_photos));
    epd_app_text(lv_obj_get_child(page->albums, 0), epd_get_str(gallery_albums));
    const char *name = strrchr(page->path, '/');
    epd_app_text(page->title, page->album_open ?
        (!strcmp(page->path, PHOTO_ROOT) ? epd_get_str(gallery_tf_album) : name ? name + 1 : page->path) : epd_get_str(gallery_title));
    lv_obj_t *tabs[] = {page->photos, page->albums};
    for (unsigned i = 0; i < 2; ++i) {
        bool selected = (i == 0) == page->photos_mode;
        lv_obj_set_style_bg_color(tabs[i], selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(tabs[i], selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tabs[i], 0), selected ? lv_color_white() : lv_color_black(), 0);
    }
    page->locale_revision = epd_locale_revision();
}

static void browser_back(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (!page->album_open) { gui_app_goback(); return; }
    page->album_open = false;
    memcpy(page->path, PHOTO_ROOT, sizeof(PHOTO_ROOT));
    page->first = page->albums_first;
    memcpy(page->anchor, page->albums_anchor, sizeof(page->anchor));
    browser_scan(page);
}

static void open_picture(browser_t *page, unsigned row)
{
    memcpy(page->anchor, page->entries[row].path, sizeof(page->anchor));
    snprintf(picture_path, sizeof(picture_path), "%s", page->entries[row].path);
    snprintf(picture_root, sizeof(picture_root), "%s", page->path);
    picture_session = storage_card_session();
    picture_recursive = page->photos_mode;
    if (gui_app_create_page_for_app_ext("gallery", "picture", viewer_message, NULL, sizeof(viewer_t)) != RT_EOK)
        browser_status(page, GALLERY_STATUS_PAGE);
}

static void tile_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    if (!page->current) return;
    for (unsigned i = 0; i < page->count; ++i) {
        if (lv_event_get_target_obj(event) != page->rows[i]) continue;
        if (page->entries[i].directory) {
            page->albums_first = page->first;
            memcpy(page->albums_anchor, page->entries[i].path, sizeof(page->albums_anchor));
            page->anchor[0] = 0;
            page->album_open = true;
            snprintf(page->path, sizeof(page->path), "%s", page->entries[i].path);
            page->first = 0;
            browser_scan(page);
        } else open_picture(page, i);
        return;
    }
}

static void tab_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    page->photos_mode = lv_event_get_target_obj(event) == page->photos;
    page->album_open = false;
    page->albums_first = 0;
    page->anchor[0] = page->albums_anchor[0] = 0;
    snprintf(page->path, sizeof(page->path), "%s", PHOTO_ROOT);
    page->first = 0;
    browser_scan(page);
}

static void browser_move(browser_t *page, bool next)
{
    if (!page->current || !page->path[0]) return;
    if (next) {
        if (page->first + GALLERY_ROWS >= page->total) return;
        page->first += GALLERY_ROWS;
    } else {
        if (!page->first) return;
        page->first = page->first >= GALLERY_ROWS ? page->first - GALLERY_ROWS : 0;
    }
    page->anchor[0] = 0;
    browser_scan(page);
}
static void page_clicked(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    browser_move(page, lv_event_get_target_obj(event) == page->next);
}
static void browser_gesture(lv_event_t *event)
{
    browser_t *page = lv_event_get_user_data(event);
    lv_indev_t *input = lv_event_get_indev(event);
    if (!input) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_TOP || direction == LV_DIR_LEFT) browser_move(page, true);
    else if (direction == LV_DIR_BOTTOM || direction == LV_DIR_RIGHT) browser_move(page, false);
    lv_indev_wait_release(input);
}
static void browser_key(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    if (event == EPD_APP_KEY_CLICKED) browser_move(data, key == EPD_APP_KEY3);
}

static uint32_t browser_revision(const browser_t *page)
{
    (void)page;
    return gallery_catalog_generation();
}
static void browser_show(browser_t *page)
{
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) {
        visible(page->rows[i], i < page->count);
        tile_layout(page, i, lv_obj_get_style_width(page->rows[i], 0));
        visible(page->images[i], i < page->count && page->thumbs[i]);
        visible(page->labels[i], i < page->count && (page->entries[i].directory || !page->thumbs[i]));
        if (i < page->count) epd_app_text(page->labels[i],
            page->entries[i].directory && !strcmp(page->entries[i].path, PHOTO_ROOT) ?
            epd_get_str(gallery_tf_album) : page->entries[i].name);
    }
    visible(page->previous, page->first > 0);
    visible(page->next, page->first + page->count < page->total);
    char counter[32] = "";
    if (page->total && page->path[0]) snprintf(counter, sizeof(counter), "%u / %u", page->first / GALLERY_ROWS + 1,
                                              (page->total + GALLERY_ROWS - 1) / GALLERY_ROWS);
    epd_app_text(page->counter, counter);
    browser_status(page, page->count ? GALLERY_STATUS_NONE :
                   page->complete ? GALLERY_STATUS_EMPTY : GALLERY_STATUS_LOADING);
}
static void browser_scan(browser_t *page)
{
    gallery_cancel_request(page->request);
    page->request = 0;
    page->current = false;
    page->complete = false;
    page->count = page->total = page->directories = page->thumb_next = 0;
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) {
        drop_image(page->images[i], &page->thumbs[i]);
        visible(page->rows[i], false);
    }
    visible(page->previous, false);
    visible(page->next, false);
    epd_app_text(page->counter, "");
    browser_text(page);
    page->revision = browser_revision(page);
    page->catalog_revision = gallery_catalog_revision();
    page->card_session = storage_card_session();
    if (!page->started) { browser_status(page, GALLERY_STATUS_SERVICE); return; }
    if (!storage_path_available(page->path)) { browser_status(page, GALLERY_STATUS_STORAGE); return; }
    page->job = !page->photos_mode && !page->album_open ? GALLERY_ALBUMS : GALLERY_SCAN;
    page->request = gallery_submit_page(page->job, page->path, page->first, page->photos_mode, page->anchor);
    browser_status(page, page->request ? GALLERY_STATUS_LOADING : GALLERY_STATUS_SERVICE);
}
static void request_thumbnail(browser_t *page)
{
    if (!page->current || page->request) return;
    while (page->thumb_next < page->count) {
        unsigned row = page->thumb_next++;
        if (page->thumbs[row]) continue;
        page->thumb_row = row;
        page->job = GALLERY_THUMB;
        const char *source = page->entries[row].directory ? page->entries[row].cover : page->entries[row].path;
        if (!source[0]) continue;
        page->request = gallery_submit(GALLERY_THUMB, source, 0);
        break;
    }
}

static void browser_apply(browser_t *page, const gallery_result_t *result)
{
    /* A rebuilt catalog may not have reached this path yet. Keep the visible
     * page and its references until it does, or proves the path is gone. */
    if (page->current && !result->complete && !result->count && !result->error_code) {
        page->complete = false;
        page->catalog_revision = result->catalog_revision;
        page->revision = result->catalog_generation;
        return;
    }
    gallery_result_t *kept[GALLERY_ROWS] = {0};
    for (unsigned i = 0; i < result->count; ++i)
        for (unsigned j = 0; j < page->count; ++j)
            if (page->thumbs[j] && !strcmp(result->entries[i].path, page->entries[j].path) &&
                !strcmp(result->entries[i].cover, page->entries[j].cover)) {
                kept[i] = page->thumbs[j];
                page->thumbs[j] = NULL;
                lv_image_set_src(page->images[j], NULL);
                break;
            }
    for (unsigned i = 0; i < GALLERY_ROWS; ++i) {
        drop_image(page->images[i], &page->thumbs[i]);
        page->thumbs[i] = kept[i];
        if (kept[i]) lv_image_set_src(page->images[i], &kept[i]->image);
    }
    memcpy(page->entries, result->entries, sizeof(page->entries));
    page->count = result->count;
    page->total = result->total;
    page->directories = result->directories;
    page->first = result->index;
    if (result->count && (!page->anchor[0] || !result->located))
        memcpy(page->anchor, result->entries[0].path, sizeof(page->anchor));
    page->complete = result->complete;
    page->catalog_revision = result->catalog_revision;
    page->revision = result->catalog_generation ? result->catalog_generation : browser_revision(page);
    page->thumb_next = 0;
    page->current = true;
    browser_show(page);
}

static void browser_update(browser_t *page)
{
    browser_scale(page);
    if (page->locale_revision != epd_locale_revision()) {
        browser_text(page);
        browser_status(page, page->status);
    }
    if (storage_changing()) return;
    bool sd = !strcmp(storage_path_root(page->path), STORAGE_SD_ROOT);
    if (sd && page->card_session != storage_card_session()) {
        page->anchor[0] = page->albums_anchor[0] = 0;
        browser_scan(page);
        return;
    }
    if (page->path[0] && page->current && page->revision != browser_revision(page)) {
        gallery_cancel_request(page->request);
        page->complete = false;
        page->revision = browser_revision(page);
        page->job = !page->photos_mode && !page->album_open ? GALLERY_ALBUMS : GALLERY_SCAN;
        page->request = gallery_submit_page(page->job, page->path, page->first, page->photos_mode, page->anchor);
        return;
    }
    if (!page->request) {
        request_thumbnail(page);
        if (!page->request && !page->complete && page->catalog_revision != gallery_catalog_revision()) {
            page->job = !page->photos_mode && !page->album_open ? GALLERY_ALBUMS : GALLERY_SCAN;
            page->request = gallery_submit_page(page->job, page->path, page->first, page->photos_mode, page->anchor);
        }
        return;
    }
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request)) return;
    page->request = 0;
    if (page->job == GALLERY_SCAN || page->job == GALLERY_ALBUMS) {
        if (result && result->error_code == GALLERY_STATUS_REMOVED &&
            storage_session_valid(STORAGE_SD, page->card_session)) {
            /* A volume revision changed during selection, on the same card. */
            result->error_code = GALLERY_STATUS_NONE;
            result->complete = false;
            result->count = 0;
            result->index = page->first;
            result->catalog_revision = result->catalog_generation = 0;
        }
        if (result) browser_apply(page, result);
        if (!result || result->error_code) {
            browser_status(page, result ? result->error_code : GALLERY_STATUS_MEMORY);
            gallery_result_free(result);
            return;
        }
        gallery_result_free(result);
    } else {
        unsigned row = page->thumb_row;
        if (result && !result->error_code) {
            page->thumbs[row] = result;
            lv_image_set_src(page->images[row], &result->image);
            visible(page->images[row], true);
            visible(page->labels[row], page->entries[row].directory);
        } else {
            gallery_result_free(result);
            if (!page->entries[row].directory)
                epd_app_text(page->labels[row], epd_get_str(gallery_bad_image));
            visible(page->labels[row], true);
        }
    }
    request_thumbnail(page);
}

static void browser_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    browser_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART) {
        page->screen = prepare(browser_back, page, &page->title, &page->back);
        page->photos_mode = false;
        strcpy(page->path, PHOTO_ROOT);
        page->photos = epd_app_button(page->screen, 0, 0, 1, 1, epd_get_str(gallery_photos), tab_clicked, page);
        page->albums = epd_app_button(page->screen, 0, 0, 1, 1, epd_get_str(gallery_albums), tab_clicked, page);
        page->empty = epd_app_centered_label(page->screen, "", 32, 520, 620, epd_app_font());
        for (unsigned i = 0; i < GALLERY_ROWS; ++i) {
            page->rows[i] = epd_app_button(page->screen, 0, 0, 1, 1, NULL, tile_clicked, page);
            lv_obj_set_style_border_width(page->rows[i], 2, 0);
            lv_obj_set_style_border_color(page->rows[i], lv_color_hex(0x555555), 0);
            lv_obj_set_style_radius(page->rows[i], epd_ui_px(12), 0);
            page->images[i] = lv_image_create(page->rows[i]);
            lv_image_set_inner_align(page->images[i], LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_remove_flag(page->images[i], LV_OBJ_FLAG_CLICKABLE);
            page->labels[i] = epd_app_centered_label(page->rows[i], "", 0, 0, 1, epd_app_font());
            visible(page->rows[i], false);
        }
        page->previous = epd_app_button(page->screen, 0, 0, 1, 1, "‹", page_clicked, page);
        page->next = epd_app_button(page->screen, 0, 0, 1, 1, "›", page_clicked, page);
        page->counter = epd_app_centered_label(page->screen, "", 0, 0, 1, epd_app_font());
        lv_obj_add_event_cb(page->screen, browser_gesture, LV_EVENT_GESTURE, page);
        lv_obj_remove_flag(page->screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
        epd_app_input_listen(page->screen, browser_key, page);
        page->scale_revision = ~epd_ui_scale_revision();
        browser_scale(page);
        page->started = gallery_start();
        browser_scan(page);
    } else if (message == GUI_APP_MSG_ONRESUME) {
        epd_app_header_refresh(page->screen);
        browser_active = page;
        if (launch_error != RT_EOK) {
            epd_app_text(page->empty, app_content_error(launch_error));
            visible(page->empty, true);
            launch_error = RT_EOK;
        }
        if (launch_path[0]) {
            strcpy(picture_path, launch_path);
            launch_path[0] = 0;
            picture_session = storage_card_session();
            picture_recursive = false;
            memcpy(picture_root, picture_path, sizeof(picture_root));
            char *slash = strrchr(picture_root, '/');
            if (slash) *slash = 0;
            if (gui_app_create_page_for_app_ext("gallery", "picture", viewer_message, NULL, sizeof(viewer_t)) != RT_EOK)
                browser_status(page, GALLERY_STATUS_PAGE);
            return;
        }
        if (!page->current && !page->request) browser_scan(page);
        browser_update(page);
    } else if (message == GUI_APP_MSG_ONPAUSE) {
        if (browser_active == page) browser_active = NULL;
        gallery_cancel_request(page->request);
        if (page->request && page->job == GALLERY_THUMB) page->thumb_next = page->thumb_row;
        page->request = 0;
    } else if (message == GUI_APP_MSG_ONSTOP) {
        if (browser_active == page) browser_active = NULL;
        gallery_stop();
        for (unsigned i = 0; i < GALLERY_ROWS; ++i) drop_image(page->images[i], &page->thumbs[i]);
    }
}

static void viewer_status(viewer_t *page, gallery_status_t status)
{
    page->status_code = status;
    epd_app_text(page->status, status_text(status));
    visible(page->status, status != GALLERY_STATUS_NONE);
}
static void viewer_request(viewer_t *page)
{
    page->locating = false;
    page->complete = false;
    page->previous_path[0] = page->next_path[0] = 0;
    page->request = gallery_submit(GALLERY_IMAGE, page->path, 0);
    page->work_serial = page->request;
    viewer_status(page, page->request ? (page->displayed ? GALLERY_STATUS_NONE : GALLERY_STATUS_LOADING) : GALLERY_STATUS_SERVICE);
}
static void viewer_locate(viewer_t *page)
{
    page->locating = true;
    page->catalog_revision = gallery_catalog_revision();
    page->catalog_generation = gallery_catalog_generation();
    page->request = gallery_submit_location(page->root, page->path, page->recursive, page->direction);
    page->work_serial = page->request;
    if (!page->request) viewer_status(page, GALLERY_STATUS_SERVICE);
}
static void viewer_move(viewer_t *page, bool next)
{
    const char *path = next ? page->next_path : page->previous_path;
    if (page->unavailable || !path[0]) return;
    snprintf(page->path, sizeof(page->path), "%s", path);
    page->direction = next ? 1 : -1;
    viewer_request(page);
}

static void viewer_touch(lv_event_t *event)
{
    if (!reader_view_touch_input(event)) return;
    viewer_t *page = lv_event_get_user_data(event);
    bool active = page->touch.active;
    reader_touch_action_t action = reader_view_touch_event(&page->touch, event);
    if (action == READER_TOUCH_NEXT || action == READER_TOUCH_PREVIOUS)
        viewer_move(page, action == READER_TOUCH_NEXT);
    else if (lv_event_get_code(event) == LV_EVENT_RELEASED && active && !page->touch.active &&
             !page->touch.moved && !page->touch.held)
        gui_app_goback();
}
static void viewer_key(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    if (event == EPD_APP_KEY_CLICKED) viewer_move(data, key == EPD_APP_KEY3);
}
static void viewer_update(viewer_t *page)
{
    if (page->locale_revision != epd_locale_revision()) {
        page->locale_revision = epd_locale_revision();
        viewer_status(page, page->status_code);
    }
    bool sd = !strcmp(storage_path_root(page->path), STORAGE_SD_ROOT);
    bool available = !storage_changing() && storage_path_available(page->path);
    if (!available || (sd && !storage_session_valid(STORAGE_SD, page->card_session))) {
        if (!page->unavailable) {
            gallery_cancel_request(page->request);
            page->request = 0;
            drop_image(page->image, &page->displayed);
            page->unavailable = true;
            viewer_status(page, GALLERY_STATUS_UNAVAILABLE);
        }
        if (!available) return;
        page->card_session = storage_card_session();
    }
    if (page->unavailable && available) {
        page->unavailable = false;
        viewer_request(page);
    }
    if (!page->request) {
        bool rebuilt = page->catalog_generation != gallery_catalog_generation();
        if (rebuilt) {
            page->complete = false;
            page->previous_path[0] = page->next_path[0] = 0;
        }
        if (page->displayed && page->displayed->source_revision &&
            page->displayed->source_revision != storage_path_revision(page->path)) {
            viewer_request(page);
            return;
        }
        if (page->root[0] && (rebuilt || (!page->complete && page->catalog_revision != gallery_catalog_revision())))
            viewer_locate(page);
        return;
    }
    gallery_result_t *result = gallery_take(page->request);
    if (!result && !gallery_failed(page->request)) return;
    page->request = 0;
    if (page->locating) {
        page->locating = false;
        if (!result) { viewer_status(page, GALLERY_STATUS_MEMORY); return; }
        if (result->error_code == GALLERY_STATUS_REMOVED &&
            storage_session_valid(STORAGE_SD, page->card_session)) {
            result->error_code = GALLERY_STATUS_NONE;
            result->complete = result->located = false;
            result->catalog_revision = result->catalog_generation = 0;
        }
        page->catalog_revision = result->catalog_revision;
        page->catalog_generation = result->catalog_generation;
        page->complete = result->complete;
        page->previous_path[0] = page->next_path[0] = 0;
        if (result->located && !result->error_code) {
            memcpy(page->previous_path, result->previous, sizeof(page->previous_path));
            memcpy(page->next_path, result->next, sizeof(page->next_path));
        }
        if (result->error_code) viewer_status(page, result->error_code);
        else if (!result->located && result->complete) {
            drop_image(page->image, &page->displayed);
            viewer_status(page, GALLERY_STATUS_UNAVAILABLE);
        }
        gallery_result_free(result);
        return;
    }
    page->catalog_revision = UINT32_MAX;
    page->catalog_generation = gallery_catalog_generation();
    if (!result || result->error_code) {
        drop_image(page->image, &page->displayed);
        if (result && result->path[0]) snprintf(page->path, sizeof(page->path), "%s", result->path);
        viewer_status(page, result ? result->error_code : GALLERY_STATUS_MEMORY);
        gallery_result_free(result);
        return;
    }
    drop_image(page->image, &page->displayed);
    page->displayed = result;
    if (result->path[0])
        snprintf(page->path, sizeof(page->path), "%s", result->path);
    lv_image_set_src(page->image, &result->image);
    visible(page->image, true);
    viewer_status(page, GALLERY_STATUS_NONE);
    viewer_locate(page);
}

void gallery_ui_update(void)
{
    lv_obj_t *screen = lv_screen_active();
    if (viewer_active && viewer_active->screen == screen) viewer_update(viewer_active);
    else if (browser_active && browser_active->screen == screen) browser_update(browser_active);
}

static void viewer_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    viewer_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART) {
        strcpy(page->path, picture_path);
        strcpy(page->root, picture_root);
        page->direction = 1;
        page->catalog_revision = UINT32_MAX;
        if (!page->root[0]) {
            memcpy(page->root, page->path, sizeof(page->root));
            char *slash = strrchr(page->root, '/');
            if (slash) *slash = 0;
        }
        page->recursive = picture_recursive;
        page->card_session = picture_session;
        gui_app_close_anim();
        page->screen = lv_screen_active();
        epd_app_object_init(page->screen);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        lv_obj_add_flag(page->screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        page->image = lv_image_create(page->screen);
        lv_obj_set_size(page->image, lv_pct(100), lv_pct(100));
        lv_image_set_inner_align(page->image, LV_IMAGE_ALIGN_CONTAIN);
        page->status = epd_app_centered_label(page->screen, "", 0, 0, GALLERY_WIDTH - 64, epd_app_font());
        lv_obj_center(page->status);
        lv_obj_add_event_cb(page->screen, viewer_touch, LV_EVENT_ALL, page);
        epd_app_input_listen(page->screen, viewer_key, page);
    } else if (message == GUI_APP_MSG_ONRESUME) {
        viewer_active = page;
        if (!page->displayed && !page->request) viewer_request(page);
        viewer_update(page);
    } else if (message == GUI_APP_MSG_ONPAUSE) {
        if (viewer_active == page) viewer_active = NULL;
        reader_view_touch_reset(&page->touch);
        gallery_cancel_request(page->work_serial);
        page->request = 0;
    } else if (message == GUI_APP_MSG_ONSTOP) {
        if (viewer_active == page) viewer_active = NULL;
        gallery_cancel_request(page->work_serial);
        drop_image(page->image, &page->displayed);
    }
}

int app_main(intent_t intent)
{
    launch_error = app_content_receive(intent, launch_path, sizeof(launch_path));
    if (launch_error == -ENOENT) launch_error = RT_EOK;
    if (launch_path[0] && !gallery_sd_path(launch_path)) {
        launch_path[0] = 0;
        launch_error = -EINVAL;
    }
    return gui_app_regist_msg_handler_ext("gallery", browser_message, NULL, sizeof(browser_t));
}
