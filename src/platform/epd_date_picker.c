#include "epd_date_picker.h"
#include "platform/epd_app.h"
#include "platform/epd_input.h"
#include "platform/epd_locale.h"
#include <stdio.h>
#include <string.h>
#include <rtm.h>

typedef struct
{
    lv_obj_t *root, *grid, *month, *previous, *next, *year_month;
    lv_obj_t *cells[42], *numbers[42], *wheels[3];
    lv_obj_t *arrows[3];
    lv_point_precise_t points[3][3];
    epd_date_picker_value_t value;
    epd_date_picker_mode_t mode;
    int shown_year, shown_month, width;
    bool choosing_month, updating;
} date_picker_t;

static int dp_px(int n) { return epd_ui_px(n); }
static bool dp_english(void) { return !strcmp(epd_locale_get(), "en_us"); }
static void dp_event(lv_event_t *event);
static lv_obj_t *dp_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    epd_app_object_init(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}
static lv_obj_t *dp_label(lv_obj_t *parent, const char *text, int y, int w,
                          epd_font_role_t role, unsigned color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, w);
    lv_obj_set_y(label, y);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, epd_app_font_role(role), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}
static lv_obj_t *dp_button(date_picker_t *p, lv_obj_t *parent, const char *text,
                           int x, int y, int w, int h)
{
    lv_obj_t *button = dp_box(parent, x, y, w, h);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, dp_px(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -dp_px(4), LV_STATE_FOCUS_KEY);
    epd_input_add_object(button);
    lv_obj_set_style_radius(button, dp_px(16), 0);
    lv_obj_t *label = dp_label(button, text, 0, w, EPD_FONT_BODY, 0x222222);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, dp_event, LV_EVENT_CLICKED, p);
    return button;
}
static lv_obj_t *dp_wheel(date_picker_t *p, lv_obj_t *parent, int first, int last,
                          int x, int y, int w, bool loop)
{
    char options[1600] = {0}; size_t used = 0;
    for (int i = first; i <= last; ++i)
        used += snprintf(options + used, sizeof(options) - used, "%s%02d", i == first ? "" : "\n", i);
    lv_obj_t *wheel = lv_roller_create(parent);
    lv_obj_set_pos(wheel, x, y);
    lv_obj_set_width(wheel, w);
    lv_obj_set_style_border_width(wheel, 0, 0);
    lv_obj_set_style_radius(wheel, 0, 0);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_TRANSP, LV_PART_SELECTED);
    lv_obj_set_style_text_align(wheel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wheel, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_style_text_font(wheel, epd_app_font_role(EPD_FONT_TITLE), LV_PART_SELECTED);
    lv_obj_set_style_text_color(wheel, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(wheel, lv_color_hex(0x111111), LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(wheel, dp_px(34), 0);
    lv_obj_set_style_anim_duration(wheel, 0, 0);
    lv_obj_remove_flag(wheel, LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(wheel, options, loop ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(wheel, 3);
    epd_input_add_object(wheel);
    lv_obj_add_event_cb(wheel, dp_event, LV_EVENT_VALUE_CHANGED, p);
    return wheel;
}
static void dp_arrow(date_picker_t *p, int index, lv_obj_t *parent, int x, int y)
{
    int points[3][6] = {{20,4,8,16,20,28},{8,4,20,16,8,28},{5,11,16,22,27,11}};
    for (int i = 0; i < 3; ++i)
        p->points[index][i] = (lv_point_precise_t){dp_px(points[index][i*2]), dp_px(points[index][i*2+1])};
    p->arrows[index] = lv_line_create(parent);
    lv_obj_set_pos(p->arrows[index], x, y);
    lv_line_set_points(p->arrows[index], p->points[index], 3);
    lv_obj_set_style_line_color(p->arrows[index], lv_color_hex(0x777777), 0);
    lv_obj_set_style_line_width(p->arrows[index], dp_px(2), 0);
    lv_obj_set_style_line_rounded(p->arrows[index], true, 0);
}
static void dp_grid_update(date_picker_t *p)
{
    char caption[40];
    snprintf(caption, sizeof(caption), "%04d / %02d", p->shown_year, p->shown_month);
    lv_label_set_text(lv_obj_get_child(p->month, 0), caption);
    for (int i = 0; i < 3; ++i)
        p->points[2][i].y = dp_px(p->choosing_month ? (i == 1 ? 9 : 20) : (i == 1 ? 22 : 11));
    lv_obj_invalidate(p->arrows[2]);
    bool minimum = p->shown_year == p->value.first_year && p->shown_month == 1;
    bool maximum = p->shown_year == p->value.last_year && p->shown_month == 12;
    lv_obj_set_state(p->previous, LV_STATE_DISABLED, minimum);
    lv_obj_set_state(p->next, LV_STATE_DISABLED, maximum);
    int offset = (epd_date_weekday(epd_date_day((epd_date_t){p->shown_year, p->shown_month, 1})) +
                  7 - p->value.first_weekday) % 7;
    int count = epd_date_month_days(p->shown_year, p->shown_month);
    for (int i = 0; i < 42; ++i)
    {
        int day = i - offset + 1;
        bool valid = day > 0 && day <= count;
        lv_obj_set_flag(p->cells[i], LV_OBJ_FLAG_HIDDEN, !valid);
        if (!valid) continue;
        char number[12]; snprintf(number, sizeof(number), "%d", day);
        lv_label_set_text(p->numbers[i], number);
        bool selected = p->value.date.year == p->shown_year && p->value.date.month == p->shown_month && p->value.date.day == day;
        lv_obj_set_style_bg_color(p->cells[i], lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(p->cells[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(p->numbers[i], lv_color_hex(selected ? 0xffffff : 0x222222), 0);
    }
}
static void dp_month_toggle(date_picker_t *p)
{
    p->choosing_month = !p->choosing_month;
    lv_obj_set_flag(p->grid, LV_OBJ_FLAG_HIDDEN, p->choosing_month);
    if (p->choosing_month)
    {
        p->year_month = dp_box(p->root, 0, dp_px(96), p->width, dp_px(360));
        int w = (p->width - dp_px(80)) / 2;
        dp_label(p->year_month, dp_english() ? "Year" : "年", 0, w, EPD_FONT_SMALL, 0x888888);
        lv_obj_set_x(lv_obj_get_child(p->year_month, 0), dp_px(24));
        lv_obj_t *label = dp_label(p->year_month, dp_english() ? "Month" : "月", 0, w, EPD_FONT_SMALL, 0x888888);
        lv_obj_set_x(label, p->width - w - dp_px(24));
        p->wheels[0] = dp_wheel(p, p->year_month, p->value.first_year, p->value.last_year, dp_px(24), dp_px(54), w, false);
        p->wheels[1] = dp_wheel(p, p->year_month, 1, 12, p->width - w - dp_px(24), dp_px(54), w, true);
        lv_roller_set_selected(p->wheels[0], p->shown_year - p->value.first_year, LV_ANIM_OFF);
        lv_roller_set_selected(p->wheels[1], p->shown_month - 1, LV_ANIM_OFF);
    }
    else
    {
        lv_obj_delete(p->year_month); p->year_month = NULL;
        memset(p->wheels, 0, sizeof(p->wheels));
    }
    dp_grid_update(p);
}
static void dp_event(lv_event_t *event)
{
    date_picker_t *p = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    if (p->updating) return;
    if (p->mode == EPD_PICK_TIME)
    {
        unsigned hour = lv_roller_get_selected(p->wheels[0]);
        if (!p->value.hour24) hour = (hour + 1) % 12 + 12 * lv_roller_get_selected(p->wheels[2]);
        p->value.minute = hour * 60 + lv_roller_get_selected(p->wheels[1]);
        lv_obj_send_event(p->root, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
    if (target == p->month) { dp_month_toggle(p); return; }
    if (target == p->previous || target == p->next)
    {
        int month = p->shown_year * 12 + p->shown_month - 1 + (target == p->next ? 1 : -1);
        if (month < p->value.first_year * 12 || month > p->value.last_year * 12 + 11) return;
        p->shown_year = month / 12; p->shown_month = month % 12 + 1;
        if (p->choosing_month)
        {
            lv_roller_set_selected(p->wheels[0], p->shown_year - p->value.first_year, LV_ANIM_OFF);
            lv_roller_set_selected(p->wheels[1], p->shown_month - 1, LV_ANIM_OFF);
        }
        dp_grid_update(p); return;
    }
    if (p->choosing_month)
    {
        p->shown_year = p->value.first_year + lv_roller_get_selected(p->wheels[0]);
        p->shown_month = 1 + lv_roller_get_selected(p->wheels[1]);
        dp_grid_update(p); return;
    }
    int offset = (epd_date_weekday(epd_date_day((epd_date_t){p->shown_year, p->shown_month, 1})) +
                  7 - p->value.first_weekday) % 7;
    for (int i = 0; i < 42; ++i) if (target == p->cells[i])
    {
        epd_date_t selected = {p->shown_year, p->shown_month, i - offset + 1};
        if (!epd_date_valid(selected)) return;
        p->value.date = selected;
        dp_grid_update(p);
        lv_obj_send_event(p->root, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
}
static void dp_delete(lv_event_t *event) { lv_free(lv_event_get_user_data(event)); }
static epd_date_picker_value_t dp_normalize(epd_date_picker_value_t v)
{
    v.first_year = LV_CLAMP(1901, v.first_year, 2099);
    v.last_year = LV_CLAMP(v.first_year, v.last_year, 2099);
    if (!epd_date_valid(v.date)) v.date = (epd_date_t){v.first_year, 1, 1};
    v.date.year = LV_CLAMP(v.first_year, v.date.year, v.last_year);
    v.date.day = LV_MIN(v.date.day, epd_date_month_days(v.date.year, v.date.month));
    v.minute = LV_MIN(v.minute, 1439); v.first_weekday %= 7;
    return v;
}
int32_t epd_date_picker_height(epd_date_picker_mode_t mode) { return dp_px(mode == EPD_PICK_DATE ? 500 : 300); }
lv_obj_t *epd_date_picker_create(lv_obj_t *parent, epd_date_picker_mode_t mode,
                                const epd_date_picker_value_t *value, int32_t width)
{
    if (!value) return NULL;
    date_picker_t *p = lv_malloc_zeroed(sizeof(*p)); if (!p) return NULL;
    p->value = dp_normalize(*value); p->mode = mode; p->width = width;
    p->shown_year = p->value.date.year; p->shown_month = p->value.date.month;
    p->root = dp_box(parent, 0, 0, width, epd_date_picker_height(mode));
    lv_obj_set_user_data(p->root, p);
    lv_obj_add_event_cb(p->root, dp_delete, LV_EVENT_DELETE, p);
    if (mode == EPD_PICK_DATE)
    {
        p->previous = dp_button(p, p->root, "", dp_px(8), dp_px(8), dp_px(56), dp_px(56));
        p->month = dp_button(p, p->root, "", dp_px(70), dp_px(8), width - dp_px(140), dp_px(56));
        p->next = dp_button(p, p->root, "", width - dp_px(64), dp_px(8), dp_px(56), dp_px(56));
        dp_arrow(p, 0, p->previous, dp_px(12), dp_px(12));
        dp_arrow(p, 1, p->next, dp_px(12), dp_px(12));
        dp_arrow(p, 2, p->month, (width - dp_px(140)) / 2 + dp_px(88), dp_px(12));
        p->grid = dp_box(p->root, 0, dp_px(80), width, dp_px(420));
        int cell = width / 7;
        const char *zh[] = {"日", "一", "二", "三", "四", "五", "六"};
        const char *en[] = {"S", "M", "T", "W", "T", "F", "S"};
        for (int i = 0; i < 7; ++i)
        {
            int day = (i + p->value.first_weekday) % 7;
            lv_obj_t *label = dp_label(p->grid, dp_english() ? en[day] : zh[day], 0, cell, EPD_FONT_SMALL, 0x888888);
            lv_obj_set_x(label, i * cell);
        }
        for (int i = 0; i < 42; ++i)
        {
            p->cells[i] = dp_button(p, p->grid, "", i % 7 * cell + dp_px(4), dp_px(46 + i / 7 * 60), cell - dp_px(8), dp_px(54));
            p->numbers[i] = lv_obj_get_child(p->cells[i], 0);
        }
        dp_grid_update(p);
    }
    else
    {
        int columns = p->value.hour24 ? 2 : 3;
        int available = LV_MIN(width - dp_px(32), dp_px(columns == 2 ? 340 : 500));
        int cell = available / columns, start = (width - available) / 2;
        const char *labels[] = {dp_english() ? "Hour" : "时", dp_english() ? "Minute" : "分", ""};
        for (int i = 0; i < columns; ++i)
        {
            lv_obj_t *label = dp_label(p->root, labels[i], dp_px(12), cell, EPD_FONT_SMALL, 0x999999);
            lv_obj_set_x(label, start + i * cell);
            p->wheels[i] = dp_wheel(p, p->root, i == 0 && !p->value.hour24 ? 1 : 0,
                                     i == 0 ? (p->value.hour24 ? 23 : 12) : i == 1 ? 59 : 1,
                                     start + i * cell, dp_px(64), cell, i != 2);
        }
        if (columns == 3) lv_roller_set_options(p->wheels[2], dp_english() ? "AM\nPM" : "上午\n下午", LV_ROLLER_MODE_NORMAL);
        epd_date_picker_set(p->root, &p->value);
    }
    return p->root;
}
void epd_date_picker_get(lv_obj_t *picker, epd_date_picker_value_t *value)
{
    if (picker && value) { date_picker_t *p = lv_obj_get_user_data(picker); *value = p->value; }
}
void epd_date_picker_set(lv_obj_t *picker, const epd_date_picker_value_t *value)
{
    if (!picker || !value) return;
    date_picker_t *p = lv_obj_get_user_data(picker);
    epd_date_picker_value_t v = *value;
    v.first_year = p->value.first_year; v.last_year = p->value.last_year;
    v.first_weekday = p->value.first_weekday; v.hour24 = p->value.hour24;
    p->value = dp_normalize(v); p->updating = true;
    if (p->mode == EPD_PICK_DATE)
    {
        if (p->choosing_month) dp_month_toggle(p);
        p->shown_year = p->value.date.year; p->shown_month = p->value.date.month;
        dp_grid_update(p);
    }
    else
    {
        int hour = p->value.minute / 60;
        lv_roller_set_selected(p->wheels[0], p->value.hour24 ? hour : (hour + 11) % 12, LV_ANIM_OFF);
        lv_roller_set_selected(p->wheels[1], p->value.minute % 60, LV_ANIM_OFF);
        if (p->wheels[2]) lv_roller_set_selected(p->wheels[2], hour / 12, LV_ANIM_OFF);
    }
    p->updating = false;
}
RTM_EXPORT(epd_date_picker_create);
RTM_EXPORT(epd_date_picker_get);
RTM_EXPORT(epd_date_picker_set);
RTM_EXPORT(epd_date_picker_height);
