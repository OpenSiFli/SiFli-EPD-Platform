#ifndef UI_WEATHER_DATA_H
#define UI_WEATHER_DATA_H
#include <stdbool.h>
#include <stdint.h>
#include "weather.h"

#define UI_WEATHER_METRIC_COUNT 8
#define UI_WEATHER_FORECAST_COUNT 3

typedef struct
{
    int code;
    char date[16], text[32], temperature_range[32], wind[64];
} ui_weather_forecast_t;

typedef struct
{
    uint32_t revision, locale_revision;
    int code;
    char city[48], source[96], update_time[128], summary[96];
    char temperature[24], description[96], range_wind[128];
    char metrics[UI_WEATHER_METRIC_COUNT][64];
    ui_weather_forecast_t forecast[UI_WEATHER_FORECAST_COUNT];
} ui_weather_view_t;

/* UI-thread-only presentation layer. It copies, then formats a locked snapshot. */
bool ui_weather_process(void);
const ui_weather_view_t *ui_weather_view(void);
void ui_weather_result_text(const weather_result_t *result, char *buffer, size_t capacity);
#endif
