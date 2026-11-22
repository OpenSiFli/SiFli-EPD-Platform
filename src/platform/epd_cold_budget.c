/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_cold_budget.h"
#include "epd_memory.h"
#include "epd_shared_service.h"
#include <storage_file.h>
#include <dfs_posix.h>
#include <rtthread.h>
#include <rthw.h>
#include <rtm.h>
#include <mem_section.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#ifndef EPD_COLD_BUDGET_INDEX
#define EPD_COLD_BUDGET_INDEX STORAGE_FLASH_ROOT "/system/cold-budget.bin"
#endif
#define INDEX_MAGIC 0x43424754u
#define INDEX_VERSION 1u
#define DISK_FLOOR (2u * 1024u * 1024u)
typedef struct {
    char path[EPD_COLD_BUDGET_PATH];
    uint32_t bytes, kind, volume, age;
} record_t;
typedef struct {
    uint32_t magic, version, size, crc;
    record_t records[EPD_COLD_BUDGET_RECORDS];
} index_t;
typedef struct {
    uint32_t generation, session, reserved;
    bool active, deferred, writing;
} access_t;
typedef struct {
    index_t index;
    access_t access[EPD_COLD_BUDGET_RECORDS];
    struct rt_mutex lock;
    bool loaded, dirty;
    uint32_t serial, age, evicted[2];
} budget_state_t;
static budget_state_t *budget_state;
#define BUDGET_STARTING ((budget_state_t *)(uintptr_t)1)

static bool budget_ready(void)
{ return budget_state && budget_state != BUDGET_STARTING; }

static int cold_budget_init(void)
{
    if (budget_ready()) {
        rt_mutex_detach(&budget_state->lock);
        epd_app_free(budget_state);
    }
    budget_state = NULL;
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(cold_budget_init);

/* Allocate only on a real worker transaction, before taking either disk lock.
 * Concurrent first callers get BUSY; allocation failure permits a later retry. */
static int budget_start(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (budget_ready()) { rt_hw_interrupt_enable(level); return EPD_COLD_BUDGET_OK; }
    if (budget_state) { rt_hw_interrupt_enable(level); return EPD_COLD_BUDGET_BUSY; }
    budget_state = BUDGET_STARTING;
    rt_hw_interrupt_enable(level);
    epd_shared_service_guard_t guard = {0};
    budget_state_t *fresh = NULL;
    if (epd_shared_service_enter(&guard)) {
        fresh = epd_app_alloc(sizeof(*fresh), EPD_APP_PSRAM);
        if (fresh) {
            memset(fresh, 0, sizeof(*fresh));
            if (rt_mutex_init(&fresh->lock, "coldbud", RT_IPC_FLAG_PRIO) != RT_EOK) {
                epd_app_free(fresh); fresh = NULL;
            }
        }
        epd_shared_service_leave(&guard);
    }
    level = rt_hw_interrupt_disable(); budget_state = fresh; rt_hw_interrupt_enable(level);
    return fresh ? EPD_COLD_BUDGET_OK : EPD_COLD_BUDGET_IO;
}

bool epd_cold_budget_temporary(const char *path, char *out, size_t capacity)
{
    int n = snprintf(out, capacity, "%s.stage", path);
    return n > 0 && (size_t)n < capacity;
}
static int volume_of(const char *path)
{
    const char *root = storage_path_root(path);
    if (!root) return -1;
    if (!strcmp(root, STORAGE_FLASH_ROOT)) return STORAGE_FLASH;
    if (!strcmp(root, STORAGE_SD_ROOT)) return STORAGE_SD;
    return -1;
}
static bool flush_index(void)
{
    if (!budget_state->dirty) return true;
    budget_state->index.magic = INDEX_MAGIC; budget_state->index.version = INDEX_VERSION;
    budget_state->index.size = sizeof(budget_state->index.records);
    budget_state->index.crc = storage_crc32(budget_state->index.records, sizeof(budget_state->index.records));
    if (!storage_file_replace_session(EPD_COLD_BUDGET_INDEX, &budget_state->index, sizeof(budget_state->index), 0)) return false;
    budget_state->dirty = false; return true;
}
/* Stat only at first inventory or an actual disk transaction, never RAM hits. */
static bool file_bytes(const char *path, uint32_t *bytes)
{
    struct stat st;
    if (!stat(path, &st)) {
        if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) return false;
        *bytes = (uint32_t)st.st_size; return true;
    }
    if (errno != ENOENT && errno != -ENOENT) return false;
    *bytes = 0; return true;
}
static bool load_index(void)
{
    if (budget_state->loaded) return true;
    uint32_t bytes = 0;
    storage_lock();
    bool ok = storage_path_available(EPD_COLD_BUDGET_INDEX) &&
        storage_file_recover(EPD_COLD_BUDGET_INDEX) && file_bytes(EPD_COLD_BUDGET_INDEX, &bytes);
    storage_unlock();
    if (!ok) return false;
    if (bytes && !storage_record_load(EPD_COLD_BUDGET_INDEX, INDEX_MAGIC, INDEX_VERSION,
                                     budget_state->index.records, sizeof(budget_state->index.records))) return false;
    if (!bytes) memset(budget_state->index.records, 0, sizeof(budget_state->index.records));
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i) {
        record_t *r = &budget_state->index.records[i];
        if (!r->path[0]) continue;
        if (!memchr(r->path, 0, sizeof(r->path)) || r->kind > EPD_COLD_REQUIRED ||
            r->volume >= STORAGE_COUNT || volume_of(r->path) != (int)r->volume) return false;
        if (r->age > budget_state->age) budget_state->age = r->age;
        if (r->volume == STORAGE_SD) {
            /* A mounted path is not a persistent medium identity. Validate the
             * record again before allowing eviction after reboot/card changes. */
            r->kind = EPD_COLD_REQUIRED; budget_state->dirty = true;
            budget_state->access[i].session = storage_card_session();
        }
        /* Offline media stays accounted, but cannot be selected for deletion. */
        if (!storage_available((storage_volume_t)r->volume)) continue;
        char temporary[EPD_COLD_BUDGET_PATH + 8u];
        (void)epd_cold_budget_temporary(r->path, temporary, sizeof(temporary));
        storage_lock();
        ok = storage_file_recover(r->path) && file_bytes(r->path, &bytes);
        uint32_t staging = 0;
        if (ok) ok = file_bytes(temporary, &staging);
        if (ok && staging) {
            if (r->volume == STORAGE_SD) {
                /* No persisted card identity: account both files and leave the
                 * new medium untouched until its store explicitly acquires it. */
                if (staging > UINT32_MAX - bytes) ok = false;
                else bytes += staging;
            } else if (storage_file_writers(temporary) || unlink(temporary)) {
                budget_state->access[i].reserved = staging;
                budget_state->access[i].deferred = true;
            }
        }
        storage_unlock();
        if (!ok) return false;
        if (bytes != r->bytes) { r->bytes = bytes; budget_state->dirty = true; }
    }
    budget_state->loaded = true; return true;
}
static bool refresh(unsigned i, bool cleanup)
{
    record_t *r = &budget_state->index.records[i]; access_t *a = &budget_state->access[i];
    char temporary[EPD_COLD_BUDGET_PATH + 8u]; uint32_t bytes = 0, staging = 0;
    (void)epd_cold_budget_temporary(r->path, temporary, sizeof(temporary));
    storage_lock();
    bool ok = storage_session_valid((storage_volume_t)r->volume, a->session);
    if (ok && cleanup && (storage_file_writers(r->path) || storage_file_writers(temporary))) ok = false;
    if (ok) ok = file_bytes(r->path, &bytes) && file_bytes(temporary, &staging);
    if (ok && cleanup && staging && !unlink(temporary)) staging = 0;
    storage_unlock();
    if (!ok || staging) return false;
    if (r->bytes != bytes) { r->bytes = bytes; budget_state->dirty = true; }
    return true;
}
static void reap(void)
{
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i) {
        access_t *a = &budget_state->access[i];
        record_t *r = &budget_state->index.records[i];
        if (!r->path[0] || a->active) continue;
        if (r->volume == STORAGE_SD && !storage_session_valid(STORAGE_SD, a->session)) {
            char temporary[EPD_COLD_BUDGET_PATH + 8u];
            (void)epd_cold_budget_temporary(r->path, temporary, sizeof(temporary));
            storage_lock();
            bool held = storage_file_writers(r->path) || storage_file_writers(temporary);
            storage_unlock();
            if (!held) { memset(r, 0, sizeof(*r)); memset(a, 0, sizeof(*a)); budget_state->dirty = true; }
        } else if (a->deferred && refresh(i, true)) { a->deferred = false; a->reserved = 0; }
    }
}
static size_t usage(unsigned volume, bool reservations)
{
    /* Include the index and its atomic replacement, not just payload files. */
    size_t bytes = volume == STORAGE_FLASH ? 2u * sizeof(budget_state->index) : 0;
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i)
        if (budget_state->index.records[i].path[0] && budget_state->index.records[i].volume == volume)
            bytes += budget_state->index.records[i].bytes + (reservations ? budget_state->access[i].reserved : 0u);
    return bytes;
}
static bool room(unsigned volume, size_t bytes, epd_cold_class_t kind)
{
    size_t used = usage(volume, true);
    storage_info_t info;
    if (bytes > EPD_COLD_BUDGET_VOLUME_BYTES || used > EPD_COLD_BUDGET_VOLUME_BYTES - bytes ||
        !storage_info((storage_volume_t)volume, &info) || info.used > info.total) return false;
    uint64_t pending = bytes;
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i)
        if (budget_state->index.records[i].volume == volume) pending += budget_state->access[i].reserved;
    if (kind != EPD_COLD_REQUIRED) pending += DISK_FLOOR;
    return pending <= info.total - info.used;
}
static unsigned evict_one(unsigned volume, unsigned except, epd_cold_class_t kind)
{
    bool tried[EPD_COLD_BUDGET_RECORDS] = {false};
    for (unsigned attempt = 0; attempt < EPD_COLD_BUDGET_RECORDS; ++attempt) {
        unsigned victim = EPD_COLD_BUDGET_RECORDS;
        for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i) {
            record_t *r = &budget_state->index.records[i]; access_t *a = &budget_state->access[i];
            if (i == except || tried[i] || !r->bytes || r->volume != volume ||
                a->active || a->deferred || r->kind == EPD_COLD_REQUIRED || r->kind > (unsigned)kind) continue;
            if (victim == EPD_COLD_BUDGET_RECORDS || r->kind < budget_state->index.records[victim].kind ||
                (r->kind == budget_state->index.records[victim].kind && r->age < budget_state->index.records[victim].age)) victim = i;
        }
        if (victim == EPD_COLD_BUDGET_RECORDS) return victim;
        tried[victim] = true;
        record_t *r = &budget_state->index.records[victim];
        storage_lock();
        bool ok = storage_path_available(r->path) && !storage_file_writers(r->path) &&
            (unlink(r->path) == 0 || errno == ENOENT || errno == -ENOENT);
        storage_unlock();
        if (!ok) continue;
        ++budget_state->evicted[r->kind]; r->bytes = 0; budget_state->dirty = true;
        return victim;
    }
    return EPD_COLD_BUDGET_RECORDS;
}
static bool reclaim(unsigned volume, unsigned except, size_t bytes, epd_cold_class_t kind)
{
    while (!room(volume, bytes, kind))
        if (evict_one(volume, except, kind) == EPD_COLD_BUDGET_RECORDS) return false;
    return true;
}

int epd_cold_budget_begin(const char *path, epd_cold_class_t kind, size_t bytes,
                          uint32_t session, epd_cold_lease_t *out)
{
    if (out) *out = 0;
    if (!out || !path || !*path || strlen(path) >= EPD_COLD_BUDGET_PATH ||
        kind > EPD_COLD_REQUIRED || bytes > UINT32_MAX) return EPD_COLD_BUDGET_INVALID;
    int volume = volume_of(path), result = EPD_COLD_BUDGET_IO;
    if (volume < 0 || !storage_session_valid((storage_volume_t)volume, session)) return result;
    size_t index_overhead = volume == STORAGE_FLASH ? 2u * sizeof(index_t) : 0;
    if (bytes && (index_overhead > EPD_COLD_BUDGET_VOLUME_BYTES ||
                  bytes > EPD_COLD_BUDGET_VOLUME_BYTES - index_overhead))
        return EPD_COLD_BUDGET_FULL;
    result = budget_start();
    if (result != EPD_COLD_BUDGET_OK) return result;
    result = EPD_COLD_BUDGET_IO;
    rt_mutex_take(&budget_state->lock, RT_WAITING_FOREVER);
    if (!load_index()) goto done;
    reap();
    unsigned slot = EPD_COLD_BUDGET_RECORDS, spare = slot;
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i) {
        if (!strcmp(budget_state->index.records[i].path, path)) { slot = i; break; }
        if (!budget_state->index.records[i].bytes && !budget_state->access[i].active && !budget_state->access[i].deferred) spare = i;
    }
    if (slot == EPD_COLD_BUDGET_RECORDS) slot = spare;
    /* The index is a bound as well as the byte quota. Reuse an idle derived
     * record under the same eviction rules instead of permanently exhausting
     * registration after visiting more cache keys than fit in the index. */
    if (slot == EPD_COLD_BUDGET_RECORDS && budget_state->serial != UINT32_MAX)
        slot = evict_one((unsigned)volume, EPD_COLD_BUDGET_RECORDS, kind);
    if (slot == EPD_COLD_BUDGET_RECORDS || budget_state->serial == UINT32_MAX) { result = EPD_COLD_BUDGET_FULL; goto done; }
    access_t *a = &budget_state->access[slot]; record_t *r = &budget_state->index.records[slot];
    if (a->active || a->deferred) { result = EPD_COLD_BUDGET_BUSY; goto done; }
    if (strcmp(r->path, path)) {
        memset(r, 0, sizeof(*r)); strcpy(r->path, path);
        r->volume = (unsigned)volume;
        r->kind = kind == EPD_COLD_DERIVED ? EPD_COLD_DERIVED : EPD_COLD_REQUIRED;
        budget_state->dirty = true;
    }
    a->session = session;
    if (!refresh(slot, true)) goto done;
    if (bytes && !reclaim((unsigned)volume, slot, bytes, kind)) { result = EPD_COLD_BUDGET_FULL; goto done; }
    /* Upgrade before publication: a reboot during OPTIONAL -> REQUIRED cannot
     * leave the new unique file labelled as an eviction candidate. */
    if (bytes && (unsigned)kind > r->kind) { r->kind = (unsigned)kind; budget_state->dirty = true; }
    if (!flush_index()) goto done;
    a->active = true; a->writing = bytes != 0; a->reserved = (uint32_t)bytes;
    a->generation = ++budget_state->serial; r->age = ++budget_state->age;
    *out = ((uint64_t)a->generation << 32) | (slot + 1u);
    result = EPD_COLD_BUDGET_OK;
done:
    rt_mutex_release(&budget_state->lock); return result;
}
int epd_cold_budget_finish(epd_cold_lease_t lease, epd_cold_class_t kind, bool success)
{
    unsigned slot = (uint32_t)lease;
    if (!budget_ready() || !slot || slot > EPD_COLD_BUDGET_RECORDS || kind > EPD_COLD_REQUIRED)
        return EPD_COLD_BUDGET_INVALID;
    --slot; rt_mutex_take(&budget_state->lock, RT_WAITING_FOREVER);
    access_t *a = &budget_state->access[slot]; record_t *r = &budget_state->index.records[slot];
    int result = EPD_COLD_BUDGET_INVALID;
    if (!a->active || a->generation != (uint32_t)(lease >> 32)) goto done;
    a->active = false;
    if (!refresh(slot, a->writing)) { a->deferred = true; result = EPD_COLD_BUDGET_IO; goto done; }
    a->reserved = 0;
    if (success && r->kind != (unsigned)kind) { r->kind = (unsigned)kind; budget_state->dirty = true; }
    result = flush_index() ? EPD_COLD_BUDGET_OK : EPD_COLD_BUDGET_IO;
done:
    rt_mutex_release(&budget_state->lock); return result;
}
void epd_cold_budget_snapshot(unsigned volume, epd_cold_budget_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (volume >= STORAGE_COUNT) return;
    out->index_bytes = volume == STORAGE_FLASH ? 2u * sizeof(index_t) : 0;
    out->bytes = out->index_bytes; out->limit = EPD_COLD_BUDGET_VOLUME_BYTES;
    if (!budget_ready()) return;
    rt_mutex_take(&budget_state->lock, RT_WAITING_FOREVER);
    out->index_bytes = volume == STORAGE_FLASH ? 2u * sizeof(budget_state->index) : 0;
    out->bytes = usage(volume, false); out->limit = EPD_COLD_BUDGET_VOLUME_BYTES;
    for (unsigned i = 0; i < EPD_COLD_BUDGET_RECORDS; ++i) {
        if (!budget_state->index.records[i].path[0] || budget_state->index.records[i].volume != volume) continue;
        ++out->records; out->reserved += budget_state->access[i].reserved;
        out->active += budget_state->access[i].active; out->deferred += budget_state->access[i].deferred;
    }
    out->evicted_derived = budget_state->evicted[0]; out->evicted_optional = budget_state->evicted[1];
    rt_mutex_release(&budget_state->lock);
}
RTM_EXPORT(epd_cold_budget_begin);
RTM_EXPORT(epd_cold_budget_finish);
RTM_EXPORT(epd_cold_budget_temporary);
RTM_EXPORT(epd_cold_budget_snapshot);
