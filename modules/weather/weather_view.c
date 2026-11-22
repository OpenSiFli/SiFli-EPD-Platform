#include "platform/app_image.h"
#include "weather_ui.h"

void weather_pause(void)
{
    epd_app_image_release(current->weather_widgets.icon);
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
        epd_app_image_release(current->weather_widgets.forecast_icon[i]);
    current->weather_widgets.icons_loaded = false;
}

static lv_obj_t *weather_image_create(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *image = icon_create(parent, NULL, x, y);
    /* Keep layout unchanged even when resources have not been installed yet. */
    lv_obj_set_size(image, size, size);
    return image;
}

void weather_data_update(void) { ui_weather_process(); }

bool weather_time_update(void)
{
    const ui_weather_view_t *data = ui_weather_view();
    if (!current->weather_widgets.city) return true;
    bool load_icons = !current->weather_widgets.icons_loaded;
    if (load_icons || current->weather_widgets.code != data->code)
        ui_weather_icon_set(current->weather_widgets.icon, data->code, true);
    current->weather_widgets.code = data->code;
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        int code = data->forecast[i].code;
        if (load_icons || current->weather_widgets.forecast_code[i] != code)
            ui_weather_icon_set(current->weather_widgets.forecast_icon[i], code, false);
        current->weather_widgets.forecast_code[i] = code;
    }
    current->weather_widgets.icons_loaded = true;
    if (current->weather_widgets.revision == data->revision &&
        current->weather_widgets.locale_revision == data->locale_revision) return true;
    bool translated = text_update(current->weather_widgets.city, data->city);
    translated = text_update(current->weather_widgets.time, data->update_time) && translated;
    translated = text_update(current->weather_widgets.temperature, data->temperature) && translated;
    translated = text_update(current->weather_widgets.description, data->description) && translated;
    translated = text_update(current->weather_widgets.range, data->range_wind) && translated;
    translated = text_update(current->weather_widgets.source, data->source) && translated;
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
        translated = text_update(current->weather_widgets.metrics[i], data->metrics[i]) && translated;
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const ui_weather_forecast_t *f = &data->forecast[i];
        translated = text_update(current->weather_widgets.date[i], f->date) && translated;
        translated = text_update(current->weather_widgets.text[i], f->text) && translated;
        translated = text_update(current->weather_widgets.temperatures[i], f->temperature_range) && translated;
        translated = text_update(current->weather_widgets.wind[i], f->wind) && translated;
    }
    if (translated)
    {
        current->weather_widgets.revision = data->revision;
        current->weather_widgets.locale_revision = data->locale_revision;
    }
    weather_layout();
    return translated;
}

void weather_create(lv_obj_t *screen)
{
    weather_data_update();
    const ui_weather_view_t *data = ui_weather_view();
    current->weather_widgets.symbols[UI_WEATHER_METRIC_COUNT] =
        icon_create(screen, weather_symbol(UI_WEATHER_METRIC_COUNT), PAGE_TITLE_X, PAGE_TITLE_Y + 3);
    current->weather_widgets.city = label_create(screen, data->city, PAGE_TITLE_X + 44, PAGE_TITLE_Y, 272, ui_font_title());
    lv_obj_set_style_text_color(current->weather_widgets.city, lv_color_black(), 0);
    lv_obj_t *content = panel_create(screen, MARGIN, PAGE_CONTENT_Y,
                                    CONTENT_WIDTH, HEIGHT - PAGE_CONTENT_Y - MARGIN - 84);
    current->weather_widgets.content = content;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);
    current->weather_widgets.time = centered_label(content, data->update_time, 0, 0, CONTENT_WIDTH, ui_font_small());
    current->weather_widgets.icon = weather_image_create(content, 0, 0, 160);
    current->weather_widgets.temperature = centered_label(content, data->temperature, 0, 0, CONTENT_WIDTH, ui_font_temperature());
    current->weather_widgets.description = centered_label(content, data->description, 0, 0, CONTENT_WIDTH, ui_font_body());
    current->weather_widgets.range = centered_label(content, data->range_wind, 0, 0, CONTENT_WIDTH, ui_font_small());

    static const lv_ext_str_id_t metric_names[] = {
        LV_EXT_STR_ID(weather_humidity), LV_EXT_STR_ID(weather_wind_speed),
        LV_EXT_STR_ID(weather_visibility), LV_EXT_STR_ID(weather_cloud),
        LV_EXT_STR_ID(weather_sunrise), LV_EXT_STR_ID(weather_sunset),
        LV_EXT_STR_ID(weather_pressure), LV_EXT_STR_ID(weather_air)
    };
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
    {
        lv_obj_t *card = panel_create(content, 0, 0, 298, 80);
        current->weather_widgets.cards[i] = card;
        current->weather_widgets.symbols[i] = icon_create(card, weather_symbol(i), 16, 24);
        current->weather_widgets.captions[i] =
            weather_bind_text(label_create(card, "", 64, 9, 216, ui_font_caption()), metric_names[i]);
        current->weather_widgets.metrics[i] = label_create(card, data->metrics[i], 64, 39, 216, ui_font_body());
    }

    current->weather_widgets.forecast_title = weather_label(content, weather_forecast_title, 0, 0, CONTENT_WIDTH, ui_font_body());
    const int forecast_height = 64;
    lv_obj_t *forecasts = content;
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const ui_weather_forecast_t *forecast = &data->forecast[i];
        int y = i * forecast_height;
        int text_y = y + (forecast_height - ui_font_small()->line_height) / 2;
        current->weather_widgets.date[i] = label_create(forecasts, forecast->date, 0, text_y, 76, ui_font_small());
        current->weather_widgets.forecast_icon[i] = weather_image_create(forecasts, 80, y + 8, 48);
        current->weather_widgets.text[i] = label_create(forecasts, forecast->text, 138, text_y, 126, ui_font_small());
        current->weather_widgets.temperatures[i] = centered_label(forecasts, forecast->temperature_range, 272, text_y, 158, ui_font_small());
        lv_obj_t *wind = label_create(forecasts, forecast->wind, 442,
                                      y + (forecast_height - ui_font_caption()->line_height) / 2,
                                      178, ui_font_caption());
        lv_obj_set_style_text_align(wind, LV_TEXT_ALIGN_RIGHT, 0);
        current->weather_widgets.wind[i] = wind;
        current->weather_widgets.forecast_code[i] = forecast->code;
        lv_obj_t *line = panel_create(forecasts, 0, y + forecast_height - 1, CONTENT_WIDTH, 1);
        current->weather_widgets.lines[i] = line;
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_border_color(line, lv_color_hex(0x666666), 0);
    }
    current->weather_widgets.source = centered_label(content, data->source, 0, 0, CONTENT_WIDTH, ui_font_caption());
    current->weather_widgets.code = data->code;
    current->weather_widgets.revision = data->revision;
    current->weather_widgets.locale_revision = data->locale_revision;
    current->weather_widgets.refresh = weather_button(screen, MARGIN, HEIGHT - MARGIN - 70, CONTENT_WIDTH, 70, weather_refresh, WEATHER_SYNC);
}

static int weather_text_height(const char *text, const lv_font_t *font, int width)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    return size.y > font->line_height ? size.y : font->line_height;
}

static void weather_wrap(lv_obj_t *label, int x, int y, int width, int height, const lv_font_t *font)
{
    weather_label_layout(label, x, y, width, font);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, height);
}

void weather_layout(void)
{
    lv_area_t area;
    epd_ui_content_area(current->screen, &area);
    int width = area.x2 - area.x1 + 1, gap = epd_ui_px(6), pad = epd_ui_px(8);
    int body = ui_font_body()->line_height, small = ui_font_small()->line_height;
    int caption = ui_font_caption()->line_height, temperature = ui_font_temperature()->line_height;
    int button = epd_ui_touch(70), height = area.y2 - area.y1 + 1 - button - gap;
    weather_box(current->weather_widgets.content, area.x1, area.y1, width, height);
    weather_button_layout(current->weather_widgets.refresh, area.x1, area.y2 + 1 - button, width, button);
    int title_x = area.x1 + epd_ui_touch(56) + epd_ui_px(16);
    weather_box(current->weather_widgets.symbols[UI_WEATHER_METRIC_COUNT], title_x, epd_ui_px(PAGE_TITLE_Y + 3), 32, 32);
    weather_label_layout(current->weather_widgets.city, title_x + 32 + pad, epd_ui_px(PAGE_TITLE_Y),
                         area.x2 - title_x - 32 - pad + 1, ui_font_title());
    const ui_weather_view_t *data = ui_weather_view();
    int card_width = (width - gap) / 2, card_x = 2 * pad + 32;
    int metric_width = card_width - card_x - pad, metric_text = body;
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
    {
        int measured = weather_text_height(data->metrics[i], ui_font_body(), metric_width);
        if (measured > metric_text) metric_text = measured;
    }
    int metric_height = caption + metric_text + 2 * pad;
    int description = weather_text_height(data->description, ui_font_body(), width - 160 - gap);
    int range = weather_text_height(data->range_wind, ui_font_small(), width);
    int source = weather_text_height(data->source, ui_font_caption(), width);
    lv_point_t measured;
    lv_text_get_size(&measured, "00/00", ui_font_small(), 0, 0, width, LV_TEXT_FLAG_NONE);
    int date_width = measured.x + gap, text_x = date_width + 48 + gap;
    int text_width = (width - text_x - gap) / 2;
    int forecast_top[UI_WEATHER_FORECAST_COUNT], forecast_height[UI_WEATHER_FORECAST_COUNT];
    int forecasts = 0;
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        int text = weather_text_height(data->forecast[i].text, ui_font_small(), text_width);
        int temperatures = weather_text_height(data->forecast[i].temperature_range, ui_font_small(), width - text_x - text_width - gap);
        forecast_top[i] = text > temperatures ? text : temperatures;
        int wind = weather_text_height(data->forecast[i].wind, ui_font_caption(), width - text_x);
        forecast_height[i] = forecast_top[i] + wind + 2 * gap;
        if (forecast_height[i] < 52) forecast_height[i] = 52;
        forecasts += forecast_height[i];
    }
    int fixed = small + range + 4 * metric_height + body + forecasts + source + 9 * gap;
    int hero = height - fixed;
    int hero_min = temperature + description + gap;
    if (hero_min < 160) hero_min = 160;
    if (hero < hero_min) hero = hero_min;
    /* Overflow stays inside the content viewport; controls remain at its bottom. */
    lv_obj_add_flag(current->weather_widgets.content, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;
    weather_label_layout(current->weather_widgets.time, 0, y, width, ui_font_small());
    y += small + gap;
    /* Icons retain their resource dimensions. The temperature shares the row. */
    weather_box(current->weather_widgets.icon, 0, y + (hero - 160) / 2, 160, 160);
    weather_label_layout(current->weather_widgets.temperature, 160 + gap,
                         y + (hero - hero_min) / 2, width - 160 - gap, ui_font_temperature());
    weather_wrap(current->weather_widgets.description, 160 + gap,
                  y + (hero - hero_min) / 2 + temperature + gap, width - 160 - gap, description, ui_font_body());
    y += hero + gap;
    weather_wrap(current->weather_widgets.range, 0, y, width, range, ui_font_small());
    y += range + gap;
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
    {
        weather_box(current->weather_widgets.cards[i], (i % 2) * (card_width + gap),
                     y + (i / 2) * (metric_height + gap), card_width, metric_height);
        weather_box(current->weather_widgets.symbols[i], pad, (metric_height - 32) / 2, 32, 32);
        weather_label_layout(current->weather_widgets.captions[i], card_x, pad, metric_width, ui_font_caption());
        weather_wrap(current->weather_widgets.metrics[i], card_x, pad + caption, metric_width, metric_text, ui_font_body());
    }
    y += 4 * metric_height + 4 * gap;
    weather_label_layout(current->weather_widgets.forecast_title, 0, y, width, ui_font_body());
    y += body + gap;
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        int row = forecast_height[i], top = forecast_top[i];
        weather_label_layout(current->weather_widgets.date[i], 0, y + (row - small) / 2, date_width, ui_font_small());
        weather_box(current->weather_widgets.forecast_icon[i], date_width, y + (row - 48) / 2, 48, 48);
        weather_wrap(current->weather_widgets.text[i], text_x, y, text_width, top, ui_font_small());
        weather_wrap(current->weather_widgets.temperatures[i], text_x + text_width + gap, y,
                      width - text_x - text_width - gap, top, ui_font_small());
        weather_wrap(current->weather_widgets.wind[i], text_x, y + top + gap, width - text_x, row - top - 2 * gap, ui_font_caption());
        weather_box(current->weather_widgets.lines[i], 0, y + row - 1, width, 1);
        y += row;
    }
    weather_wrap(current->weather_widgets.source, 0, y + gap, width, source, ui_font_caption());
}

void weather_action_start(weather_job_t job)
{
    if (!current || !current->foreground || current->popup || current->weather_popup_phase) return;
    bool city_job = job == WEATHER_JOB_CITY || job == WEATHER_JOB_HISTORY_CITY;
    if (weather_request_job(job, city_job ? current->city_digits : NULL, &current->weather_ticket) != RT_EOK)
    {
        weather_popup_code(job == WEATHER_JOB_SYNC ? WEATHER_RESULT_SYNC_FAILED : WEATHER_RESULT_OPERATION_FAILED);
        return;
    }
    current->weather_job = job;
    current->weather_popup_tick = lv_tick_get();
    current->weather_popup_phase = 1;
    weather_ui_process();
}

void weather_ui_process(void)
{
    if (!current || !current->foreground) return;
    weather_data_update();
    weather_scale_update();
    weather_locale_update();
    if (!lv_refreshing_done()) return;
    if (!current->weather_popup_phase)
    {
        if (!current->popup) weather_time_update();
        return;
    }
    weather_result_t result;
    weather_get_result(&result);
    if (result.ticket == current->weather_ticket && result.busy)
    {
        if (!current->popup)
        {
            uint32_t elapsed = lv_tick_elaps(current->weather_popup_tick);
            if (elapsed >= 200) {
                weather_popup_code(WEATHER_RESULT_WORKING);
                lv_group_remove_all_objs(current->group);
                lv_obj_add_flag(current->popup_button, LV_OBJ_FLAG_HIDDEN);
            } else if (current->timer) {
                lv_timer_set_period(current->timer, 200 - elapsed);
                lv_timer_reset(current->timer);
                lv_timer_resume(current->timer);
            }
        }
        return;
    }
    if (result.ticket != current->weather_ticket)
    {
        memset(&result, 0, sizeof(result));
        result.code = WEATHER_RESULT_OPERATION_FAILED;
    }
    current->weather_popup_phase = 0;
    if (current->timer) lv_timer_pause(current->timer);
    if (result.success)
    {
        bool return_to_settings = current->weather_job == WEATHER_JOB_CITY ||
                                  current->weather_job == WEATHER_JOB_HISTORY_CITY;
        popup_close();
        if (return_to_settings)
        {
            settings_city_committed();
            weather_settings_return();
        }
        else if (launcher_current_page() == UI_PAGE_WEATHER_SETTINGS) weather_settings_render();
        else
        {
            weather_data_update();
            weather_time_update();
        }
    }
    else if (current->popup)
    {
        current->visible_result = result;
        weather_popup_retranslate();
        lv_obj_remove_flag(current->popup_button, LV_OBJ_FLAG_HIDDEN);
        lv_group_add_obj(current->group, current->popup_button);
        lv_group_focus_obj(current->popup_button);
    }
    else weather_popup_result(&result);
}
