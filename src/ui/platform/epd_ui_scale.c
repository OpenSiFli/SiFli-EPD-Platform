#include "epd_ui_scale.h"
#include "ui_scale.h"
#include "ui_font.h"
#include "ui_shell.h"
#include <limits.h>
#include <rtm.h>

static const unsigned percentages[] = {90, 95, 100, 110, 120};
static unsigned selected_scale = 2;
static uint32_t scale_revision = 1;

unsigned ui_scale_percent_at(unsigned index)
{
    return index < 5 ? percentages[index] : 100;
}

unsigned epd_ui_scale_percent(void) { return percentages[selected_scale]; }
uint32_t epd_ui_scale_revision(void) { return scale_revision; }

int32_t epd_ui_px(int32_t base)
{
    int64_t value = (int64_t)base * epd_ui_scale_percent();
    value = (value + (value < 0 ? -50 : 50)) / 100;
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : (int32_t)value;
}

int32_t epd_ui_touch(int32_t base) { return LV_MAX(56, epd_ui_px(base)); }

void epd_ui_content_area(lv_obj_t *screen, lv_area_t *area)
{
    if (!area) return;
    if (!screen) screen = lv_screen_active();
    int32_t width = lv_obj_get_width(screen), height = lv_obj_get_height(screen);
    int32_t bottom = screen == lv_screen_active() ? ui_shell_content_bottom() : height - epd_ui_px(84);
    area->x1 = epd_ui_px(32);
    area->y1 = epd_ui_px(174);
    area->x2 = LV_MAX(area->x1, width - epd_ui_px(32) - 1);
    area->y2 = LV_MAX(area->y1, bottom - 1);
}

bool ui_scale_prepare(unsigned index) { return index < 5 && ui_font_prepare_scale(index); }

void ui_scale_apply(unsigned index)
{
    if (index >= 5 || index == selected_scale) return;
    lv_draw_wait_for_finish();
    ui_font_select_scale(index);
    selected_scale = index;
    ++scale_revision;
}

const lv_font_t *epd_ui_font_at(unsigned index, epd_font_role_t role)
{
    static const unsigned roles[] = {1, 0, 2, 3, 4};
    if (index >= 5 || (unsigned)role >= 5 || !ui_scale_prepare(index)) return NULL;
    return ui_font_at(index, roles[role]);
}

RTM_EXPORT(epd_ui_scale_percent);
RTM_EXPORT(epd_ui_scale_revision);
RTM_EXPORT(epd_ui_px);
RTM_EXPORT(epd_ui_touch);
RTM_EXPORT(epd_ui_content_area);
RTM_EXPORT(epd_ui_font_at);
