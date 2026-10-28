#include "ui_settings_internal.h"
#include "ui_settings_store.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "sleep_guard.h"
#include "epd_memory.h"
#include "platform/epd_power.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define SETTINGS_PATH STORAGE_FLASH_ROOT "/system/settings.bin"
#define SETTINGS_MAGIC 0x45505332u
#define SETTINGS_VERSION 2u
#define V1_SIZE 13u
#define HEADER_SIZE 16u
#define V2_REQUIRED_FIELDS 14u
#define V2_FIELDS 15u
#define V2_SIZE (V2_FIELDS * 4u)
#define V2_MAX_SIZE 64u

static struct rt_mutex jobs_lock;
static struct rt_semaphore jobs_wake, jobs_done;
static bool started, queued, writing, barrier, failed;
static uint8_t pending[UI_SETTING_COUNT];
static uint32_t result_revision;
static sleep_guard_owner_t save_hold = {.id = "ui-settings"};

/* Only the worker or a drained synchronous reload accesses this cache, with
 * storage_lock held. No queue mutex is held during filesystem operations. */
static bool cache_valid;
static uint8_t cached[UI_SETTING_COUNT];
static uint32_t cached_path_revision, cached_volume_revision;
static bool save_record(const uint8_t *values);

static void jobs_take(void) { rt_mutex_take(&jobs_lock, RT_WAITING_FOREVER); }
static void jobs_release(void) { rt_mutex_release(&jobs_lock); }

static void save_worker(void *unused)
{
    (void)unused;
    for (;;)
    {
        rt_sem_take(&jobs_wake, RT_WAITING_FOREVER);
        for (;;)
        {
            uint8_t values[UI_SETTING_COUNT];
            jobs_take();
            if (!queued) { jobs_release(); break; }
            memcpy(values, pending, sizeof(values));
            queued = false; writing = true;
            jobs_release();
            bool ok = save_record(values);
            jobs_take();
            writing = false;
            /* A newer pending snapshot is authoritative; completing this one
             * must not acknowledge or overwrite its status or runtime values. */
            if (!queued) { failed = !ok; ++result_revision; sleep_guard_release(&save_hold); }
            if (barrier) rt_sem_release(&jobs_done);
            jobs_release();
            epd_power_notify();
        }
    }
}

bool ui_settings_store_start(void)
{
    if (started) return true;
    if (rt_mutex_init(&jobs_lock, "uiset", RT_IPC_FLAG_PRIO) != RT_EOK) return false;
    if (rt_sem_init(&jobs_wake, "uisetw", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto mutex_failed;
    if (rt_sem_init(&jobs_done, "uisetd", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto wake_failed;
    rt_thread_t thread = epd_thread_create_psram("ui_settings", save_worker, NULL, 3072, 23, 10);
    if (!thread) goto done_failed;
    if (rt_thread_startup(thread) != RT_EOK) { rt_thread_delete(thread); goto done_failed; }
    started = true;
    return true;
done_failed:
    rt_sem_detach(&jobs_done);
wake_failed:
    rt_sem_detach(&jobs_wake);
mutex_failed:
    rt_mutex_detach(&jobs_lock);
    return false;
}

ui_settings_save_status_t ui_settings_store_status(uint32_t *revision)
{
    if (!started) { if (revision) *revision = 0; return UI_SETTINGS_SAVE_OK; }
    jobs_take();
    ui_settings_save_status_t status = queued || writing ? UI_SETTINGS_SAVE_PENDING :
                                       failed ? UI_SETTINGS_SAVE_FAILED : UI_SETTINGS_SAVE_OK;
    if (revision) *revision = result_revision;
    jobs_release();
    return status;
}

/* Called with jobs_lock held by the single synchronous barrier owner. */
static bool drain_locked(void)
{
    while (queued || writing)
    {
        jobs_release();
        rt_sem_take(&jobs_done, RT_WAITING_FOREVER);
        jobs_take();
    }
    return !failed;
}

static void cache_remember(const uint8_t *values)
{
    memcpy(cached, values, sizeof(cached));
    cached_path_revision = storage_path_revision(SETTINGS_PATH);
    cached_volume_revision = storage_volume_revision(STORAGE_FLASH);
    cache_valid = !storage_file_writers(SETTINGS_PATH);
}

typedef struct
{
    ui_setting_id_t id;
    uint16_t tag;
    unsigned count;
    uint16_t options[8];
} setting_field_t;

/* V2: little-endian (u16 field tag, u16 semantic value) pairs. Tags and
 * values are independent of enum ordinals and page order. Time 0 means never;
 * scale uses percent, line spacing uses hundredths, sizes/margins use pixels.
 * Identity values for fonts/weights/encoding are fixed wire IDs. */
static const setting_field_t fields[V2_FIELDS] = {
    {UI_SETTING_TOUCH,        1, 2, {0, 1}},
    {UI_SETTING_TIMEOUT,      2, 6, {1, 3, 5, 10, 15, 0}},
    {UI_SETTING_FULL_REFRESH, 3, 6, {1, 5, 8, 10, 15, 20}},
    {UI_SETTING_BLUETOOTH,    4, 2, {0, 1}},
    {UI_SETTING_WIFI,         5, 2, {0, 1}},
    {UI_SETTING_LOW_POWER,    7, 2, {0, 1}},
    {UI_SETTING_FONT,         8, 5, {0, 1, 2, 3, 4}},
    {UI_SETTING_FONT_SIZE,    9, 8, {28, 30, 32, 34, 36, 40, 44, 48}},
    {UI_SETTING_FONT_WEIGHT, 10, 3, {0, 1, 2}},
    {UI_SETTING_LINE_SPACING,11, 5, {100, 120, 140, 160, 180}},
    {UI_SETTING_MARGIN,      12, 5, {4, 8, 12, 16, 20}},
    {UI_SETTING_ENCODING,    13, 3, {0, 1, 2}},
    {UI_SETTING_UI_SCALE,    14, 5, {90, 95, 100, 110, 120}},
    /* Tag 15 is retired; system sleep keeps its existing wire ID. */
    {UI_SETTING_SYSTEM_SLEEP,16, 6, {1, 5, 15, 30, 60, 0}},
    {UI_SETTING_NAVIGATION,  17, 2, {0, 1}}
};

/* Frozen v1 slots/options, including the original five timeout choices. */
static const setting_field_t legacy[V1_SIZE] = {
    {UI_SETTING_TOUCH,        1, 2, {0, 1}},
    {UI_SETTING_TIMEOUT,      2, 5, {1, 3, 5, 10, 15}},
    {UI_SETTING_FULL_REFRESH, 3, 6, {1, 5, 8, 10, 15, 20}},
    {UI_SETTING_BLUETOOTH,    4, 2, {0, 1}},
    {UI_SETTING_WIFI,         5, 2, {0, 1}},
    {UI_SETTING_COUNT,        6, 3, {0, 1, 2}}, /* Retired network selection. */
    {UI_SETTING_LOW_POWER,    7, 2, {0, 1}},
    {UI_SETTING_FONT,         8, 5, {0, 1, 2, 3, 4}},
    {UI_SETTING_FONT_SIZE,    9, 8, {16, 18, 20, 22, 24, 28, 32, 36}},
    {UI_SETTING_FONT_WEIGHT, 10, 3, {0, 1, 2}},
    {UI_SETTING_LINE_SPACING,11, 5, {100, 120, 140, 160, 180}},
    {UI_SETTING_MARGIN,      12, 5, {4, 8, 12, 16, 20}},
    {UI_SETTING_ENCODING,    13, 3, {0, 1, 2}}
};

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned get_u16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void put_u16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void default_values(uint8_t values[UI_SETTING_COUNT])
{
    for (unsigned i = 0; i < UI_SETTING_COUNT; ++i)
        values[i] = (uint8_t)ui_settings_default((ui_setting_id_t)i);
}

static bool decode_value(const setting_field_t *field, unsigned value, uint8_t *values)
{
    /* Retired 16/18/20/22/24 px choices use the minimum reading size. */
    if (field->id == UI_SETTING_FONT_SIZE && value >= 16 && value <= 24 && !(value & 1u))
        value = 28;
    for (unsigned i = 0; i < field->count; ++i)
        if (field->options[i] == value)
        {
            values[field->id] = (uint8_t)i;
            return true;
        }
    return false;
}

static bool decode_v1(const uint8_t *payload, uint8_t *values)
{
    default_values(values);
    for (unsigned i = 0; i < V1_SIZE; ++i)
    {
        if (payload[i] >= legacy[i].count) return false;
        if (legacy[i].tag == 6) continue;
        unsigned j;
        for (j = 0; j < V2_FIELDS; ++j)
            if (fields[j].tag == legacy[i].tag) break;
        if (j == V2_FIELDS || !decode_value(&fields[j], legacy[i].options[payload[i]], values))
            return false;
    }
    for (unsigned i = 0; i < V2_FIELDS; ++i)
        if (fields[i].id == UI_SETTING_SYSTEM_SLEEP)
            return decode_value(&fields[i], payload[6] ? 15 : 0, values);
    return false;
}

static bool decode_v2(const uint8_t *payload, unsigned size, uint8_t *values)
{
    default_values(values);
    bool seen[V2_FIELDS] = {false};
    for (unsigned i = 0; i < size / 4u; ++i)
    {
        unsigned tag = get_u16(payload + i * 4);
        if (tag == 6 || tag == 15) continue;
        unsigned j;
        for (j = 0; j < V2_FIELDS; ++j)
            if (fields[j].tag == tag) break;
        if (j == V2_FIELDS || seen[j] ||
            !decode_value(&fields[j], get_u16(payload + i * 4 + 2), values)) return false;
        seen[j] = true;
    }
    for (unsigned i = 0; i < V2_REQUIRED_FIELDS; ++i)
        if (!seen[i]) return false;
    return true;
}

/* Called with storage_lock held. A short read can be completed, but EOF before
 * the length reported by fstat is an IO failure, not a missing/default record. */
static bool read_exact(int fd, uint8_t *data, size_t size)
{
    while (size)
    {
        int count = read(fd, data, size);
        if (count <= 0 || (size_t)count > size) return false;
        data += count;
        size -= (size_t)count;
    }
    return true;
}

static ui_settings_load_result_t read_record(uint8_t *values, bool *v1)
{
    *v1 = false;
    if (!storage_path_available(SETTINGS_PATH) || !storage_file_recover(SETTINGS_PATH))
        return UI_SETTINGS_LOAD_IO_ERROR;
    uint32_t revision = storage_path_revision(SETTINGS_PATH);
    uint32_t volume = storage_volume_revision(STORAGE_FLASH);
    if (storage_file_writers(SETTINGS_PATH)) return UI_SETTINGS_LOAD_IO_ERROR;
    int fd = open(SETTINGS_PATH, O_RDONLY);
    if (fd < 0)
    {
        /* SDK DFS reports negative errno; accept POSIX errno for host ports. */
        int error = errno;
        return storage_path_available(SETTINGS_PATH) && (error == -ENOENT || error == ENOENT)
               ? UI_SETTINGS_LOAD_MISSING : UI_SETTINGS_LOAD_IO_ERROR;
    }
    uint8_t header[HEADER_SIZE], payload[V2_MAX_SIZE];
    struct stat st;
    ui_settings_load_result_t result = UI_SETTINGS_LOAD_INVALID;
    if (fstat(fd, &st) != 0) result = UI_SETTINGS_LOAD_IO_ERROR;
    else if (S_ISREG(st.st_mode) && st.st_size >= HEADER_SIZE)
    {
        if (!read_exact(fd, header, sizeof(header))) result = UI_SETTINGS_LOAD_IO_ERROR;
        else if (get_u32(header) == SETTINGS_MAGIC)
        {
            uint32_t version = get_u32(header + 4);
            if (version != 1 && version != SETTINGS_VERSION) result = UI_SETTINGS_LOAD_UNKNOWN_VERSION;
            else
            {
                unsigned size = get_u32(header + 8);
                bool valid_size = version == 1 ? size == V1_SIZE :
                                  size >= V2_REQUIRED_FIELDS * 4u && size <= V2_MAX_SIZE && !(size % 4u);
                if (valid_size && st.st_size == HEADER_SIZE + size)
                {
                    if (!read_exact(fd, payload, size)) result = UI_SETTINGS_LOAD_IO_ERROR;
                    else if (get_u32(header + 12) == storage_crc32(payload, size) &&
                             (version == 1 ? decode_v1(payload, values) : decode_v2(payload, size, values)))
                    {
                        *v1 = version == 1;
                        result = UI_SETTINGS_LOAD_OK;
                    }
                }
            }
        }
    }
    if (close(fd) != 0 || !storage_path_available(SETTINGS_PATH) ||
        revision != storage_path_revision(SETTINGS_PATH) || volume != storage_volume_revision(STORAGE_FLASH) ||
        storage_file_writers(SETTINGS_PATH)) result = UI_SETTINGS_LOAD_IO_ERROR;
    return result;
}

static bool encode_record(const uint8_t *values, uint8_t record[HEADER_SIZE + V2_SIZE])
{
    if (!values) return false;
    for (unsigned i = 0; i < V2_FIELDS; ++i)
    {
        const setting_field_t *field = &fields[i];
        if (values[field->id] >= field->count) return false;
        uint8_t *p = record + HEADER_SIZE + i * 4;
        put_u16(p, field->tag);
        put_u16(p + 2, field->options[values[field->id]]);
    }
    put_u32(record, SETTINGS_MAGIC);
    put_u32(record + 4, SETTINGS_VERSION);
    put_u32(record + 8, V2_SIZE);
    put_u32(record + 12, storage_crc32(record + HEADER_SIZE, V2_SIZE));
    return true;
}

ui_settings_load_result_t ui_settings_store_reload(uint8_t values[UI_SETTING_COUNT])
{
    if (!values) return UI_SETTINGS_LOAD_IO_ERROR;
    if (started)
    {
        jobs_take();
        if (barrier) { jobs_release(); return UI_SETTINGS_LOAD_IO_ERROR; }
        barrier = true;
        if (!drain_locked()) { barrier = false; jobs_release(); return UI_SETTINGS_LOAD_IO_ERROR; }
        jobs_release();
    }
    uint8_t candidate[UI_SETTING_COUNT];
    bool v1;
    storage_lock();
    cache_valid = false;
    ui_settings_load_result_t result = read_record(candidate, &v1);
    if (result == UI_SETTINGS_LOAD_OK && v1)
    {
        uint8_t record[HEADER_SIZE + V2_SIZE];
        result = encode_record(candidate, record) && storage_file_replace(SETTINGS_PATH, record, sizeof(record))
                 ? UI_SETTINGS_LOAD_MIGRATED : UI_SETTINGS_LOAD_MIGRATION_PENDING;
    }
    if (result == UI_SETTINGS_LOAD_OK || result == UI_SETTINGS_LOAD_MIGRATED) cache_remember(candidate);
    storage_unlock();
    if (started) { jobs_take(); barrier = false; jobs_release(); }
    if (result != UI_SETTINGS_LOAD_IO_ERROR)
    {
        if (result == UI_SETTINGS_LOAD_MISSING || result == UI_SETTINGS_LOAD_INVALID ||
            result == UI_SETTINGS_LOAD_UNKNOWN_VERSION) default_values(candidate);
        memcpy(values, candidate, sizeof(candidate));
    }
    return result;
}

bool ui_settings_store_load(uint8_t values[UI_SETTING_COUNT])
{
    uint8_t candidate[UI_SETTING_COUNT];
    if (!values) return false;
    ui_settings_load_result_t result = ui_settings_store_reload(candidate);
    if (result != UI_SETTINGS_LOAD_OK && result != UI_SETTINGS_LOAD_MIGRATED &&
        result != UI_SETTINGS_LOAD_MIGRATION_PENDING) return false;
    memcpy(values, candidate, sizeof(candidate));
    return true;
}

static bool save_record(const uint8_t *values)
{
    uint8_t record[HEADER_SIZE + V2_SIZE], current[UI_SETTING_COUNT];
    if (!encode_record(values, record)) return false;
    bool v1;
    storage_lock();
    if (!system_reset_write_allowed() || !storage_path_available(SETTINGS_PATH) || storage_file_writers(SETTINGS_PATH))
    { cache_valid = false; storage_unlock(); return false; }
    if (cache_valid && cached_path_revision == storage_path_revision(SETTINGS_PATH) &&
        cached_volume_revision == storage_volume_revision(STORAGE_FLASH) && !memcmp(values, cached, sizeof(cached)))
    { storage_unlock(); return true; }
    cache_valid = false;
    ui_settings_load_result_t result = read_record(current, &v1);
    bool ok = result != UI_SETTINGS_LOAD_IO_ERROR;
    if (ok && !(result == UI_SETTINGS_LOAD_OK && !v1 && !memcmp(values, current, sizeof(current))))
        ok = storage_file_replace(SETTINGS_PATH, record, sizeof(record));
    if (ok) cache_remember(values);
    storage_unlock();
    return ok;
}

static bool enqueue_locked(const uint8_t *values)
{
    if (!sleep_guard_acquire(&save_hold)) return false;
    bool wake = !queued && !writing;
    memcpy(pending, values, sizeof(pending));
    queued = true; failed = false;
    if (wake) rt_sem_release(&jobs_wake);
    return true;
}

bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT])
{
    uint8_t record[HEADER_SIZE + V2_SIZE];
    if (!encode_record(values, record) || !ui_settings_store_start()) return false;
    jobs_take();
    bool ok = !barrier && enqueue_locked(values);
    jobs_release();
    return ok;
}

bool ui_settings_store_save_sync(const uint8_t values[UI_SETTING_COUNT])
{
    uint8_t record[HEADER_SIZE + V2_SIZE];
    if (!encode_record(values, record) || !ui_settings_store_start()) return false;
    jobs_take();
    if (barrier) { jobs_release(); return false; }
    barrier = true;
    bool ok = enqueue_locked(values) && drain_locked();
    barrier = false;
    jobs_release();
    return ok;
}
