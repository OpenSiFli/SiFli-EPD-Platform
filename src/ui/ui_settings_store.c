#include "ui_settings_internal.h"
#include "ui_settings_store.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "sleep_guard.h"
#include "epd_memory.h"
#include "epd_operation_runtime.h"
#include "epd_shared_service.h"
#include "platform/epd_power.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define SETTINGS_PATH STORAGE_FLASH_ROOT "/system/settings.bin"
#define SETTINGS_MAGIC 0x45505332u
#define SETTINGS_VERSION 4u
#define HEADER_SIZE 16u
#define REQUIRED_FIELDS 15u
#define FIELD_COUNT 16u
#define PAYLOAD_SIZE (FIELD_COUNT * 4u)
#define PAYLOAD_MAX_SIZE PAYLOAD_SIZE

static struct rt_mutex jobs_lock;
static struct rt_semaphore jobs_done;
static epd_operation_scope_t save_scope, cleanup_scope;
static epd_operation_executor_t save_executor;
static bool started, queued, writing, barrier, failed;
static struct { uint8_t pending[UI_SETTING_COUNT], cached[UI_SETTING_COUNT]; } values_store;
static uint32_t result_revision;
static sleep_guard_owner_t save_hold = {.id = "ui-settings"};

/* Only the worker or a drained synchronous reload accesses this cache, with
 * storage_lock held. No queue mutex is held during filesystem operations. */
static bool cache_valid;
static uint32_t cached_path_revision, cached_volume_revision;
static bool save_record(const uint8_t *values);

static void jobs_take(void) { rt_mutex_take(&jobs_lock, RT_WAITING_FOREVER); }
static void jobs_release(void) { rt_mutex_release(&jobs_lock); }

static void save_operation(epd_operation_t operation, void *unused)
{
    (void)unused;
    uint8_t values[UI_SETTING_COUNT];
    jobs_take();
    memcpy(values, values_store.pending, sizeof(values));
    queued = false; writing = true;
    jobs_release();
    bool ok = save_record(values);
    jobs_take();
    writing = false;
    bool again = queued;
    /* Only the latest accepted snapshot controls the public save result. */
    if (!again) { failed = !ok; ++result_revision; sleep_guard_release(&save_hold); }
    if (barrier) rt_sem_release(&jobs_done);
    jobs_release();
    epd_power_notify();
    if (again) (void)epd_operation_yield(operation);
    else (void)epd_operation_complete(operation, ok ? EPD_OPERATION_OK : EPD_OPERATION_FAILED, ok ? 0 : -1, NULL);
}

static bool save_received(epd_operation_t operation, const epd_operation_snapshot_t *snapshot,
                          void *result, void *unused)
{
    (void)operation; (void)snapshot; (void)result; (void)unused;
    return true;
}

bool ui_settings_store_start(void)
{
    if (started) return true;
    if (!epd_operation_runtime_init()) return false;
    epd_operation_executor_t deliver, cleanup_executor;
    if (!epd_operation_runtime_executors(&save_executor, &deliver, &cleanup_executor)) return false;
    epd_shared_service_guard_t guard = {0};
    if (!epd_shared_service_enter(&guard)) return false;
    if (rt_mutex_init(&jobs_lock, "uiset", RT_IPC_FLAG_PRIO) != RT_EOK) goto guard_failed;
    if (rt_sem_init(&jobs_done, "uisetd", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto mutex_failed;
    if (!epd_operation_scope_open(EPD_MEMORY_OWNER_SYSTEM, 0, &save_scope)) goto done_failed;
    if (!epd_operation_scope_open(EPD_MEMORY_OWNER_SYSTEM, 0, &cleanup_scope)) goto scope_failed;
    started = true;
    epd_shared_service_leave(&guard);
    return true;
scope_failed:
    epd_operation_scope_close(save_scope);
    epd_operation_scope_release(save_scope);
done_failed:
    rt_sem_detach(&jobs_done);
mutex_failed:
    rt_mutex_detach(&jobs_lock);
guard_failed:
    epd_shared_service_leave(&guard);
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
    memcpy(values_store.cached, values, sizeof(values_store.cached));
    cached_path_revision = storage_path_revision(SETTINGS_PATH);
    cached_volume_revision = storage_volume_revision(STORAGE_FLASH);
    cache_valid = !storage_file_writers(SETTINGS_PATH);
}

typedef struct
{
    ui_setting_id_t id;
    uint16_t tag;
    unsigned count;
    uint16_t options[UI_READER_APPEARANCE_COUNT];
} setting_field_t;

/* Little-endian (u16 field tag, u16 semantic value) pairs. Tags and
 * values are independent of enum ordinals and page order. Time 0 means never;
 * scale uses percent and font size uses pixels. Line spacing and margin retain
 * their original wire values as preset IDs across layout revisions. */
static const setting_field_t fields[FIELD_COUNT] = {
    {UI_SETTING_TOUCH,        1, 2, {0, 1}},
    {UI_SETTING_TIMEOUT,      2, 6, {1, 3, 5, 10, 15, 0}},
    {UI_SETTING_FULL_REFRESH, 3, 9, {UI_FULL_REFRESH_COUNTS}},
    {UI_SETTING_BLUETOOTH,    4, 2, {0, 1}},
    {UI_SETTING_WIFI,         5, 2, {0, 1}},
    {UI_SETTING_LOW_POWER,    7, 2, {0, 1}},
    {UI_SETTING_FONT,         8, 5, {0, 1, 2, 3, 4}},
    {UI_SETTING_FONT_SIZE,    9, 8, {UI_READING_FONT_SIZES}},
    {UI_SETTING_FONT_WEIGHT, 10, 3, {0, 1, 2}},
    {UI_SETTING_LINE_SPACING,11, 5, {100, 120, 140, 160, 180}},
    {UI_SETTING_MARGIN,      12, 5, {4, 8, 12, 16, 20}},
    {UI_SETTING_ENCODING,    13, 3, {0, 1, 2}},
    {UI_SETTING_UI_SCALE,    14, 5, {90, 95, 100, 110, 120}},
    /* Tag 15 is retired; system sleep keeps its existing wire ID. */
    {UI_SETTING_SYSTEM_SLEEP,16, 6, {1, 5, 15, 30, 60, 0}},
    {UI_SETTING_NAVIGATION,  17, 2, {0, 1}},
    {UI_SETTING_READER_APPEARANCE, 18, UI_READER_APPEARANCE_COUNT,
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}}
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
    if (field->id == UI_SETTING_READER_APPEARANCE && !ui_reader_appearance_valid(value))
        return false;
    for (unsigned i = 0; i < field->count; ++i)
        if (field->options[i] == value)
        {
            values[field->id] = (uint8_t)i;
            return true;
        }
    return false;
}

static bool decode_payload(const uint8_t *payload, unsigned size, uint8_t *values)
{
    default_values(values);
    bool seen[FIELD_COUNT] = {false};
    for (unsigned i = 0; i < size / 4u; ++i)
    {
        unsigned tag = get_u16(payload + i * 4);
        unsigned j;
        for (j = 0; j < FIELD_COUNT; ++j)
            if (fields[j].tag == tag) break;
        if (j == FIELD_COUNT || seen[j] ||
            !decode_value(&fields[j], get_u16(payload + i * 4 + 2), values)) return false;
        seen[j] = true;
    }
    for (unsigned i = 0; i < REQUIRED_FIELDS; ++i)
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

static ui_settings_load_result_t read_record(uint8_t *values)
{
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
    uint8_t header[HEADER_SIZE], payload[PAYLOAD_MAX_SIZE];
    struct stat st;
    ui_settings_load_result_t result = UI_SETTINGS_LOAD_INVALID;
    if (fstat(fd, &st) != 0) result = UI_SETTINGS_LOAD_IO_ERROR;
    else if (S_ISREG(st.st_mode) && st.st_size >= HEADER_SIZE)
    {
        if (!read_exact(fd, header, sizeof(header))) result = UI_SETTINGS_LOAD_IO_ERROR;
        else if (get_u32(header) == SETTINGS_MAGIC)
        {
            uint32_t version = get_u32(header + 4);
            if (version != SETTINGS_VERSION) result = UI_SETTINGS_LOAD_UNKNOWN_VERSION;
            else
            {
                unsigned size = get_u32(header + 8);
                bool valid_size = size == PAYLOAD_SIZE;
                if (valid_size && st.st_size == HEADER_SIZE + size)
                {
                    if (!read_exact(fd, payload, size)) result = UI_SETTINGS_LOAD_IO_ERROR;
                    else if (get_u32(header + 12) == storage_crc32(payload, size) &&
                             decode_payload(payload, size, values))
                    {
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

static bool encode_record(const uint8_t *values, uint8_t record[HEADER_SIZE + PAYLOAD_SIZE])
{
    if (!values) return false;
    for (unsigned i = 0; i < FIELD_COUNT; ++i)
    {
        const setting_field_t *field = &fields[i];
        unsigned index = values[field->id];
        if (index >= field->count ||
            (field->id == UI_SETTING_READER_APPEARANCE && !ui_reader_appearance_valid(index)))
            return false;
        uint8_t *p = record + HEADER_SIZE + i * 4;
        put_u16(p, field->tag);
        put_u16(p + 2, field->options[index]);
    }
    put_u32(record, SETTINGS_MAGIC);
    put_u32(record + 4, SETTINGS_VERSION);
    put_u32(record + 8, PAYLOAD_SIZE);
    put_u32(record + 12, storage_crc32(record + HEADER_SIZE, PAYLOAD_SIZE));
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
    storage_lock();
    cache_valid = false;
    ui_settings_load_result_t result = read_record(candidate);
    if (result == UI_SETTINGS_LOAD_OK) cache_remember(candidate);
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
    if (result != UI_SETTINGS_LOAD_OK) return false;
    memcpy(values, candidate, sizeof(candidate));
    return true;
}

static bool save_record(const uint8_t *values)
{
    uint8_t record[HEADER_SIZE + PAYLOAD_SIZE], current[UI_SETTING_COUNT];
    if (!encode_record(values, record)) return false;
    storage_lock();
    if (!system_reset_write_allowed() || !storage_path_available(SETTINGS_PATH) || storage_file_writers(SETTINGS_PATH))
    { cache_valid = false; storage_unlock(); return false; }
    if (cache_valid && cached_path_revision == storage_path_revision(SETTINGS_PATH) &&
        cached_volume_revision == storage_volume_revision(STORAGE_FLASH) && !memcmp(values, values_store.cached, sizeof(values_store.cached)))
    { storage_unlock(); return true; }
    cache_valid = false;
    ui_settings_load_result_t result = read_record(current);
    bool ok = result != UI_SETTINGS_LOAD_IO_ERROR;
    if (ok && !(result == UI_SETTINGS_LOAD_OK && !memcmp(values, current, sizeof(current))))
        ok = storage_file_replace(SETTINGS_PATH, record, sizeof(record));
    if (ok) cache_remember(values);
    storage_unlock();
    return ok;
}

static bool enqueue_locked(const uint8_t *values)
{
    if (!sleep_guard_acquire(&save_hold)) return false;
    if (!queued && !writing) {
        epd_operation_spec_t spec = {0};
        spec.scope = save_scope;
        spec.cleanup_scope = cleanup_scope;
        spec.execute = spec.deliver = spec.cleanup = save_executor;
        spec.run = save_operation;
        spec.receive = save_received;
        epd_operation_t operation = 0;
        epd_shared_service_guard_t guard = {0};
        bool entered = epd_shared_service_enter(&guard);
        bool submitted = entered && epd_operation_submit(&spec, &operation);
        if (entered) epd_shared_service_leave(&guard);
        if (!submitted) { sleep_guard_release(&save_hold); return false; }
        /* Completion lives in the resident save status; no observer handle is
         * needed. The manager retains execution and cleanup until they finish. */
        epd_operation_release(operation);
    }
    memcpy(values_store.pending, values, sizeof(values_store.pending));
    queued = true; failed = false;
    return true;
}

bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT])
{
    uint8_t record[HEADER_SIZE + PAYLOAD_SIZE];
    if (!encode_record(values, record) || !ui_settings_store_start()) return false;
    jobs_take();
    bool ok = !barrier && enqueue_locked(values);
    jobs_release();
    return ok;
}

bool ui_settings_store_save_sync(const uint8_t values[UI_SETTING_COUNT])
{
    uint8_t record[HEADER_SIZE + PAYLOAD_SIZE];
    if (!encode_record(values, record) || !ui_settings_store_start()) return false;
    jobs_take();
    if (barrier) { jobs_release(); return false; }
    barrier = true;
    bool ok = enqueue_locked(values) && drain_locked();
    barrier = false;
    jobs_release();
    return ok;
}
