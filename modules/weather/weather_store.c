/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_store.h"
#include "storage.h"
#include "storage_file.h"
#include "system_time.h"
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <cJSON.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#define CONFIG_MAGIC 0x57584332u
#define CACHE_MAGIC 0x57584932u
#define RECORD_VERSION 1u
#define CACHE_VERSION 3u
static char config_path[96], cache_path[96];

typedef struct
{
    uint32_t host_crc, key_crc;
    int64_t saved_utc;
    rt_tick_t period_elapsed;
    weather_retry_t states[WEATHER_RETRY_COUNT];
} weather_retry_record_t;

typedef struct
{
    uint32_t config_crc;
    weather_info_t info;
    weather_retry_record_t retry;
} weather_cache_t;

static weather_config_t saved_config;
static bool saved_valid;
static weather_cache_t saved_cache;

bool weather_store_retry_save(const weather_config_t *config, const weather_retry_t *states, rt_tick_t last_period)
{
    if (!weather_config_valid(config)) return true;
    weather_retry_record_t record = {0};
    record.host_crc = storage_crc32(config->api_host, strlen(config->api_host));
    record.key_crc = storage_crc32(config->api_key, strlen(config->api_key));
    record.saved_utc = system_time_get_utc();
    rt_tick_t now = rt_tick_get();
    record.period_elapsed = now - last_period;
    memcpy(record.states, states, sizeof(record.states));
    for (unsigned i = 0; i < WEATHER_RETRY_COUNT; ++i)
        weather_retry_due(&record.states[i], now);
    saved_cache.retry = record;
    return storage_record_save(cache_path, CACHE_MAGIC, CACHE_VERSION, &saved_cache, sizeof(saved_cache));
}

bool weather_store_retry_load(const weather_config_t *config, weather_retry_t *states, rt_tick_t *last_period)
{
    weather_retry_record_t record = saved_cache.retry;
    if (!weather_config_valid(config) ||
        record.host_crc != storage_crc32(config->api_host, strlen(config->api_host)) ||
        record.key_crc != storage_crc32(config->api_key, strlen(config->api_key))) return false;
    int64_t utc = system_time_get_utc();
    uint64_t elapsed = 0;
    /* Whole-second clocks must never shorten a cooldown on reload. */
    if (record.saved_utc >= 1577836800 && utc > record.saved_utc)
        elapsed = (uint64_t)(utc - record.saved_utc - 1) * RT_TICK_PER_SECOND;
    rt_tick_t now = rt_tick_get();
    rt_tick_t period = rt_tick_from_millisecond(WEATHER_UPDATE_INTERVAL_MS);
    uint64_t age = elapsed + record.period_elapsed;
    *last_period = now - (age >= period ? period : (rt_tick_t)age);
    for (unsigned i = 0; i < WEATHER_RETRY_COUNT; ++i)
    {
        states[i] = record.states[i];
        states[i].remaining = elapsed >= states[i].remaining ? 0 : states[i].remaining - elapsed;
        states[i].observed_at = now;
    }
    return true;
}

bool weather_store_load(weather_config_t *config, weather_info_t *info)
{
    if (!storage_app_path(config_path, sizeof(config_path), "weather", STORAGE_APP_DATA, "config.bin") ||
        !storage_app_path(cache_path, sizeof(cache_path), "weather", STORAGE_APP_CACHE, "current.bin")) return false;
    weather_config_t candidate;
    memset(&saved_cache, 0, sizeof(saved_cache));
    saved_valid = storage_record_load(config_path, CONFIG_MAGIC, RECORD_VERSION,
                                      &candidate, sizeof(candidate));
    if (!saved_valid) return false;
    *config = saved_config = candidate;
    /* Cache is dispensable. Never combine a city's old snapshot with new config. */
    if (storage_record_load(cache_path, CACHE_MAGIC, CACHE_VERSION, &saved_cache, sizeof(saved_cache)))
    {
        if (saved_cache.config_crc == storage_crc32(config, sizeof(*config))) *info = saved_cache.info;
    }
    else memset(&saved_cache, 0, sizeof(saved_cache));
    return true;
}

bool weather_store_save(const weather_config_t *config, const weather_info_t *info)
{
    if (!storage_app_available("weather")) return false;
    weather_cache_t cache = saved_cache;
    cache.config_crc = storage_crc32(config, sizeof(*config));
    cache.info = *info;
    if (!storage_record_save(cache_path, CACHE_MAGIC, CACHE_VERSION, &cache, sizeof(cache))) return false;
    saved_cache = cache;
    /* Publish config last. An interrupted pair can invalidate cache, not config. */
    if ((!saved_valid || memcmp(config, &saved_config, sizeof(*config))) &&
        !storage_record_save(config_path, CONFIG_MAGIC, RECORD_VERSION, config, sizeof(*config))) return false;
    saved_config = *config;
    saved_valid = true;
    return true;
}

bool weather_config_valid(const weather_config_t *config)
{
    static const char suffix[] = ".qweatherapi.com";
    size_t host_len = strlen(config->api_host), key_len = strlen(config->api_key);
    if (host_len <= sizeof(suffix) - 1 || host_len >= sizeof(config->api_host) ||
        strcmp(config->api_host + host_len - (sizeof(suffix) - 1), suffix) ||
        key_len == 0 || key_len >= sizeof(config->api_key)) return false;
    for (size_t i = 0; i < host_len; ++i)
        if (!isalnum((unsigned char)config->api_host[i]) && config->api_host[i] != '-' &&
            config->api_host[i] != '.') return false;
    for (size_t i = 0; i < key_len; ++i)
        if (!isalnum((unsigned char)config->api_key[i]) && config->api_key[i] != '-' &&
            config->api_key[i] != '_') return false;
    return true;
}

bool weather_store_import(weather_config_t *config, weather_result_code_t *error)
{
    char path[128], json[1025];
    int fd = -1, count;
    struct stat st;
    cJSON *root = RT_NULL, *host, *key;
    bool ok = false;
    *error = WEATHER_RESULT_SD_REQUIRED;
    storage_lock();
    if (!storage_available(STORAGE_SD)) goto finish;
    rt_snprintf(path, sizeof(path), STORAGE_SD_ROOT "/%s", WEATHER_CONFIG_FILE);
    *error = WEATHER_RESULT_CONFIG_NOT_FOUND;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) goto finish;
    *error = WEATHER_RESULT_CONFIG_INVALID;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024) goto finish;
    count = read(fd, json, st.st_size);
    if (count != st.st_size) goto finish;
    json[count] = '\0';
    const char *start = json;
    if (count >= 3 && (unsigned char)json[0] == 0xef &&
        (unsigned char)json[1] == 0xbb && (unsigned char)json[2] == 0xbf) start += 3;
    root = cJSON_ParseWithOpts(start, RT_NULL, 1);
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 2) goto finish;
    host = cJSON_GetObjectItemCaseSensitive(root, "api_host");
    key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    if (!cJSON_IsString(host) || !cJSON_IsString(key) ||
        strlen(host->valuestring) >= sizeof(config->api_host) ||
        strlen(key->valuestring) >= sizeof(config->api_key)) goto finish;
    const char *host_text = host->valuestring;
    if (!strncmp(host_text, "https://", 8)) host_text += 8;
    rt_snprintf(config->api_host, sizeof(config->api_host), "%s", host_text);
    size_t n = strlen(config->api_host);
    if (n && config->api_host[n - 1] == '/') config->api_host[n - 1] = '\0';
    rt_snprintf(config->api_key, sizeof(config->api_key), "%s", key->valuestring);
    ok = weather_config_valid(config);
    if (ok) *error = WEATHER_RESULT_IMPORT_OK;
finish:
    if (root) cJSON_Delete(root);
    if (fd >= 0) close(fd);
    memset(json, 0, sizeof(json));
    storage_unlock();
    return ok;
}
