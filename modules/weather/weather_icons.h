#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H
#include "lvgl.h"
#include <stdbool.h>

#define UI_WEATHER_ICON_COUNT 70
/* QWeather code lookup; large selects 160 px, otherwise 48 px. Unknown codes use 999. */
void ui_weather_icon_set(lv_obj_t *image, int code, bool large);
#endif
