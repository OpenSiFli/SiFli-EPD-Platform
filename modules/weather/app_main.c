#include "platform/epd_locale.h"
#include "weather_ui.h"
#include "storage.h"

EPD_APP_DEFINE("weather");
weather_page_t *current;

static const char *const page_names[] = {"root", "settings", "city", "city_input", "city_history"};
static void page_message(gui_app_msg_type_t message, void *parameter);
static void action_clicked(lv_event_t *event);

lv_obj_t *weather_bind_text(lv_obj_t *label, lv_ext_str_id_t id)
{
    if (!label) return NULL;
    if (current->text_count < WEATHER_TEXT_CAPACITY)
    {
        current->texts[current->text_count].label = label;
        current->texts[current->text_count++].id = id;
    }
    if (!text_update(label, LV_EXT_STR_GET(id))) current->locale_retry = true;
    return label;
}

lv_obj_t *weather_button_text(lv_obj_t *button, lv_ext_str_id_t id)
{
    weather_bind_text(lv_obj_get_child(button, 0), id);
    return button;
}

void weather_locale_update(void)
{
    uint32_t revision = epd_locale_revision();
    if (current->locale_revision == revision && !current->locale_retry) return;
    bool translated = true;
    for (unsigned i = 0; i < current->text_count; ++i)
        translated = text_update(current->texts[i].label, LV_EXT_STR_GET(current->texts[i].id)) && translated;
    weather_data_update();
    if (current->id == UI_PAGE_WEATHER) translated = weather_time_update() && translated;
    else if (current->id == UI_PAGE_WEATHER_SETTINGS) translated = weather_settings_render() && translated;
    else if (current->id == UI_PAGE_CITY_HISTORY)
    {
        translated = city_history_translate() && translated;
        city_history_layout();
    }
    if (current->popup) translated = weather_popup_retranslate() && translated;
    current->locale_retry = !translated;
    if (translated) current->locale_revision = revision;
}

lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    current->title = label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y, WIDTH - MARGIN - PAGE_TITLE_X, font);
    return current->title;
}

void weather_box(lv_obj_t *object, int x, int y, int width, int height)
{
    if (!object) return;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

void weather_label_layout(lv_obj_t *label, int x, int y, int width, const lv_font_t *font)
{
    if (!label) return;
    lv_obj_set_style_text_font(label, font, 0);
    weather_box(label, x, y, width, font->line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void weather_button_layout(lv_obj_t *button, int x, int y, int width, int height)
{
    weather_box(button, x, y, width, height);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label)
    {
        weather_label_layout(label, 0, 0, width - epd_ui_px(24), ui_font_body());
        lv_obj_center(label);
    }
}

static void popup_layout(void)
{
    if (!current->popup) return;
    int width = lv_obj_get_width(current->screen), height = lv_obj_get_height(current->screen);
    int pad = epd_ui_px(24), gap = epd_ui_px(10), button = epd_ui_touch(64);
    int text = ui_font_body()->line_height * 3 + gap;
    int detail = ui_font_small()->line_height * 4 + gap;
    int w = width - epd_ui_px(108), h = pad * 2 + text + detail + button + gap * 2;
    weather_box(current->popup, 0, 0, width, height);
    lv_obj_t *dialog = lv_obj_get_child(current->popup, 0);
    weather_box(dialog, (width - w) / 2, (height - h) / 2, w, h);
    lv_obj_set_style_radius(dialog, epd_ui_px(25), 0);
    weather_label_layout(current->popup_text, pad, pad, w - 2 * pad, ui_font_body());
    lv_label_set_long_mode(current->popup_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->popup_text, text);
    weather_label_layout(current->popup_detail, pad, pad + text + gap, w - 2 * pad, ui_font_small());
    lv_label_set_long_mode(current->popup_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->popup_detail, detail);
    weather_button_layout(current->popup_button, pad, h - pad - button, w - 2 * pad, button);
    lv_obj_set_style_radius(current->popup_button, epd_ui_px(20), 0);
}

void weather_scale_update(void)
{
    uint32_t revision = epd_ui_scale_revision();
    lv_area_t area;
    epd_ui_content_area(current->screen, &area);
    if (current->scale_revision == revision && !memcmp(&current->scale_area, &area, sizeof(area))) return;
    lv_obj_set_style_text_font(current->screen, ui_font_body(), 0);
    int title_x = area.x1 + epd_ui_touch(60) + epd_ui_px(16);
    epd_app_back_button_layout(current->back, epd_ui_px(24),
        epd_ui_px(PAGE_TITLE_Y) + ui_font_title()->line_height / 2);
    weather_label_layout(current->title, title_x, epd_ui_px(PAGE_TITLE_Y), area.x2 - title_x + 1, ui_font_title());
    switch (current->id)
    {
    case UI_PAGE_WEATHER: weather_layout(); break;
    case UI_PAGE_CITY_HISTORY: city_history_layout(); break;
    default: weather_settings_layout(); break;
    }
    popup_layout();
    current->scale_revision = revision;
    current->scale_area = area;
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int action)
{
    lv_obj_t *button = epd_app_button(parent, x, y, w, h, text,
                                     action_clicked, (void *)(intptr_t)action);
    return button;
}

void popup_close(void)
{
    if (!current->popup) return;
    lv_obj_delete(current->popup);
    current->popup = current->popup_text = current->popup_button = NULL;
    current->popup_detail = NULL;
    memset(&current->visible_result, 0, sizeof(current->visible_result));
    current->weather_popup_phase = 0;
    current->group = current->page_group;
    if (current->previous_focus) lv_group_focus_obj(current->previous_focus);
}

void popup_open(const char *text, const char *detail)
{
    if (current->popup) return;
    current->previous_focus = lv_group_get_focused(current->group);
    current->popup = panel_create(current->screen, 0, 0, WIDTH, HEIGHT);
    current->group = epd_app_input_group(current->popup);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(current->popup, 54, 398, 576, 362);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    current->popup_text = centered_label(dialog, text, 24, 32, 528, ui_font_body());
    lv_label_set_long_mode(current->popup_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->popup_text, 100);
    current->popup_detail = centered_label(dialog, detail ? detail : "", 24, 142, 528, ui_font_small());
    lv_label_set_long_mode(current->popup_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->popup_detail, 116);
    current->popup_button = button_create(dialog, 108, 274, 360, 64, epd_get_str(weather_ok), POPUP_CLOSE);
    popup_layout();
    epd_app_input_back(current->popup, current->popup_button);
    lv_group_focus_obj(current->popup_button);
}

bool weather_popup_retranslate(void)
{
    if (!current->popup) return true;
    char text[192];
    ui_weather_result_text(&current->visible_result, text, sizeof(text));
    bool translated = text_update(current->popup_text, text);
    translated = text_update(current->popup_detail, current->visible_result.detail) && translated;
    translated = text_update(lv_obj_get_child(current->popup_button, 0), epd_get_str(weather_ok)) && translated;
    if (!translated) current->locale_retry = true;
    return translated;
}

void weather_popup_result(const weather_result_t *result)
{
    if (current->popup) return;
    current->visible_result = *result;
    char text[192];
    ui_weather_result_text(result, text, sizeof(text));
    popup_open(text, result->detail);
}

void weather_popup_code(weather_result_code_t code)
{
    weather_result_t result = {0};
    result.code = code;
    weather_popup_result(&result);
}

void weather_page_open(unsigned page)
{
    if (page >= sizeof(page_names) / sizeof(page_names[0])) return;
    if (gui_app_create_page_for_app_ext("weather", page_names[page], page_message,
             (void *)(uintptr_t)page, sizeof(weather_page_t)) != RT_EOK)
        weather_popup_code(WEATHER_RESULT_PAGE_OPEN_FAILED);
}

void weather_settings_return(void)
{
    /* A settings Intent gives this application a settings root page. */
    if (gui_app_goback_to_page("root") != RT_EOK) weather_popup_code(WEATHER_RESULT_BACK_FAILED);
}

static void action_clicked(lv_event_t *event)
{
    if (!current || !current->foreground || storage_changing() || current->weather_popup_phase ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    lv_group_focus_obj(lv_event_get_target_obj(event));
    if (action == POPUP_CLOSE) popup_close();
    else if (action == NAV_BACK)
    {
        if (current->popup) popup_close();
        else gui_app_goback();
    }
    else if (action == WEATHER_SYNC) weather_action_start(WEATHER_JOB_SYNC);
    else if (action == WEATHER_IMPORT) weather_action_start(WEATHER_JOB_IMPORT);
    else if (city_action(action)) return;
    else if (city_history_action(action)) return;
    else if (action >= 0) weather_page_open((unsigned)action);
}

static void page_tick(lv_timer_t *timer)
{
    weather_page_t *page = lv_timer_get_user_data(timer);
    if (page == current && page->foreground) weather_ui_process();
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    weather_page_t *page = gui_app_this_page_memory();
    weather_page_t *previous = current;
    current = page;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
    {
        gui_app_close_anim();
        page->id = (unsigned)(uintptr_t)gui_app_this_page_userdata();
        page->locale_revision = epd_locale_revision();
        page->locale_retry = true;
        page->screen = lv_screen_active();
        page->page_group = page->group = epd_app_input_group(page->screen);
        epd_obj_init(page->screen);
        lv_obj_set_size(page->screen, WIDTH, HEIGHT);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(page->screen, ui_font_body(), 0);
        epd_app_header(page->screen);
        switch (page->id)
        {
        case UI_PAGE_WEATHER: weather_create(page->screen); break;
        case UI_PAGE_WEATHER_SETTINGS: weather_settings_create(page->screen); break;
        case UI_PAGE_CITY: city_create(page->screen); break;
        case UI_PAGE_CITY_INPUT: city_input_create(page->screen); break;
        case UI_PAGE_CITY_HISTORY: city_history_create(page->screen); break;
        }
        page->back = epd_app_back_button_cb(page->screen, action_clicked, (void *)(intptr_t)NAV_BACK);
        page->scale_revision = ~epd_ui_scale_revision();
        weather_scale_update();
        page->timer = lv_timer_create(page_tick, 250, page);
        lv_timer_pause(page->timer);
        current = previous;
        break;
    }
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        weather_scale_update();
        weather_locale_update();
        epd_app_header_refresh(page->screen);
        weather_data_update();
        if (page->id == UI_PAGE_WEATHER) weather_resume();
        else if (page->id == UI_PAGE_WEATHER_SETTINGS) weather_settings_render();
        else if (page->id == UI_PAGE_CITY_HISTORY) city_history_resume();
        lv_timer_resume(page->timer);
        weather_ui_process();
        break;
    case GUI_APP_MSG_ONPAUSE:
        lv_timer_pause(page->timer);
        if (page->id == UI_PAGE_WEATHER) weather_pause();
        if (page->id == UI_PAGE_CITY_INPUT) city_input_pause();
        page->foreground = false;
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        lv_timer_delete(page->timer);
        if (page->id == UI_PAGE_CITY_HISTORY) city_history_stop();
        current = previous == page ? NULL : previous;
        /* Screen deletion releases input groups; app_fwk owns page memory. */
        break;
    default:
        current = previous;
        break;
    }
}

int app_main(intent_t intent)
{
    if (!weather_service_ready()) return -RT_ENOMEM;
    const char *entry = intent_get_string(intent, "page");
    unsigned root = entry && !strcmp(entry, "settings") ? UI_PAGE_WEATHER_SETTINGS : UI_PAGE_WEATHER;
    return gui_app_regist_msg_handler_ext("weather", page_message,
                 (void *)(uintptr_t)root, sizeof(weather_page_t));
}
