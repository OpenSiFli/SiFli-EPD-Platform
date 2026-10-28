#include "ui_settings_internal.h"
#include "ui_internal.h"
#include "ui_shell.h"
#include "ui_scale.h"
#include "storage.h"

#define SCALE_CHOICE_BASE (-8100)
#define SCALE_APPLY (-8110)

typedef struct {
    lv_obj_t *title, *choices[5], *preview[3], *apply;
    unsigned selected;
} scale_page_t;

static scale_page_t *scale_page(void)
{ return current && current->id == UI_PAGE_UI_SCALE ? current->feature : NULL; }

static bool scale_page_layout(scale_page_t *view)
{
    static const epd_font_role_t roles[] = {EPD_FONT_TITLE, EPD_FONT_BODY, EPD_FONT_SMALL};
    const lv_font_t *fonts[3];
    for (unsigned i = 0; i < 3; ++i)
    {
        fonts[i] = epd_ui_font_at(view->selected, roles[i]);
        if (!fonts[i]) return false;
    }
    lv_obj_update_layout(current->screen);
    int32_t width = lv_obj_get_width(current->screen) - 2 * MARGIN;
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32);
    int32_t row = epd_ui_touch(64), gap = epd_ui_px(8);
    if (lv_obj_get_style_text_font(view->title, 0) != ui_font_settings_title())
        lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, lv_obj_get_width(current->screen) - SETTINGS_TITLE_X - MARGIN),
                    ui_font_settings_title()->line_height);
    for (unsigned i = 0; i < 5; ++i)
    {
        lv_obj_t *button = view->choices[i];
        lv_obj_set_pos(button, MARGIN, top + i * (row + gap));
        lv_obj_set_size(button, width, row);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (lv_obj_get_style_text_font(label, 0) != ui_font_settings_body())
            lv_obj_set_style_text_font(label, ui_font_settings_body(), 0);
        lv_obj_set_size(label, width - epd_ui_px(80), ui_font_settings_body()->line_height);
    }
    int32_t y = top + 5 * (row + gap) + epd_ui_px(20);
    for (unsigned i = 0; i < 3; ++i)
    {
        const lv_font_t *font = fonts[i];
        if (lv_obj_get_style_text_font(view->preview[i], 0) != font)
            lv_obj_set_style_text_font(view->preview[i], font, 0);
        lv_obj_set_pos(view->preview[i], MARGIN, y);
        lv_obj_set_size(view->preview[i], width, font->line_height);
        y += font->line_height + epd_ui_px(16);
    }
    int32_t height = epd_ui_touch(64);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    lv_obj_set_pos(view->apply, MARGIN, bottom - height - epd_ui_px(16));
    lv_obj_set_size(view->apply, width, height);
    lv_obj_t *label = lv_obj_get_child(view->apply, 0);
    if (lv_obj_get_style_text_font(label, 0) != ui_font_settings_body())
        lv_obj_set_style_text_font(label, ui_font_settings_body(), 0);
    lv_obj_set_size(label, width - epd_ui_px(24), ui_font_settings_body()->line_height);
    return true;
}

void scale_page_process(void)
{
    scale_page_t *view = scale_page();
    if (!view || !current->foreground) return;
    if (!scale_page_layout(view)) { current->locale_retry = true; return; }
    for (unsigned i = 0; i < 5; ++i)
    {
        lv_obj_t *button = view->choices[i];
        char text[80];
        snprintf(text, sizeof(text), "%s  %u%%", ui_settings_option(UI_SETTING_UI_SCALE, i), ui_scale_percent_at(i));
        text_update(lv_obj_get_child(button, 0), text);
        int32_t border = view->selected == i ? 3 : 1;
        if (lv_obj_get_style_border_width(button, 0) != border)
            lv_obj_set_style_border_width(button, border, 0);
    }
    const char *texts[] = {epd_get_str(settings_scale_preview), epd_get_str(settings_scale_example),
                          epd_get_str(settings_time)};
    for (unsigned i = 0; i < 3; ++i) text_update(view->preview[i], texts[i]);
}

bool scale_page_action(int *target)
{
    scale_page_t *view = scale_page();
    if (!view || current->popup || storage_changing()) return false;
    if (*target >= SCALE_CHOICE_BASE && *target < SCALE_CHOICE_BASE + 5)
    {
        unsigned next = (unsigned)(*target - SCALE_CHOICE_BASE);
        if (ui_scale_prepare(next)) view->selected = next;
        else popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
        scale_page_process();
        return true;
    }
    if (*target != SCALE_APPLY) return false;
    if (!ui_settings_set(UI_SETTING_UI_SCALE, view->selected))
        popup_open_i18n(LV_EXT_STR_ID(platform_setting_failed), NULL);
    else page_retranslate();
    return true;
}

void scale_page_create(lv_obj_t *screen)
{
    scale_page_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    lv_obj_t *title = page_title_create(screen, LV_EXT_STR_ID(settings_config_scale), ui_font_settings_title());
    if (!view)
    { centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, PAGE_CONTENT_Y, CONTENT_WIDTH, ui_font_settings_body()); return; }
    view->title = title;
    view->selected = ui_settings_index(UI_SETTING_UI_SCALE);
    for (unsigned i = 0; i < 5; ++i)
        view->choices[i] = button_create(screen, MARGIN, PAGE_CONTENT_Y, CONTENT_WIDTH, epd_ui_touch(64), "", SCALE_CHOICE_BASE + (int)i);
    for (unsigned i = 0; i < 3; ++i)
        view->preview[i] = label_create(screen, "", MARGIN, PAGE_CONTENT_Y, CONTENT_WIDTH, ui_font_settings_body());
    view->apply = button_create_i18n(screen, MARGIN, PAGE_CONTENT_Y, CONTENT_WIDTH, epd_ui_touch(64),
                                    LV_EXT_STR_ID(platform_ok), SCALE_APPLY);
}

void scale_page_stop(void)
{ lv_free(current->feature); current->feature = NULL; }
