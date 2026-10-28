#include "weather_ui.h"

static char city_draft[WEATHER_CITY_ID_MAX];
static bool city_draft_valid;
void settings_city_committed(void)
{
    city_draft_valid = false;
    current->city_committed = true;
}

void city_input_pause(void)
{
    if (current->city_committed) return;

    snprintf(city_draft, sizeof(city_draft), "%s", current->city_digits);
    city_draft_valid = true;
}
bool city_action(int target)
{
    if (launcher_current_page() == UI_PAGE_CITY_INPUT)
    {
        size_t length = strlen(current->city_digits);
        if (target >= CITY_DIGIT_BASE && target < CITY_DIGIT_BASE + 10)
        {
            if (length + 1 < sizeof(current->city_digits))
            {
                current->city_digits[length] = '0' + target - CITY_DIGIT_BASE;
                current->city_digits[length + 1] = '\0';
                text_update(current->city_input, current->city_digits);
            }
            return true;
        }
        if (target == CITY_ERASE)
        {
            if (length) current->city_digits[length - 1] = '\0';
            text_update(current->city_input, current->city_digits);
            return true;
        }
        if (target == CITY_CONFIRM)
        {
            if (!length) weather_popup_code(WEATHER_RESULT_CITY_ID_REQUIRED);
            else weather_action_start(WEATHER_JOB_CITY);
            return true;
        }
    }

    return false;
}

void city_create(lv_obj_t *screen)
{
    weather_title(screen, weather_city_title, ui_font_title());
    lv_obj_t *quiet = panel_create(screen, (WIDTH - 348) / 2, 296, 348, 348);
    current->form[0] = quiet;
    lv_obj_set_style_border_width(quiet, 0, 0);
    lv_obj_set_style_radius(quiet, 0, 0);
    lv_obj_t *qr = lv_qrcode_create(quiet);
    epd_obj_init(qr);
    lv_qrcode_set_size(qr, 300);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_center(qr);
    if (lv_qrcode_update(qr, WEATHER_CITY_LOOKUP_URL, strlen(WEATHER_CITY_LOOKUP_URL)) != LV_RESULT_OK)
        current->form[3] = weather_centered(screen, weather_qr_failed, MARGIN, 420, CONTENT_WIDTH, ui_font_body());
    /* Make the indexed canvas visible to EPIC before it is drawn. */
    epd_app_clean_draw_buffer(lv_canvas_get_draw_buf(qr));
    current->form[1] = weather_centered(screen, weather_scan_city, MARGIN, 696, CONTENT_WIDTH, ui_font_body());
    current->form[2] = weather_button(screen, 112, 800, 460, 80, weather_have_city_id, UI_PAGE_CITY_INPUT);
}

void city_input_create(lv_obj_t *screen)
{
    weather_config_t cfg;
    weather_get_config(&cfg);
    snprintf(current->city_digits, sizeof(current->city_digits), "%s",
             city_draft_valid ? city_draft : cfg.city_id);
    weather_title(screen, weather_city_title, ui_font_title());
    current->form[0] = weather_centered(screen, weather_city_id_required, MARGIN, 238, CONTENT_WIDTH, ui_font_body());
    lv_obj_t *input = panel_create(screen, 64, 302, 556, 88);
    current->form[1] = input;
    current->city_input = centered_label(input, current->city_digits, 16,
        (88 - ui_font_title()->line_height) / 2, 524, ui_font_title());
    /* A static label avoids the textarea cursor's periodic blinking on e-paper. */
    lv_obj_t *keys = panel_create(screen, MARGIN, 724, CONTENT_WIDTH, 436);
    current->form[2] = keys;
    static const char *const labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "", ""};
    for (unsigned i = 0; i < 12; ++i)
    {
        int target = i < 9 ? CITY_DIGIT_BASE + (int)i + 1 :
                     i == 9 ? CITY_DIGIT_BASE : i == 10 ? CITY_CONFIRM : CITY_ERASE;
        lv_obj_t *button = button_create(keys, 18 + (i % 3) * 198, 18 + (i / 3) * 102, 186, 88, labels[i], target);
        current->form[3 + i] = button;
        if (i >= 10) weather_button_text(button, i == 10 ? LV_EXT_STR_ID(weather_confirm) : LV_EXT_STR_ID(weather_erase));
    }
}

bool weather_settings_render(void)
{
    weather_config_t cfg;
    char text[128];
    weather_get_config(&cfg);
    EPD_FMT_weather_config_status(text, sizeof(text),
             cfg.api_key[0] ? epd_get_str(weather_imported) : epd_get_str(weather_not_imported),
             cfg.city[0] ? cfg.city : epd_get_str(weather_not_set));
    bool translated = text_update(current->weather_config_label, text);
    if (!translated) current->locale_retry = true;
    return translated;
}

void weather_settings_create(lv_obj_t *screen)
{
    weather_title(screen, weather_settings_title, ui_font_title());
    current->weather_config_label = centered_label(screen, "", MARGIN, 246, CONTENT_WIDTH, ui_font_small());
    lv_label_set_long_mode(current->weather_config_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->weather_config_label, 80);
    weather_settings_render();
    current->form[0] = weather_button(screen, MARGIN, 348, CONTENT_WIDTH, 84, weather_import_config, WEATHER_IMPORT);
    current->form[1] = weather_button(screen, MARGIN, 464, CONTENT_WIDTH, 84, weather_city_title, UI_PAGE_CITY);
    current->form[2] = weather_button(screen, MARGIN, 580, CONTENT_WIDTH, 84, weather_history_title, UI_PAGE_CITY_HISTORY);
    current->form[3] = weather_centered(screen, weather_config_file, MARGIN, 724, CONTENT_WIDTH, ui_font_small());
}

void weather_settings_layout(void)
{
    lv_area_t area;
    epd_ui_content_area(current->screen, &area);
    int width = area.x2 - area.x1 + 1, height = area.y2 - area.y1 + 1;
    int gap = epd_ui_px(20), body = ui_font_body()->line_height;
    if (current->id == UI_PAGE_WEATHER_SETTINGS)
    {
        int y = area.y1 + gap, label_height = 3 * ui_font_small()->line_height;
        weather_label_layout(current->weather_config_label, area.x1, y, width, ui_font_small());
        lv_label_set_long_mode(current->weather_config_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(current->weather_config_label, label_height);
        y += label_height + gap;
        for (unsigned i = 0; i < 3; ++i)
        {
            weather_button_layout(current->form[i], area.x1, y, width, epd_ui_touch(84));
            y += epd_ui_touch(84) + gap;
        }
        weather_label_layout(current->form[3], area.x1, y, width, ui_font_small());
    }
    else if (current->id == UI_PAGE_CITY)
    {
        int qr = 348, button = epd_ui_touch(80);
        int y = area.y1 + (height - qr - body - button - 2 * gap) / 2;
        weather_box(current->form[0], area.x1 + (width - qr) / 2, y, qr, qr);
        weather_label_layout(current->form[3], area.x1, y + qr / 2, width, ui_font_body());
        weather_label_layout(current->form[1], area.x1, y + qr + gap, width, ui_font_body());
        weather_button_layout(current->form[2], area.x1, y + qr + body + 2 * gap, width, button);
    }
    else if (current->id == UI_PAGE_CITY_INPUT)
    {
        int pad = epd_ui_px(16), key_gap = epd_ui_px(12), key_height = epd_ui_touch(88);
        int keys_height = 2 * pad + 4 * key_height + 3 * key_gap;
        weather_label_layout(current->form[0], area.x1, area.y1 + gap, width, ui_font_body());
        int input_height = ui_font_title()->line_height + 2 * pad;
        weather_box(current->form[1], area.x1, area.y1 + body + 2 * gap, width, input_height);
        weather_label_layout(current->city_input, pad, pad, width - 2 * pad, ui_font_title());
        weather_box(current->form[2], area.x1, area.y2 + 1 - keys_height, width, keys_height);
        int key_width = (width - 2 * pad - 2 * key_gap) / 3;
        for (unsigned i = 0; i < 12; ++i)
            weather_button_layout(current->form[3 + i], pad + (i % 3) * (key_width + key_gap),
                                   pad + (i / 3) * (key_height + key_gap), key_width, key_height);
    }
}
