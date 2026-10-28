#ifndef UI_SEPARATOR_H
#define UI_SEPARATOR_H

#include "lvgl.h"

enum { UI_SEPARATOR_THICKNESS = 3, UI_SEPARATOR_DASH = 6, UI_SEPARATOR_GAP = 6 };

/* Start with a complete dash at left; right is exclusive. Clipping leaves the
 * pattern anchored to the original left edge. */
static inline void ui_separator_draw(lv_layer_t *layer, int32_t left, int32_t right, int32_t y)
{
    const lv_area_t *clip = &layer->_clip_area;
    if (y + UI_SEPARATOR_THICKNESS / 2 < clip->y1 ||
        y - UI_SEPARATOR_THICKNESS / 2 > clip->y2 ||
        right <= clip->x1 || left > clip->x2) return;
    int32_t first = left;
    if (clip->x1 > left)
        first += (clip->x1 - left) / (UI_SEPARATOR_DASH + UI_SEPARATOR_GAP) *
                 (UI_SEPARATOR_DASH + UI_SEPARATOR_GAP);
    right = LV_MIN(right, clip->x2 + 1);
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = lv_color_black();
    fill.opa = LV_OPA_COVER;
    for (int32_t x = first; x < right; x += UI_SEPARATOR_DASH + UI_SEPARATOR_GAP)
    {
        lv_area_t dash = {x, y - UI_SEPARATOR_THICKNESS / 2,
            LV_MIN(x + UI_SEPARATOR_DASH, right) - 1, y + UI_SEPARATOR_THICKNESS / 2};
        if (dash.x2 >= clip->x1) lv_draw_fill(layer, &fill, &dash);
    }
}

#endif
