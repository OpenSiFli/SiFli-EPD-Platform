#include "app_service.h"
#include "epd_memory.h"
#include "platform/epd_locale.h"
#include "platform/epd_app.h"
#include "platform/epd_i18n.h"
#include "board_model.h"
#include "board_store.h"
#include <string.h>

EPD_APP_DEFINE("board");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

typedef struct
{
    board_model_t *model;
    lv_obj_t *root, *back_button, *error_label, *title;
    lv_obj_t **bodies, **headings, **panels;
    lv_timer_t *timer;
    uint32_t revision, locale_revision, scale_revision;
    int content_bottom;
    board_message_t error;
    bool foreground, observed, relayout;
} board_page_t;

static const char *error_text(board_message_t error)
{
    switch (error)
    {
    case BOARD_MESSAGE_CONFIG_UNAVAILABLE: return epd_get_str(board_config_unavailable);
    case BOARD_MESSAGE_LOAD_MEMORY: return epd_get_str(board_load_memory);
    case BOARD_MESSAGE_CONFIG_INVALID: return epd_get_str(board_config_invalid);
    case BOARD_MESSAGE_CONFIG_BOUNDS: return epd_get_str(board_config_bounds);
    case BOARD_MESSAGE_LAYOUT_BOUNDS: return epd_get_str(board_layout_bounds);
    case BOARD_MESSAGE_DISPLAY_MEMORY: return epd_get_str(board_display_memory);
    case BOARD_MESSAGE_TIMER_FAILED: return epd_get_str(board_timer_failed);
    default: return "";
    }
}

static void back(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

static bool draw(const board_command_t *c, void *context)
{
    board_page_t *page = context;
    lv_obj_t *object = NULL;
    int x = c->x, y = c->y, width = c->width, height = c->height;
    int header_height = epd_ui_touch(64), header_delta = header_height - 64;
    int back_width = epd_ui_touch(60);
    if (page->model && page->root)
    {
        int limit = lv_obj_get_width(lv_screen_active()) - 2 * (int)page->model->margin - (int)page->model->gap - 1;
        if (back_width > limit) back_width = limit;
    }
    const lv_font_t *font = epd_app_font_role(c->kind == BOARD_TITLE ? EPD_FONT_TITLE : EPD_FONT_BODY);
    if (c->kind == BOARD_BACK)
    {
        width = back_width;
        height = header_height;
    }
    else if (c->kind == BOARD_TITLE)
    {
        int delta = back_width - 96;
        x += delta;
        width -= delta;
        if (width < 1) width = 1;
        y = (int)page->model->margin + (header_height - font->line_height) / 2;
        height = font->line_height;
    }
    else
    {
        y += header_delta;
        int heading_height = font->line_height + epd_ui_px(12);
        int text_height = c->kind == BOARD_BODY ? c->height + 44 : 0;
        if (c->kind == BOARD_HEADING && page->model)
        {
            const board_card_t *card = page->model->cards;
            for (size_t i = 0; i < c->card && card; ++i) card = card->next;
            if (card) text_height = (int)card->height - 2 * (int)page->model->padding;
        }
        if (text_height && heading_height > text_height - font->line_height)
            heading_height = text_height - font->line_height;
        if (c->kind == BOARD_HEADING) height = heading_height;
        else if (c->kind == BOARD_BODY)
        {
            y += heading_height - 44;
            height -= heading_height - 44;
            if (height < 1) height = 1;
        }
    }
    if (page->relayout)
    {
        switch (c->kind)
        {
        case BOARD_BACK: object = page->back_button; break;
        case BOARD_TITLE: object = page->title; break;
        case BOARD_PANEL: object = page->panels[c->card]; break;
        case BOARD_HEADING: object = page->headings[c->card]; break;
        case BOARD_BODY: object = page->bodies[c->card]; break;
        }
    }
    else if (c->kind == BOARD_BACK)
    {
        object = epd_app_back_button_cb(page->root, back, NULL);
        page->back_button = object;
    }
    else if (c->kind == BOARD_PANEL)
    {
        object = epd_app_panel(page->root, c->x, c->y, c->width, c->height);
        page->panels[c->card] = object;
    }
    else
    {
        const char *text = c->kind == BOARD_BODY && c->summary && c->default_empty ?
                           epd_get_str(board_empty_summary) : c->text;
        object = epd_app_label(page->root, text, c->x, c->y, c->width,
                              epd_app_font_role(c->kind == BOARD_TITLE ? EPD_FONT_TITLE : EPD_FONT_BODY));
        lv_obj_set_size(object, c->width, c->height);
        lv_label_set_long_mode(object, c->kind == BOARD_BODY ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
        if (c->kind == BOARD_BODY) page->bodies[c->card] = object;
        else if (c->kind == BOARD_HEADING) page->headings[c->card] = object;
        else if (c->kind == BOARD_TITLE) page->title = object;
    }
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    if (c->kind == BOARD_BACK)
    {
        epd_app_back_button_layout(object, x, y + height / 2);
    }
    else if (c->kind != BOARD_PANEL) lv_obj_set_style_text_font(object, font, 0);
    if (c->kind == BOARD_PANEL)
    {
        lv_obj_set_style_radius(object, 0, 0);
        lv_obj_set_style_border_color(object, lv_color_black(), 0);
        lv_obj_set_style_border_width(object, 1, 0);
    }
    return true;
}

static void scale_update(board_page_t *page)
{
    if (!page->root) return;
    lv_obj_t *screen = lv_screen_active();
    lv_area_t area;
    epd_ui_content_area(screen, &area);
    if (page->scale_revision == epd_ui_scale_revision() && page->content_bottom == area.y2 + 1) return;
    lv_obj_set_size(page->root, lv_obj_get_width(screen), area.y2 + 1);
    if (page->model && (!page->model->count || page->bodies))
    {
        page->relayout = true;
        board_layout(page->model, lv_obj_get_width(screen), lv_obj_get_height(screen), draw, page);
        page->relayout = false;
    }
    else
    {
        int margin = epd_ui_px(24), button = epd_ui_touch(64);
        epd_app_back_button_layout(page->back_button, margin, margin + button / 2);
        if (page->error_label)
        {
            lv_obj_set_style_text_font(page->error_label, epd_app_font(), 0);
            lv_obj_set_pos(page->error_label, margin, margin * 2 + button);
            lv_obj_set_size(page->error_label, lv_obj_get_width(screen) - 2 * margin, LV_SIZE_CONTENT);
            lv_label_set_long_mode(page->error_label, LV_LABEL_LONG_WRAP);
        }
    }
    page->scale_revision = epd_ui_scale_revision();
    page->content_bottom = area.y2 + 1;
}

static void refresh(board_page_t *page)
{
    if (!page->foreground) return;
    scale_update(page);
    uint32_t locale_revision = epd_locale_revision();
    bool language_changed = page->locale_revision != locale_revision;
    bool updated = true;
    if (language_changed)
    {
        if (page->error_label) updated &= epd_app_text(page->error_label, error_text(page->error));
    }
    if (!page->model || !page->bodies)
    {
        if (updated) page->locale_revision = locale_revision;
        return;
    }
    uint32_t revision = epd_app_summary_revision();
    if (page->observed && page->revision == revision && !language_changed) return;
    size_t i = 0;
    for (const board_card_t *card = page->model->cards; card; card = card->next, ++i)
    {
        if (!card->summary) continue;
        char text[96] = {0};
        bool found = epd_app_summary_read(card->summary, text, sizeof(text));
        text[sizeof(text) - 1] = 0;
        updated &= epd_app_text(page->bodies[i], found && text[0] ? text :
                     (card->default_empty ? epd_get_str(board_empty_summary) : card->empty));
    }
    if (!updated) return;
    page->locale_revision = locale_revision;
    /* A publication during this pass causes a new read on the next UI tick. */
    page->revision = revision;
    page->observed = true;
}

static void poll(lv_timer_t *timer)
{
    refresh(lv_timer_get_user_data(timer));
}

static void release(board_page_t *page)
{
    page->foreground = false;
    if (page->timer) { lv_timer_delete(page->timer); page->timer = NULL; }
    if (page->root) { lv_obj_delete(page->root); page->root = NULL; }
    epd_app_free(page->bodies);
    page->bodies = NULL;
    page->headings = page->panels = NULL;
    page->back_button = page->error_label = NULL;
    board_model_free(page->model);
    page->model = NULL;
}

static lv_obj_t *root_create(lv_obj_t *screen, int32_t width, int32_t height)
{
    lv_obj_t *root = epd_app_panel(screen, 0, 0, width, height);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    epd_app_input_group(root);
    return root;
}

static void start(board_page_t *page)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    epd_app_object_init(screen);
    int32_t width = lv_obj_get_width(screen), height = lv_obj_get_height(screen);
    board_message_t message;
    board_result_t result = board_store_load(&page->model, &message);
    if (result == BOARD_OK)
    {
        result = board_layout(page->model, width, height, NULL, NULL);
        if (result != BOARD_OK) message = BOARD_MESSAGE_LAYOUT_BOUNDS;
    }
    if (result == BOARD_OK && page->model->count)
    {
        if (page->model->count <= SIZE_MAX / (3 * sizeof(*page->bodies)))
            page->bodies = epd_app_alloc(page->model->count * 3 * sizeof(*page->bodies), EPD_APP_PSRAM);
        if (!page->bodies) { result = BOARD_MEMORY; message = BOARD_MESSAGE_DISPLAY_MEMORY; }
        else
        {
            page->headings = page->bodies + page->model->count;
            page->panels = page->headings + page->model->count;
        }
    }
    if (result == BOARD_OK)
    {
        page->timer = lv_timer_create(poll, 500, page);
        if (page->timer) lv_timer_pause(page->timer);
        else { result = BOARD_MEMORY; message = BOARD_MESSAGE_TIMER_FAILED; }
    }
    if (result == BOARD_OK)
    {
        page->root = root_create(screen, width, height);
        board_layout(page->model, width, height, draw, page);
    }
    else
    {
        release(page);
        page->root = root_create(screen, width, height);
        page->error = message;
        page->back_button = epd_app_back_button_cb(page->root, back, NULL);
        page->error_label = epd_app_label(page->root, error_text(message), 24, 120, width - 48, epd_app_font());
    }
    page->locale_revision = epd_locale_revision();
    page->scale_revision = ~epd_ui_scale_revision();
    scale_update(page);
    if (!page->timer) page->timer = lv_timer_create(poll, 500, page);
    if (page->timer) lv_timer_pause(page->timer);
}

static void message(gui_app_msg_type_t type, void *parameter)
{
    (void)parameter;
    board_page_t *page = gui_app_this_page_memory();
    switch (type)
    {
    case GUI_APP_MSG_ONSTART:
        memset(page, 0, sizeof(*page));
        start(page);
        break;
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        page->observed = false;
        refresh(page);
        if (!page->timer) page->timer = lv_timer_create(poll, 500, page);
        if (page->timer) lv_timer_resume(page->timer);
        break;
    case GUI_APP_MSG_ONPAUSE:
        page->foreground = false;
        if (page->timer) lv_timer_pause(page->timer);
        break;
    case GUI_APP_MSG_ONSTOP:
        release(page);
        break;
    default: break;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("board", message, NULL, sizeof(board_page_t));
}
