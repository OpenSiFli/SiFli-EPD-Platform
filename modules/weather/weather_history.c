/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_history.h"
#include "storage_file.h"
#include "epd_memory.h"
#include <dfs_posix.h>
#include <string.h>

static char history_path[96];
#define HISTORY_MAGIC 0x57584831u
#define HISTORY_VERSION 1u

typedef struct
{
    weather_city_t city;
    uint64_t sequence;
} city_record_t;

typedef struct city_node
{
    struct city_node *next;
    city_record_t record;
} city_node_t;

static city_node_t *s_cities;
static rt_mutex_t s_lock;
static uint64_t s_sequence;

static bool city_valid(const weather_city_t *city)
{
    return memchr(city->city_id, 0, sizeof(city->city_id)) &&
           memchr(city->city, 0, sizeof(city->city)) && city->city[0] &&
           memchr(city->region, 0, sizeof(city->region)) &&
           weather_city_id_valid(city->city_id) &&
           city->latitude >= -9000 && city->latitude <= 9000 &&
           city->longitude >= -18000 && city->longitude <= 18000;
}

static city_node_t **city_link(const char *id)
{
    city_node_t **link = &s_cities;
    while (*link && strcmp((*link)->record.city.city_id, id))
    {
        link = &(*link)->next;
    }
    return link;
}

static void city_insert(city_node_t *node)
{
    city_node_t **link = &s_cities;
    while (*link && (*link)->record.sequence > node->record.sequence)
    {
        link = &(*link)->next;
    }
    node->next = *link;
    *link = node;
}

void weather_history_deinit(void)
{
    while (s_cities)
    {
        city_node_t *next = s_cities->next;
        epd_app_free(s_cities);
        s_cities = next;
    }
    if (s_lock) rt_mutex_delete(s_lock);
    s_lock = NULL;
    s_sequence = 0;
}

bool weather_history_init(void)
{
    if (s_lock) return true;
    if (!storage_app_path(history_path, sizeof(history_path), "weather", STORAGE_APP_DATA, "cities")) return false;
    s_lock = rt_mutex_create("wx_hist", RT_IPC_FLAG_PRIO);
    if (!s_lock) return false;
    bool ok = true;
    storage_lock();
    DIR *directory = storage_path_available(history_path) ? opendir(history_path) : NULL;
    if (directory)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL)
        {
            if (!storage_app_available("weather")) { ok = false; break; }
            size_t length = strlen(entry->d_name);
            char id[WEATHER_CITY_ID_MAX], path[160];
            if (length > 4 && !strcmp(entry->d_name + length - 4, ".old")) length -= 4;
            if (length <= 4 || length - 4 >= sizeof(id) ||
                strncmp(entry->d_name + length - 4, ".bin", 4)) continue;
            memcpy(id, entry->d_name, length - 4);
            id[length - 4] = 0;
            if (!weather_city_id_valid(id) || *city_link(id)) continue;
            rt_snprintf(path, sizeof(path), "%s/%s.bin", history_path, id);
            city_record_t record;
            if (!storage_record_load(path, HISTORY_MAGIC, HISTORY_VERSION, &record, sizeof(record)) ||
                !city_valid(&record.city) || strcmp(record.city.city_id, id)) continue;
            city_node_t *node = epd_app_alloc(sizeof(*node), EPD_APP_PSRAM);
            if (!node)
            {
                ok = false;
                break;
            }
            node->record = record;
            city_insert(node);
            if (record.sequence > s_sequence) s_sequence = record.sequence;
        }
        closedir(directory);
    }
    storage_unlock();
    if (!ok) weather_history_deinit();
    return ok;
}

bool weather_history_record(const weather_config_t *config, bool recent)
{
    city_record_t record = {0};
    rt_snprintf(record.city.city_id, sizeof(record.city.city_id), "%s", config->city_id);
    rt_snprintf(record.city.city, sizeof(record.city.city), "%s", config->city);
    rt_snprintf(record.city.region, sizeof(record.city.region), "%s", config->region);
    record.city.latitude = config->latitude;
    record.city.longitude = config->longitude;
    if (!s_lock || !storage_app_available("weather") || !city_valid(&record.city)) return false;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    city_node_t *node = *city_link(record.city.city_id);
    bool unchanged = node && !memcmp(&node->record.city, &record.city, sizeof(record.city));
    /* Periodic syncs do not rewrite history. Re-selecting the first city also needs no write. */
    if (unchanged && (!recent || node == s_cities))
    {
        rt_mutex_release(s_lock);
        return true;
    }
    record.sequence = recent || !node ? s_sequence + 1 : node->record.sequence;
    rt_mutex_release(s_lock);

    bool added = node == NULL;
    if (added) node = epd_app_alloc(sizeof(*node), EPD_APP_PSRAM);
    if (!node) return false;
    char path[160];
    rt_snprintf(path, sizeof(path), "%s/%s.bin", history_path, record.city.city_id);
    if (!storage_record_save(path, HISTORY_MAGIC, HISTORY_VERSION, &record, sizeof(record)))
    {
        if (added) epd_app_free(node);
        return false;
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (!added) *city_link(record.city.city_id) = node->next;
    node->record = record;
    city_insert(node);
    if (record.sequence > s_sequence) s_sequence = record.sequence;
    rt_mutex_release(s_lock);
    return true;
}

bool weather_history_find(const char *city_id, weather_city_t *city)
{
    if (!s_lock) return false;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    city_node_t *node = *city_link(city_id);
    if (node) *city = node->record.city;
    rt_mutex_release(s_lock);
    return node != NULL;
}

unsigned weather_history_page(unsigned first, weather_city_t *cities, unsigned capacity)
{
    unsigned total = 0, copied = 0;
    memset(cities, 0, capacity * sizeof(*cities));
    if (!s_lock) return 0;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    for (city_node_t *node = s_cities; node; node = node->next, ++total)
    {
        if (total >= first && copied < capacity) cities[copied++] = node->record.city;
    }
    rt_mutex_release(s_lock);
    return total;
}
