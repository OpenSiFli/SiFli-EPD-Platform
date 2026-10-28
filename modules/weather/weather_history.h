/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WEATHER_HISTORY_H
#define WEATHER_HISTORY_H

#include "weather.h"

typedef struct
{
    char city_id[WEATHER_CITY_ID_MAX], city[48], region[64];
    int32_t latitude, longitude;
} weather_city_t;

bool weather_history_init(void);
void weather_history_deinit(void);
/* One writer: startup or the weather worker. No filesystem work under the list lock. */
bool weather_history_record(const weather_config_t *config, bool recent);
bool weather_history_find(const char *city_id, weather_city_t *city);
/* Copies one page under the list lock and returns the total number of cities. */
unsigned weather_history_page(unsigned first, weather_city_t *cities, unsigned capacity);

#endif
