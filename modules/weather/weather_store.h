#ifndef WEATHER_STORE_H
#define WEATHER_STORE_H
#include "weather.h"
#include "weather_retry.h"
/* Called by the weather worker only, except for loading before its startup. */
bool weather_store_load(weather_config_t *config, weather_info_t *info);
bool weather_store_save(const weather_config_t *config, const weather_info_t *info);
bool weather_store_import(weather_config_t *config, weather_result_code_t *error);
bool weather_config_valid(const weather_config_t *config);
bool weather_store_retry_load(const weather_config_t *config, weather_retry_t *states, rt_tick_t *last_period);
bool weather_store_retry_save(const weather_config_t *config, const weather_retry_t *states, rt_tick_t last_period);
#endif
