/* SPDX-License-Identifier: Apache-2.0 */
#include "weather.h"
#include "weather_store.h"
#include "weather_history.h"
#include "weather_http.h"
#include "weather_error.h"
#include "weather_json.h"
#include "epd_memory.h"
#include "network.h"
#include "app_service.h"
#include "system_time.h"
#include "platform/epd_i18n.h"
#include <rthw.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#define DBG_TAG "weather"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define UPDATE_INTERVAL_MS WEATHER_UPDATE_INTERVAL_MS
typedef struct
{
    weather_job_t job;
    uint32_t ticket;
    char city_id[WEATHER_CITY_ID_MAX];
} weather_request_t;

static rt_mutex_t lock;
static rt_mq_t requests;
static weather_config_t config;
static weather_info_t info;
static weather_result_t result;
static uint32_t next_ticket;
static epd_service_t *service;
static bool refresh(const weather_config_t *cfg);
static void publish(const weather_config_t *cfg, weather_info_t *data);
static bool summary_published;
static uint32_t summary_revision, summary_locale_revision;
/* Single-worker scratch; copied into the result together with its business code. */
static char server_detail[96];
static weather_failure_t request_failure;
static weather_retry_t retries[WEATHER_RETRY_COUNT];
static weather_config_t retry_owner;
static rt_tick_t last_period;
static bool retry_initialized, retry_dirty;
static bool refresh_complete;

static bool save_retries(void)
{
    bool saved = weather_store_retry_save(&retry_owner, retries, last_period);
    if (!saved && !retry_dirty) LOG_W("retry state save failed; runtime cooldown retained");
    retry_dirty = !saved;
    return !retry_dirty;
}

static void schedule_refresh(bool explicit_request)
{
    if (strcmp(retry_owner.api_host, config.api_host) || strcmp(retry_owner.api_key, config.api_key))
    {
        memset(retries, 0, sizeof(retries));
        retry_owner = config;
        last_period = rt_tick_get();
    }
    retry_initialized = true;
    for (unsigned i = 0; i < WEATHER_RETRY_COUNT; ++i)
        weather_retry_start(&retries[i], rt_tick_get(), explicit_request);
    save_retries();
}

bool weather_cancelled(void) { return service && epd_service_cancelled(service); }

static void empty_info(weather_info_t *out, const weather_config_t *cfg)
{
    memset(out, 0, sizeof(*out));
    out->code = 999;
    out->temperature = out->feels_like = out->high = out->low = WEATHER_MISSING;
    out->humidity = out->wind_speed = out->visibility = out->pressure = out->cloud = WEATHER_MISSING;
    out->wind_scale = WEATHER_MISSING;
    out->aqi = -1;
    rt_snprintf(out->city, sizeof(out->city), "%s", cfg->city);
    rt_snprintf(out->city_id, sizeof(out->city_id), "%s", cfg->city_id);
    rt_snprintf(out->region, sizeof(out->region), "%s", cfg->region);
}

bool weather_city_id_valid(const char *text)
{
    if (!text || !text[0] || strlen(text) >= WEATHER_CITY_ID_MAX) return false;
    for (; *text; ++text) if (*text < '0' || *text > '9') return false;
    return true;
}

static bool copy_string(char *dst, size_t capacity, cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity) return false;
    rt_snprintf(dst, capacity, "%s", item->valuestring);
    return dst[0] != '\0';
}

static int number(cJSON *object, const char *name, int min, int max)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    double value;
    if (cJSON_IsNumber(item)) value = item->valuedouble;
    else if (cJSON_IsString(item) && item->valuestring[0])
    {
        char *end;
        value = strtod(item->valuestring, &end);
        if (*end) return WEATHER_MISSING;
    }
    else return WEATHER_MISSING;
    if (!isfinite(value) || value < min || value > max) return WEATHER_MISSING;
    return (int)(value >= 0 ? value + 0.5 : value - 0.5);
}

static bool api_ok(cJSON *root)
{
    /* A transport failure already has its own diagnostic. */
    if (!root) return false;
    int code = weather_api_code(root), problem = weather_problem_status(root);
    bool ok = code == 200 && !cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!ok)
    {
        request_failure.error = weather_classify_api(root);
        if (request_failure.error == WEATHER_ERROR_NONE) request_failure.error = WEATHER_ERROR_TRANSIENT;
        if (request_failure.error == WEATHER_ERROR_RATE_LIMIT && !request_failure.retry_after)
            request_failure.retry_after = 60;
        const char *type = weather_problem_type(root);
        LOG_W("stage=api-code API=%d problem=%d type=%s", code, problem, type);
        rt_snprintf(server_detail, sizeof(server_detail), "stage=api-code API=%d problem=%d type=%s",
                    code, problem, type);
    }
    return ok;
}

static cJSON *get_json(const weather_config_t *cfg, const char *path)
{
    request_failure = (weather_failure_t){WEATHER_ERROR_TRANSIENT, 0};
    server_detail[0] = '\0';
    if (weather_cancelled() || !network_ready())
    {
        if (weather_cancelled()) request_failure.error = WEATHER_ERROR_CANCELLED;
        rt_snprintf(server_detail, sizeof(server_detail), "stage=%s",
                    weather_cancelled() ? "cancelled" : "network");
        return NULL;
    }
    int http_status;
    char *json = weather_http_get(cfg, path, &http_status, server_detail, sizeof(server_detail),
                                  &request_failure);
    if (!json) return NULL;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, 1);
    epd_app_free(json);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        request_failure.error = WEATHER_ERROR_TRANSIENT;
        rt_snprintf(server_detail, sizeof(server_detail), "stage=json-parse");
        return NULL;
    }
    return root;
}

static bool wait_network(void)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(15000);
    while (!weather_cancelled())
    {
        if (network_ready()) return true;
        rt_tick_t elapsed = rt_tick_get() - start;
        if (elapsed >= timeout) break;
        epd_service_wait(service, timeout - elapsed);
    }
    return false;
}

static bool parse_now(cJSON *root, weather_info_t *out)
{
    cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    if (!api_ok(root) || !cJSON_IsObject(now)) return false;
    out->temperature = number(now, "temp", -100, 100);
    out->code = number(now, "icon", 100, 999);
    if (out->temperature == WEATHER_MISSING || out->code == WEATHER_MISSING ||
        !copy_string(out->text, sizeof(out->text), now, "text")) return false;
    if (!copy_string(out->parts[WEATHER_NOW].update_time,
                     sizeof(out->parts[WEATHER_NOW].update_time), root, "updateTime")) return false;
    out->wind_dir[0] = out->wind_scale_text[0] = '\0';
    out->feels_like = number(now, "feelsLike", -150, 150);
    out->humidity = number(now, "humidity", 0, 100);
    out->wind_speed = number(now, "windSpeed", 0, 1000);
    out->wind_scale = number(now, "windScale", 0, 18);
    out->visibility = number(now, "vis", 0, 1000);
    out->pressure = number(now, "pressure", 0, 2000);
    out->cloud = number(now, "cloud", 0, 100);
    copy_string(out->wind_dir, sizeof(out->wind_dir), now, "windDir");
    copy_string(out->wind_scale_text, sizeof(out->wind_scale_text), now, "windScale");
    return true;
}

static bool parse_daily(cJSON *root, weather_info_t *out)
{
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!api_ok(root) || !cJSON_IsArray(daily) || cJSON_GetArraySize(daily) < 4) return false;
    out->forecast_count = 0;
    memset(out->forecast, 0, sizeof(out->forecast));
    out->sunrise[0] = out->sunset[0] = '\0';
    if (!copy_string(out->parts[WEATHER_DAILY].update_time,
                     sizeof(out->parts[WEATHER_DAILY].update_time), root, "updateTime")) return false;
    cJSON *today = cJSON_GetArrayItem(daily, 0);
    out->high = number(today, "tempMax", -100, 100);
    out->low = number(today, "tempMin", -100, 100);
    copy_string(out->sunrise, sizeof(out->sunrise), today, "sunrise");
    copy_string(out->sunset, sizeof(out->sunset), today, "sunset");
    if (out->high == WEATHER_MISSING || out->low == WEATHER_MISSING) return false;
    for (unsigned i = 0; i < WEATHER_FORECAST_MAX; ++i)
    {
        cJSON *day = cJSON_GetArrayItem(daily, i + 1);
        weather_forecast_t *forecast = &out->forecast[i];
        forecast->code = number(day, "iconDay", 100, 999);
        forecast->high = number(day, "tempMax", -100, 100);
        forecast->low = number(day, "tempMin", -100, 100);
        if (!copy_string(forecast->date, sizeof(forecast->date), day, "fxDate") ||
            !copy_string(forecast->text, sizeof(forecast->text), day, "textDay") ||
            forecast->code == WEATHER_MISSING || forecast->high == WEATHER_MISSING ||
            forecast->low == WEATHER_MISSING) return false;
        copy_string(forecast->wind_dir, sizeof(forecast->wind_dir), day, "windDirDay");
        copy_string(forecast->wind_scale_text, sizeof(forecast->wind_scale_text), day, "windScaleDay");
        forecast->wind_scale = number(day, "windScaleDay", 0, 18);
        ++out->forecast_count;
    }
    return true;
}

static bool parse_coordinate(cJSON *city, const char *name, int limit, int32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(city, name);
    char *end;
    if (!cJSON_IsString(item) || !item->valuestring[0]) return false;
    double value = strtod(item->valuestring, &end);
    if (*end || !isfinite(value) || value < -limit || value > limit) return false;
    *out = (int32_t)(value * 100 + (value >= 0 ? 0.5 : -0.5));
    return true;
}

static bool lookup_city(weather_config_t *cfg, const char *id)
{
    weather_retry_t *retry = &retries[WEATHER_LOOKUP];
    weather_retry_start(retry, rt_tick_get(), true);
    if (!weather_retry_due(retry, rt_tick_get()))
    {
        rt_snprintf(server_detail, sizeof(server_detail), "stage=city-cooldown");
        return false;
    }
    char path[128], returned_id[WEATHER_CITY_ID_MAX];
    rt_snprintf(path, sizeof(path), "/geo/v2/city/lookup?location=%s&number=1&lang=zh", id);
    cJSON *root = get_json(cfg, path);
    cJSON *cities = cJSON_GetObjectItemCaseSensitive(root, "location");
    cJSON *city = cJSON_GetArrayItem(cities, 0);
    bool ok = false;
    if (!api_ok(root)) goto finish;
    if (!cJSON_IsArray(cities) || !cJSON_IsObject(city))
    {
        LOG_W("stage=city-list no location object");
        rt_snprintf(server_detail, sizeof(server_detail), "stage=city-list");
        goto finish;
    }
    if (!copy_string(returned_id, sizeof(returned_id), city, "id") || strcmp(id, returned_id))
    {
        LOG_W("stage=city-id missing or mismatched ID");
        rt_snprintf(server_detail, sizeof(server_detail), "stage=city-id");
        goto finish;
    }
    if (!copy_string(cfg->city, sizeof(cfg->city), city, "name") ||
        !parse_coordinate(city, "lat", 90, &cfg->latitude) ||
        !parse_coordinate(city, "lon", 180, &cfg->longitude))
    {
        LOG_W("stage=city-fields invalid name or coordinates");
        rt_snprintf(server_detail, sizeof(server_detail), "stage=city-fields");
        goto finish;
    }
    {
        char province[32] = "", country[32] = "";
        copy_string(province, sizeof(province), city, "adm1");
        copy_string(country, sizeof(country), city, "country");
        rt_snprintf(cfg->city_id, sizeof(cfg->city_id), "%s", id);
        rt_snprintf(cfg->region, sizeof(cfg->region), "%s %s", country, province);
    }
    ok = true;
finish:
    cJSON_Delete(root);
    if (ok && !weather_cancelled()) request_failure = (weather_failure_t){WEATHER_ERROR_NONE, 0};
    else if (request_failure.error == WEATHER_ERROR_NONE) request_failure.error = WEATHER_ERROR_TRANSIENT;
    if (request_failure.error != WEATHER_ERROR_CANCELLED)
        weather_retry_finish(retry, rt_tick_get(), request_failure);
    save_retries();
    if (weather_cancelled()) return false;
    return ok;
}

static bool restore_city(weather_config_t *cfg, const char *id)
{
    weather_city_t city;
    if (!weather_history_find(id, &city)) return false;
    rt_snprintf(cfg->city_id, sizeof(cfg->city_id), "%s", city.city_id);
    rt_snprintf(cfg->city, sizeof(cfg->city), "%s", city.city);
    rt_snprintf(cfg->region, sizeof(cfg->region), "%s", city.region);
    cfg->latitude = city.latitude;
    cfg->longitude = city.longitude;
    return true;
}

static bool run_request(const weather_request_t *request)
{
    weather_config_t candidate;
    weather_info_t snapshot;
    weather_result_t completed = {0};
    completed.ticket = request->ticket;
    completed.job = request->job;
    completed.code = WEATHER_RESULT_SYNC_FAILED;
    server_detail[0] = '\0';
    bool ok = false;
    weather_get_config(&candidate);
    weather_get_info(&snapshot);
    if (request->job == WEATHER_JOB_SYNC)
    {
        schedule_refresh(true);
        ok = refresh(&candidate);
        if (ok) completed.code = refresh_complete ? WEATHER_RESULT_SYNC_OK : WEATHER_RESULT_SYNC_PARTIAL;
    }
    else if (request->job == WEATHER_JOB_IMPORT)
    {
        if (weather_store_import(&candidate, &completed.code))
        {
            ok = weather_store_save(&candidate, &snapshot);
            if (ok)
            {
                rt_mutex_take(lock, RT_WAITING_FOREVER);
                config = candidate;
                rt_mutex_release(lock);
                completed.code = WEATHER_RESULT_IMPORT_OK;
            }
            else completed.code = WEATHER_RESULT_CONFIG_SAVE_FAILED;
        }
    }
    else if (request->job == WEATHER_JOB_CITY || request->job == WEATHER_JOB_HISTORY_CITY)
    {
        bool from_history = request->job == WEATHER_JOB_HISTORY_CITY;
        if (from_history && !restore_city(&candidate, request->city_id))
            completed.code = WEATHER_RESULT_HISTORY_NOT_FOUND;
        else if (!from_history && !weather_config_valid(&candidate))
        {
            LOG_W("stage=city-config credentials not configured");
            completed.code = WEATHER_RESULT_CONFIG_REQUIRED;
        }
        else if (!from_history && !wait_network())
        {
            LOG_W("stage=city-network IP route not ready");
            completed.code = WEATHER_RESULT_NETWORK_UNAVAILABLE;
        }
        else if (!from_history && !lookup_city(&candidate, request->city_id))
            completed.code = WEATHER_RESULT_CITY_LOOKUP_FAILED;
        else if (!weather_history_record(&candidate, true))
            completed.code = WEATHER_RESULT_HISTORY_SAVE_FAILED;
        else
        {
            if (strcmp(candidate.city_id, snapshot.city_id)) empty_info(&snapshot, &candidate);
            rt_snprintf(snapshot.city, sizeof(snapshot.city), "%s", candidate.city);
            rt_snprintf(snapshot.region, sizeof(snapshot.region), "%s", candidate.region);
            ok = weather_store_save(&candidate, &snapshot);
            if (ok)
            {
                publish(&candidate, &snapshot);
                completed.code = WEATHER_RESULT_CITY_OK;
                rt_snprintf(completed.city, sizeof(completed.city), "%s", candidate.city);
            }
            else
            {
                LOG_W("stage=city-store save failed");
                completed.code = WEATHER_RESULT_CITY_SAVE_FAILED;
            }
        }
    }
    if (ok && (request->job == WEATHER_JOB_IMPORT || request->job == WEATHER_JOB_CITY ||
               request->job == WEATHER_JOB_HISTORY_CITY)) schedule_refresh(true);
    completed.success = ok;
    if (!ok) rt_snprintf(completed.detail, sizeof(completed.detail), "%s", server_detail);
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    result = completed;
    rt_mutex_release(lock);
    return ok;
}

static void worker_entry(epd_service_t *context)
{
    rt_tick_t interval = rt_tick_from_millisecond(UPDATE_INTERVAL_MS);
    if (!lock || !requests) return;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    service = context;
    rt_mutex_release(lock);
    if (!retry_initialized)
    {
        last_period = rt_tick_get();
        schedule_refresh(false);
    }

    while (!weather_cancelled())
    {
        weather_request_t request;
        if (rt_mq_recv(requests, &request, sizeof(request), 0) == RT_EOK)
        {
            run_request(&request);
            continue;
        }
        rt_tick_t now = rt_tick_get();
        if ((rt_tick_t)(now - last_period) >= interval)
        {
            last_period = now;
            schedule_refresh(false);
        }
        rt_int32_t wait = interval - (rt_tick_t)(now - last_period);
        bool due = false;
        for (unsigned i = 0; i < WEATHER_PART_COUNT; ++i)
        {
            due |= weather_retry_due(&retries[i], now);
            if (retries[i].pending && !retries[i].blocked && retries[i].remaining &&
                retries[i].remaining < (uint64_t)wait) wait = (rt_int32_t)retries[i].remaining;
        }
        weather_config_t current;
        weather_get_config(&current);
        if (due && network_ready() && weather_config_valid(&current) &&
            weather_city_id_valid(current.city_id))
        {
            refresh(&current);
            continue;
        }
        epd_service_wait(service, wait);
    }
    save_retries();
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    service = NULL;
    result.busy = false;
    result.success = false;
    result.code = WEATHER_RESULT_CANCELLED;
    result.city[0] = result.detail[0] = '\0';
    rt_mutex_release(lock);
}

static rt_err_t weather_service_init(void)
{
    if (requests) return RT_EOK;
    if (!lock) lock = rt_mutex_create("wx_lock", RT_IPC_FLAG_PRIO);
    if (!lock) return -RT_ENOMEM;
    memset(&config, 0, sizeof(config));
    empty_info(&info, &config);
    if (weather_store_load(&config, &info))
    {
        if (!weather_config_valid(&config) || strcmp(info.city_id, config.city_id) ||
            info.forecast_count < 0 || info.forecast_count > WEATHER_FORECAST_MAX)
            empty_info(&info, &config);
        info.state = info.valid ? WEATHER_STATE_CACHED : WEATHER_STATE_IDLE;
    }
    info.revision = 1;
    retry_owner = config;
    retry_initialized = weather_store_retry_load(&config, retries, &last_period);
    if (!weather_history_init())
    {
        rt_mutex_delete(lock);
        lock = RT_NULL;
        return -RT_ENOMEM;
    }
    /* Seed before pages can take their first history snapshot. */
    if (config.city[0] && weather_city_id_valid(config.city_id) &&
        !weather_history_record(&config, false)) LOG_W("city history seed failed");
    requests = rt_mq_create("wx_jobs", sizeof(weather_request_t), 2, RT_IPC_FLAG_FIFO);
    if (!requests)
    {
        weather_history_deinit();
        rt_mutex_delete(lock);
        lock = RT_NULL;
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

bool weather_service_ready(void) { return lock && requests; }

/* dlopen initializes the model before either a page or the worker can use it. */
static void weather_module_init(void)
{
    weather_json_init();
    weather_service_init();
}

static void weather_module_cleanup(void)
{
    /* Both the UI reference and the cooperative worker reference are gone. */
    if (requests) rt_mq_delete(requests);
    weather_history_deinit();
    if (lock) rt_mutex_delete(lock);
    requests = NULL;
    lock = NULL;
    service = NULL;
    summary_published = false;
}

MODULE_INIT_DEF(weather_module_init)
MODULE_CLEANUP_DEF(weather_module_cleanup)

static bool weather_service_ui_process(epd_service_t *context, bool stopping)
{
    if (stopping)
    {
        if (retry_dirty) LOG_W("retry state not saved; stopping with memory-only cooldown");
        summary_published = false;
        summary_revision = summary_locale_revision = 0;
        return true;
    }
    weather_info_t snapshot;
    /* The revision belongs to the same copy as the displayed business data. */
    weather_get_info(&snapshot);
    uint32_t locale_revision = epd_locale_revision();
    if (!summary_published || snapshot.revision != summary_revision ||
        locale_revision != summary_locale_revision)
    {
        char text[128];
        if (snapshot.parts[WEATHER_NOW].valid)
            EPD_FMT_weather_summary(text, sizeof(text), snapshot.city, snapshot.text, snapshot.temperature);
        else
            rt_snprintf(text, sizeof(text), "%s", epd_get_str(weather_no_data));
        epd_service_publish_summary(context, text);
        summary_revision = snapshot.revision;
        summary_locale_revision = locale_revision;
        summary_published = true;
    }
    return false;
}

int weather_command(int argc, char **argv);
const epd_background_t epd_app_background = {
    .run = worker_entry, .stack_size = 12288, .priority = 21, .network = true,
    .command = weather_command, .ui_process = weather_service_ui_process
};

rt_err_t weather_request_job(weather_job_t job, const char *city_id, uint32_t *ticket)
{
    weather_request_t request = {0};
    if (!requests) return -RT_ERROR;
    if (job <= WEATHER_JOB_NONE || job > WEATHER_JOB_HISTORY_CITY ||
        ((job == WEATHER_JOB_CITY || job == WEATHER_JOB_HISTORY_CITY) &&
         !weather_city_id_valid(city_id))) return -RT_EINVAL;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    if (!requests || !service || weather_cancelled()) { rt_mutex_release(lock); return -RT_EBUSY; }
    if (result.busy) { rt_mutex_release(lock); return -RT_EBUSY; }
    weather_result_t previous = result;
    memset(&result, 0, sizeof(result));
    if (++next_ticket == 0) ++next_ticket;
    request.job = job;
    request.ticket = next_ticket;
    if (city_id) rt_snprintf(request.city_id, sizeof(request.city_id), "%s", city_id);
    result.job = job;
    result.ticket = request.ticket;
    result.busy = true;
    result.code = WEATHER_RESULT_WORKING;
    rt_err_t ret = rt_mq_send(requests, &request, sizeof(request));
    if (ret == RT_EOK) { if (ticket) *ticket = request.ticket; epd_service_wake(service); }
    else result = previous;
    rt_mutex_release(lock);
    return ret;
}

void weather_get_result(weather_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lock) return;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = result;
    rt_mutex_release(lock);
}

void weather_get_config(weather_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lock) return;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = config;
    rt_mutex_release(lock);
}

bool weather_get_info(weather_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!lock) return false;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    *out = info;
    rt_mutex_release(lock);
    return out->valid;
}

weather_state_t weather_get_state(void)
{
    if (!lock) return WEATHER_STATE_IDLE;
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    weather_state_t state = info.state;
    rt_mutex_release(lock);
    return state;
}

rt_err_t weather_request_refresh(void)
{
    return weather_request_job(WEATHER_JOB_SYNC, RT_NULL, RT_NULL);
}

rt_err_t weather_set_city(const char *city_id)
{
    return weather_request_job(WEATHER_JOB_CITY, city_id, RT_NULL);
}

static void coordinate_text(char *buffer, size_t capacity, int32_t coordinate)
{
    unsigned absolute = coordinate < 0 ? -coordinate : coordinate;
    rt_snprintf(buffer, capacity, "%s%u.%02u", coordinate < 0 ? "-" : "", absolute / 100, absolute % 100);
}

static bool fetch_air(const weather_config_t *cfg, weather_info_t *out)
{
    char path[128], latitude[16], longitude[16];
    coordinate_text(latitude, sizeof(latitude), cfg->latitude);
    coordinate_text(longitude, sizeof(longitude), cfg->longitude);
    rt_snprintf(path, sizeof(path), "/airquality/v1/current/%s/%s?lang=zh", latitude, longitude);
    cJSON *root = get_json(cfg, path);
    bool ok = false;
    if (!root) return false;
    if (cJSON_GetObjectItemCaseSensitive(root, "error"))
    {
        api_ok(root);
        cJSON_Delete(root);
        return false;
    }
    cJSON *indexes = cJSON_GetObjectItemCaseSensitive(root, "indexes"), *item;
    /* Keep China's AQI scale; do not substitute a different international index. */
    cJSON_ArrayForEach(item, indexes)
    {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (cJSON_IsString(code) && !strcmp(code->valuestring, "cn-mee"))
        {
            int value = number(item, "aqi", 0, 500);
            if (value != WEATHER_MISSING)
            {
                out->aqi = value;
                out->aqi_category[0] = '\0';
                struct tm received;
                out->parts[WEATHER_AIR].update_time[0] = '\0';
                /* Air v1 has no provider timestamp; record local receipt time. */
                if (system_time_get_local(&received))
                    strftime(out->parts[WEATHER_AIR].update_time,
                             sizeof(out->parts[WEATHER_AIR].update_time), "%Y-%m-%dT%H:%M:%S", &received);
                ok = true;
                copy_string(out->aqi_category, sizeof(out->aqi_category), item, "category");
            }
            break;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void publish(const weather_config_t *cfg, weather_info_t *data)
{
    rt_mutex_take(lock, RT_WAITING_FOREVER);
    data->revision = info.revision + 1;
    config = *cfg;
    info = *data;
    rt_mutex_release(lock);
}

static bool refresh(const weather_config_t *cfg)
{
    weather_info_t fresh;
    bool updated = false;
    refresh_complete = false;
    weather_get_info(&fresh);
    if (strcmp(fresh.city_id, cfg->city_id)) empty_info(&fresh, cfg);
    if (!weather_config_valid(cfg) || !weather_city_id_valid(cfg->city_id) || !wait_network())
        return false;
    fresh.state = WEATHER_STATE_REFRESHING;
    publish(cfg, &fresh);
    for (unsigned i = 0; i < WEATHER_PART_COUNT && !weather_cancelled(); ++i)
    {
        if (!weather_retry_due(&retries[i], rt_tick_get())) continue;
        weather_info_t candidate = fresh;
        char path[128];
        bool parsed;
        if (i == WEATHER_AIR) parsed = fetch_air(cfg, &candidate);
        else
        {
            rt_snprintf(path, sizeof(path), "/v7/weather/%s?location=%s&lang=zh",
                        i == WEATHER_NOW ? "now" : "7d", cfg->city_id);
            cJSON *root = get_json(cfg, path);
            parsed = i == WEATHER_NOW ? parse_now(root, &candidate) : parse_daily(root, &candidate);
            cJSON_Delete(root);
        }
        if (weather_cancelled())
        {
            if (request_failure.error == WEATHER_ERROR_RATE_LIMIT ||
                request_failure.error == WEATHER_ERROR_REJECTED || request_failure.retry_after)
            {
                weather_retry_finish(&retries[i], rt_tick_get(), request_failure);
                save_retries();
            }
            break;
        }
        if (parsed)
        {
            fresh = candidate;
            fresh.parts[i].valid = true;
            fresh.valid = true;
            updated = true;
            request_failure = (weather_failure_t){WEATHER_ERROR_NONE, 0};
        }
        else if (request_failure.error == WEATHER_ERROR_NONE)
            request_failure.error = WEATHER_ERROR_TRANSIENT;
        fresh.parts[i].error = request_failure.error;
        weather_retry_finish(&retries[i], rt_tick_get(), request_failure);
        save_retries();
        if (!parsed)
            LOG_W("part=%u error=%u attempts=%u blocked=%d", i, (unsigned)request_failure.error,
                  retries[i].attempts, retries[i].blocked);
        /* Each validated part becomes visible before the next request. */
        publish(cfg, &fresh);
        if (parsed && !weather_store_save(cfg, &fresh)) LOG_W("weather cache save failed");
    }
    refresh_complete = true;
    for (unsigned i = 0; i < WEATHER_PART_COUNT; ++i)
        if (!fresh.parts[i].valid || fresh.parts[i].error != WEATHER_ERROR_NONE || retries[i].pending)
            refresh_complete = false;
    fresh.state = updated ? WEATHER_STATE_UPDATED : fresh.valid ? WEATHER_STATE_CACHED : WEATHER_STATE_FAILED;
    rt_snprintf(fresh.status, sizeof(fresh.status), "%s",
                refresh_complete ? "updated" : updated ? "partial" : "sync_failed");
    publish(cfg, &fresh);
    if (updated)
    {
        if (!weather_store_save(cfg, &fresh)) LOG_W("weather cache save failed");
        if (!weather_history_record(cfg, false)) LOG_W("city history save failed");
    }
    return updated;
}
