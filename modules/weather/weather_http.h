#ifndef WEATHER_HTTP_H
#define WEATHER_HTTP_H
#include "weather.h"
#include "weather_retry.h"
bool weather_cancelled(void);
/* Caller frees the bounded, decompressed PSRAM body with epd_app_free().
 * Failure detail contains local diagnostic fields, never server free text. */
char *weather_http_get(const weather_config_t *config, const char *path, int *status,
                       char *detail, size_t detail_capacity, weather_failure_t *failure);
#endif
