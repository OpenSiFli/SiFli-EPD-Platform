/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_WEATHER_H
#define EPD_WEATHER_H

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#define WEATHER_FORECAST_MAX 3
#define WEATHER_MISSING INT_MIN
#define WEATHER_CITY_ID_MAX 16
#define WEATHER_CONFIG_FILE "qweather.json"
#define WEATHER_CITY_LOOKUP_URL "https://dev.qweather.com/docs/resource/location-list/"

typedef enum
{
    WEATHER_STATE_IDLE, WEATHER_STATE_REFRESHING, WEATHER_STATE_UPDATED,
    WEATHER_STATE_CACHED, WEATHER_STATE_FAILED
} weather_state_t;

typedef enum
{
    WEATHER_ERROR_NONE, WEATHER_ERROR_TRANSIENT, WEATHER_ERROR_REJECTED,
    WEATHER_ERROR_RATE_LIMIT, WEATHER_ERROR_CANCELLED
} weather_error_t;

typedef enum { WEATHER_NOW, WEATHER_DAILY, WEATHER_AIR, WEATHER_PART_COUNT } weather_part_t;

typedef struct
{
    bool valid;
    weather_error_t error;
    char update_time[32];
} weather_part_info_t;

typedef struct
{
    char date[16], text[32];
    int code, high, low;
    char wind_dir[24], wind_scale_text[16];
    int wind_scale;
} weather_forecast_t;

/* Snapshots own their strings; none point into JSON or the worker's buffers. */
typedef struct
{
    bool valid;
    weather_state_t state;
    uint32_t revision;
    char status[64], city[48], city_id[WEATHER_CITY_ID_MAX], region[64];
    weather_part_info_t parts[WEATHER_PART_COUNT];
    char text[32];
    int code, temperature, feels_like, high, low;
    int humidity, wind_scale, wind_speed, visibility, pressure, cloud, aqi;
    char wind_dir[24], wind_scale_text[16], aqi_category[32];
    char sunrise[8], sunset[8];
    weather_forecast_t forecast[WEATHER_FORECAST_MAX];
    int forecast_count;
} weather_info_t;

typedef struct
{
    char api_host[128], api_key[128];
    char city_id[WEATHER_CITY_ID_MAX], city[48], region[64];
    /* Hundredths of a degree for the air-quality API. */
    int32_t latitude, longitude;
} weather_config_t;

typedef enum
{
    WEATHER_RESULT_NONE, WEATHER_RESULT_WORKING, WEATHER_RESULT_SYNC_OK,
    WEATHER_RESULT_SYNC_FAILED, WEATHER_RESULT_IMPORT_OK, WEATHER_RESULT_CONFIG_SAVE_FAILED,
    WEATHER_RESULT_HISTORY_NOT_FOUND, WEATHER_RESULT_CONFIG_REQUIRED,
    WEATHER_RESULT_NETWORK_UNAVAILABLE, WEATHER_RESULT_CITY_LOOKUP_FAILED,
    WEATHER_RESULT_HISTORY_SAVE_FAILED, WEATHER_RESULT_CITY_OK, WEATHER_RESULT_CITY_SAVE_FAILED,
    WEATHER_RESULT_CANCELLED, WEATHER_RESULT_SD_REQUIRED, WEATHER_RESULT_CONFIG_NOT_FOUND,
    WEATHER_RESULT_CONFIG_INVALID, WEATHER_RESULT_OPERATION_FAILED,
    WEATHER_RESULT_PAGE_OPEN_FAILED, WEATHER_RESULT_BACK_FAILED, WEATHER_RESULT_CITY_ID_REQUIRED,
    WEATHER_RESULT_SYNC_PARTIAL
} weather_result_code_t;

typedef enum
{
    WEATHER_JOB_NONE, WEATHER_JOB_SYNC, WEATHER_JOB_IMPORT, WEATHER_JOB_CITY,
    WEATHER_JOB_HISTORY_CITY
} weather_job_t;

typedef struct
{
    uint32_t ticket;
    weather_job_t job;
    bool busy, success;
    weather_result_code_t code;
    char city[48];
    char detail[96];
} weather_result_t;

bool weather_service_ready(void);
bool weather_city_id_valid(const char *city_id);
/* Only enqueue work; HTTP, parsing and persistence run on a single worker. */
rt_err_t weather_request_job(weather_job_t job, const char *city_id, uint32_t *ticket);
void weather_get_result(weather_result_t *out);
void weather_get_config(weather_config_t *out);
bool weather_get_info(weather_info_t *out);
weather_state_t weather_get_state(void);
rt_err_t weather_request_refresh(void);
rt_err_t weather_set_city(const char *city_id);

#endif
