#include "ui_wifi_icons.h"

enum { WIFI_GLYPH_WIDTH = 19, WIFI_GLYPH_HEIGHT = 14 };

int32_t ui_wifi_icon_width(int32_t height)
{
    return (height * WIFI_GLYPH_WIDTH + WIFI_GLYPH_HEIGHT / 2) / WIFI_GLYPH_HEIGHT;
}

void ui_wifi_icon_draw(lv_layer_t *layer, const lv_area_t *area, uintptr_t state)
{
    int32_t height = LV_MIN(lv_area_get_height(area),
        (lv_area_get_width(area) * WIFI_GLYPH_HEIGHT + WIFI_GLYPH_WIDTH / 2) / WIFI_GLYPH_WIDTH);
    if (height < WIFI_GLYPH_HEIGHT) return;

    unsigned signal = state & (UI_WIFI_ICON_LOCKED - 1);
    int32_t cx = (area->x1 + area->x2) / 2;
    int32_t y = area->y1 + (lv_area_get_height(area) - height) / 2;
    int32_t cy = y + height - 1 - height / WIFI_GLYPH_HEIGHT;
    lv_color_t black = lv_color_black();
    lv_color_t gray = lv_color_hex(0xaaaaaa);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center = (lv_point_t){cx, cy};
    arc.start_angle = 225;
    arc.end_angle = 315;
    arc.rounded = true;
    arc.width = (height + 3) / 7;
    for (unsigned i = 0; i < 3; ++i)
    {
        arc.radius = (height * (5 + 4 * i) + 7) / WIFI_GLYPH_HEIGHT;
        arc.color = i < signal ? black : gray;
        lv_draw_arc(layer, &arc);
    }

    int32_t dot = arc.width;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = LV_RADIUS_CIRCLE;
    rect.bg_color = signal ? black : gray;
    lv_area_t dot_area = {cx - dot / 2, cy - dot / 2, cx + (dot - 1) / 2, cy + (dot - 1) / 2};
    lv_draw_rect(layer, &rect, &dot_area);

    if (!(state & UI_WIFI_ICON_LOCKED)) return;

    int32_t lock_x = cx + (height + 1) / 2;
    int32_t body_width = (3 * height + 5) / 10;
    int32_t body_height = (height + 2) / 5;
    int32_t body_top = cy + height / WIFI_GLYPH_HEIGHT - body_height + 1;
    int32_t x1 = lock_x - body_width / 2;
    lv_area_t lock = {x1, body_top, x1 + body_width - 1, body_top + body_height - 1};

    arc.color = black;
    arc.center = (lv_point_t){lock_x, body_top};
    arc.start_angle = 180;
    arc.end_angle = 360;
    arc.rounded = false;
    arc.radius = (9 * height + 35) / 70;
    arc.width = (height + 8) / 16;
    lv_draw_arc(layer, &arc);

    rect.bg_color = black;
    rect.radius = (height + 12) / 26;
    lv_draw_rect(layer, &rect, &lock);
}
