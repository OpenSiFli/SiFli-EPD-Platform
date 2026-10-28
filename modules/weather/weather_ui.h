#ifndef WEATHER_UI_H
#define WEATHER_UI_H
#include "platform/epd_app.h"
#include "platform/epd_i18n.h"
#include "weather.h"
#include "ui_weather_data.h"
#include "weather_icons.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN 32
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + 72)
#define PAGE_TITLE_Y 81
#define PAGE_BACK_Y 72
#define PAGE_CONTENT_Y 174
/* Main page: eight metric captions, forecast heading and refresh button.
 * Settings uses five bindings; city/input/history pages use at most four. */
#define WEATHER_TEXT_CAPACITY (UI_WEATHER_METRIC_COUNT + 2)
enum { UI_PAGE_WEATHER, UI_PAGE_WEATHER_SETTINGS, UI_PAGE_CITY, UI_PAGE_CITY_INPUT,
       UI_PAGE_CITY_HISTORY };
enum { NAV_BACK = -2, CITY_CONFIRM = -4, POPUP_CLOSE = -5, WEATHER_SYNC = -9,
       WEATHER_IMPORT = -10, CITY_ERASE = -11, CITY_DIGIT_BASE = -40,
       HISTORY_PREVIOUS = -50, HISTORY_NEXT = -51, HISTORY_CITY_BASE = -70 };

typedef struct
{
    unsigned id;
    bool foreground;
    bool city_committed;
    bool locale_retry;
    uint32_t locale_revision, scale_revision;
    lv_area_t scale_area;
    unsigned text_count;
    struct { lv_obj_t *label; lv_ext_str_id_t id; } texts[WEATHER_TEXT_CAPACITY];

    lv_obj_t *screen;
    lv_timer_t *timer;
    lv_group_t *group, *page_group;
    lv_obj_t *previous_focus;
    lv_obj_t *title, *back;
    lv_obj_t *form[16];
    struct
    {
        lv_obj_t *city, *time, *icon, *temperature, *description, *range, *source;
        lv_obj_t *metrics[UI_WEATHER_METRIC_COUNT];
        lv_obj_t *symbols[UI_WEATHER_METRIC_COUNT + 1];
        lv_obj_t *date[3], *forecast_icon[3], *text[3], *temperatures[3], *wind[3];
        lv_obj_t *content, *cards[UI_WEATHER_METRIC_COUNT], *captions[UI_WEATHER_METRIC_COUNT];
        lv_obj_t *forecast_title, *lines[3], *refresh;
        uint32_t revision, locale_revision;
        int code, forecast_code[3];
    } weather_widgets;
    lv_obj_t *city_input, *weather_config_label;
    void *history_view;
    char city_digits[WEATHER_CITY_ID_MAX];
    uint32_t weather_ticket;
    weather_job_t weather_job;
    /* Pending requests may not have a popup until the busy delay expires. */
    unsigned weather_popup_phase;
    uint32_t weather_popup_tick;
    lv_obj_t *popup;
    lv_obj_t *popup_text, *popup_button;
    lv_obj_t *popup_detail;
    weather_result_t visible_result;
} weather_page_t;
extern weather_page_t *current;
#define ui_font_body() epd_app_font_role(EPD_FONT_BODY)
#define ui_font_small() epd_app_font_role(EPD_FONT_SMALL)
#define ui_font_title() epd_app_font_role(EPD_FONT_TITLE)
#define ui_font_caption() epd_app_font_role(EPD_FONT_CAPTION)
#define ui_font_temperature() epd_app_font_role(EPD_FONT_TEMPERATURE)
#define label_create epd_app_label
#define centered_label epd_app_centered_label
#define panel_create epd_app_panel
#define icon_create epd_app_icon
#define text_update epd_app_text
#define epd_obj_init epd_app_object_init
#define lv_refreshing_done epd_app_refresh_done
#define launcher_current_page() (current->id)

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int action);
lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font);
void popup_open(const char *text, const char *detail);
void popup_close(void);
void weather_popup_result(const weather_result_t *result);
void weather_popup_code(weather_result_code_t code);
bool weather_popup_retranslate(void);
void weather_locale_update(void);
void weather_scale_update(void);
void weather_layout(void);
void weather_settings_layout(void);
void city_history_layout(void);
void weather_box(lv_obj_t *object, int x, int y, int width, int height);
void weather_label_layout(lv_obj_t *label, int x, int y, int width, const lv_font_t *font);
void weather_button_layout(lv_obj_t *button, int x, int y, int width, int height);
lv_obj_t *weather_bind_text(lv_obj_t *label, lv_ext_str_id_t id);
lv_obj_t *weather_button_text(lv_obj_t *button, lv_ext_str_id_t id);
#define weather_label(parent, key, x, y, w, font) \
    weather_bind_text(label_create(parent, "", x, y, w, font), LV_EXT_STR_ID(key))
#define weather_centered(parent, key, x, y, w, font) \
    weather_bind_text(centered_label(parent, "", x, y, w, font), LV_EXT_STR_ID(key))
#define weather_title(parent, key, font) \
    weather_bind_text(page_title_create(parent, "", font), LV_EXT_STR_ID(key))
#define weather_button(parent, x, y, w, h, key, action) \
    weather_button_text(button_create(parent, x, y, w, h, "", action), LV_EXT_STR_ID(key))
void weather_create(lv_obj_t *screen);
void weather_resume(void);
void weather_pause(void);
bool weather_time_update(void);
void weather_action_start(weather_job_t job);
void weather_ui_process(void);
void weather_data_update(void);
void city_create(lv_obj_t *screen);
void city_input_create(lv_obj_t *screen);
void city_input_pause(void);
void settings_city_committed(void);
bool weather_settings_render(void);
void weather_settings_create(lv_obj_t *screen);
void weather_page_open(unsigned page);
void weather_settings_return(void);
bool city_action(int target);
void city_history_create(lv_obj_t *screen);
void city_history_resume(void);
bool city_history_translate(void);
void city_history_stop(void);
bool city_history_action(int target);
#endif
