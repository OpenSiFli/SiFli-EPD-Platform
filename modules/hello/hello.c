#include "platform/epd_locale.h"
#include "platform/epd_app.h"
#include "platform/epd_i18n.h"

EPD_APP_DEFINE("hello");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

typedef struct
{
    lv_obj_t *screen, *back, *title, *count_label, *count_button, *detail_button, *body;
    lv_timer_t *timer;
    uint32_t locale_revision, scale_revision;
    unsigned count;
    bool detail;
} hello_page_t;

static void layout(hello_page_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int gap = epd_ui_px(24), width = area.x2 - area.x1 + 1;
    const lv_font_t *body = epd_app_font_role(EPD_FONT_BODY);
    const lv_font_t *title = epd_app_font_role(EPD_FONT_TITLE);
    lv_obj_set_style_text_font(page->screen, body, 0);
    epd_app_back_button_layout(page->back, epd_ui_px(24), epd_ui_px(81) + title->line_height / 2);
    int title_x = area.x1 + epd_ui_touch(60) + epd_ui_px(16);
    lv_obj_set_pos(page->title, title_x, epd_ui_px(81));
    lv_obj_set_size(page->title, area.x2 + 1 - title_x, lv_font_get_line_height(title) + gap);
    lv_obj_set_style_text_font(page->title, title, 0);
    lv_label_set_long_mode(page->title, LV_LABEL_LONG_DOT);
    int y = area.y1 + gap;
    lv_obj_t *label = page->detail ? page->body : page->count_label;
    lv_obj_set_style_text_font(label, body, 0);
    lv_obj_set_pos(label, area.x1, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    if (page->detail) lv_obj_set_height(label, area.y2 + 1 - y);
    else
    {
        int line = lv_font_get_line_height(body);
        lv_obj_set_height(label, line * 2);
        y += line * 2 + gap;
        lv_obj_t *buttons[] = {page->count_button, page->detail_button};
        for (unsigned i = 0; i < 2; ++i)
        {
            lv_obj_t *text = lv_obj_get_child(buttons[i], 0);
            lv_point_t size;
            lv_text_get_size(&size, lv_label_get_text(text), body, 0, 0, width - gap, LV_TEXT_FLAG_NONE);
            int height = LV_MAX(epd_ui_touch(100), size.y + gap);
            lv_obj_set_pos(buttons[i], area.x1, y);
            lv_obj_set_size(buttons[i], width, height);
            lv_obj_set_style_text_font(text, body, 0);
            lv_obj_set_width(text, width - gap);
            lv_obj_center(text);
            y += height + gap;
        }
    }
    page->scale_revision = epd_ui_scale_revision();
}

static bool count_text(hello_page_t *page)
{
    char text[64];
    EPD_FMT_hello_count(text, sizeof(text), page->count);
    return epd_app_text(page->count_label, text);
}

static void localize(hello_page_t *page)
{
    uint32_t revision = epd_locale_revision();
    if (page->locale_revision == revision)
    {
        if (page->scale_revision != epd_ui_scale_revision()) layout(page);
        return;
    }
    bool updated = epd_app_text(page->title, page->detail ? epd_get_str(hello_detail_title) : epd_get_str(hello_title));
    if (page->detail) updated &= epd_app_text(page->body, epd_get_str(hello_detail_body));
    else
    {
        updated &= count_text(page);
        updated &= epd_app_text(lv_obj_get_child(page->count_button, 0), epd_get_str(hello_count_button));
        updated &= epd_app_text(lv_obj_get_child(page->detail_button, 0), epd_get_str(hello_open_detail));
    }
    layout(page);
    if (updated) page->locale_revision = revision;
}

static void poll(lv_timer_t *timer) { localize(lv_timer_get_user_data(timer)); }

static lv_obj_t *screen_prepare(hello_page_t *page)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    page->screen = screen;
    page->back = epd_app_back_button(screen);
    page->title = epd_app_label(screen, "", 104, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
    return screen;
}

static void detail_message(gui_app_msg_type_t message, void *parameter);

static void detail_clicked(lv_event_t *event)
{
    (void)event;
    gui_app_create_page_for_app_ext("hello", "detail", detail_message, NULL, sizeof(hello_page_t));
}

static void count_clicked(lv_event_t *event)
{
    hello_page_t *page = lv_event_get_user_data(event);
    ++page->count;
    count_text(page);
}

static lv_obj_t *button_create(lv_obj_t *screen, int y, const char *text,
                              lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = epd_app_button(screen, 80, y, 524, 100, text, callback, data);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    return button;
}

static void page_message(gui_app_msg_type_t message, bool detail)
{
    hello_page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->detail = detail;
        lv_obj_t *screen = screen_prepare(page);
        if (detail)
            page->body = epd_app_label(screen, "", 80, 330, 524, epd_app_font());
        else
        {
            page->count_label = epd_app_label(screen, "", 80, 300, 524, epd_app_font());
            page->count_button = button_create(screen, 430, "", count_clicked, page);
            page->detail_button = button_create(screen, 570, "", detail_clicked, NULL);
        }
        page->locale_revision = epd_locale_revision() - 1u;
        localize(page);
        page->timer = lv_timer_create(poll, 250, page);
        if (page->timer) lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        localize(page);
        if (page->timer) lv_timer_resume(page->timer);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        if (page->timer) lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        if (page->timer) lv_timer_delete(page->timer);
        page->timer = NULL;
    }
}

static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    page_message(message, true);
}

static void root_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    page_message(message, false);
}

/* app_fwk calls this entry on its UI thread, not in module_init. */
int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("hello", root_message, NULL, sizeof(hello_page_t));
}
