#ifndef UI_WIFI_ICONS_H
#define UI_WIFI_ICONS_H

#include "lvgl.h"
#include "ui_model.h"

enum { UI_WIFI_ICON_LOCKED = 1 << 2 };

int32_t ui_wifi_icon_width(int32_t height);
void ui_wifi_icon_draw(lv_layer_t *layer, const lv_area_t *area, uintptr_t state);

#endif
