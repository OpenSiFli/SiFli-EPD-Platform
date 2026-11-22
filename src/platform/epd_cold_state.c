/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_cold_state.h"
#include "epd_cold_budget.h"
#include "epd_memory.h"
#include "platform/epd_power.h"
#include <storage_file.h>
#include <dfs_posix.h>
#include <rthw.h>
#include <rtm.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <mem_section.h>

#ifndef EPD_COLD_STATE_ROOT
#define EPD_COLD_STATE_ROOT STORAGE_FLASH_ROOT "/system/cold_state"
#endif
#define COLD_MAGIC 0x434f4c44u
#define COLD_WIRE_VERSION 1u
#define COLD_OPTIONAL 1u

/* Explicit uint32 fields and byte arrays; disk data never contains a pointer.
 * The first four fields match storage_record_save/load's record envelope. */
typedef struct {
    char key[EPD_COLD_STATE_KEY_MAX + 1u];
    uint32_t format_version, revision, instance_generation, length, flags;
    uint8_t bytes[EPD_COLD_STATE_BYTES_MAX];
} payload_t;
typedef struct {
    uint32_t magic, version, size, crc;
    payload_t payload;
} wire_t;
typedef struct {
    epd_cold_state_t id;
    epd_memory_owner_t owner;
    wire_t *wire;
    epd_cold_state_snapshot_t view;
    uint64_t sequence;
    bool preparing, copying;
} job_t;
L2_RET_BSS_SECT_BEGIN(epd_cold_jobs)
static job_t jobs[EPD_COLD_STATE_JOBS] L2_RET_BSS_SECT(epd_cold_jobs);
L2_RET_BSS_SECT_END
static epd_operation_executor_t worker;
static bool ready, dispatching;
static uint32_t next_id;
static uint64_t next_sequence;

static int cold_metadata_init(void)
{
    memset(jobs, 0, sizeof(jobs));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(cold_metadata_init);

static job_t *find(epd_cold_state_t id)
{
    if (id) for (unsigned i = 0; i < EPD_COLD_STATE_JOBS; ++i)
        if (jobs[i].id == id) return &jobs[i];
    return NULL;
}
static void notify(void)
{
    if (worker.wake && worker.wake_bits) rt_event_send(worker.wake, worker.wake_bits);
}
bool epd_cold_state_init(const epd_operation_executor_t *executor)
{
    if (!executor || !executor->thread || (executor->wake && !executor->wake_bits)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool ok = !ready || (worker.thread == executor->thread && worker.wake == executor->wake &&
                        worker.wake_bits == executor->wake_bits);
    if (!ready) { worker = *executor; ready = true; }
    rt_hw_interrupt_enable(level);
    return ok;
}
static bool submit(const epd_cold_state_spec_t *spec, const void *data, size_t size,
                   bool saving, epd_cold_state_t *out)
{
    if (!spec || !out || !spec->key || !spec->format_version ||
        (saving && !spec->revision) || (!data && size) || size > EPD_COLD_STATE_BYTES_MAX) return false;
    size_t length = 0;
    while (length <= EPD_COLD_STATE_KEY_MAX && spec->key[length]) ++length;
    if (!length || length > EPD_COLD_STATE_KEY_MAX) return false;
    if (!epd_memory_owner_retain(spec->cleanup_owner)) return false;
    job_t *job = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    epd_memory_owner_snapshot_t owner_snapshot;
    if (ready && epd_memory_owner_get(spec->cleanup_owner, &owner_snapshot) && !owner_snapshot.closed &&
        next_id != UINT32_MAX && next_sequence != UINT64_MAX)
        for (unsigned i = 0; i < EPD_COLD_STATE_JOBS; ++i) if (!jobs[i].id) {
            job = &jobs[i];
            memset(job, 0, sizeof(*job));
            job->id = ++next_id;
            job->sequence = ++next_sequence;
            job->preparing = true;
            job->owner = spec->cleanup_owner;
            break;
        }
    rt_hw_interrupt_enable(level);
    if (!job) { epd_memory_owner_release_trusted(spec->cleanup_owner); return false; }

    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(spec->cleanup_owner);
    wire_t *wire = previous != EPD_MEMORY_OWNER_INVALID ? epd_app_alloc(sizeof(*wire), EPD_APP_PSRAM) : NULL;
    if (wire) {
        memset(wire, 0, sizeof(*wire));
        memcpy(wire->payload.key, spec->key, length);
        wire->payload.format_version = spec->format_version;
        wire->payload.revision = spec->revision;
        wire->payload.instance_generation = spec->instance_generation;
        wire->payload.flags = spec->optional ? COLD_OPTIONAL : 0;
        wire->payload.length = (uint32_t)size;
        if (size) memcpy(wire->payload.bytes, data, size);
    }
    if (previous != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_leave_trusted(previous);
    level = rt_hw_interrupt_disable();
    if (wire) {
        job->wire = wire;
        job->view.state = EPD_COLD_STATE_QUEUED;
        job->view.saving = saving;
        job->view.instance_generation = spec->instance_generation;
        job->view.format_version = spec->format_version;
        job->view.revision = spec->revision;
        job->view.size = size;
        job->preparing = false;
        *out = job->id;
    } else job->id = 0;
    rt_hw_interrupt_enable(level);
    if (!wire) epd_memory_owner_release_trusted(spec->cleanup_owner);
    else notify();
    return wire != NULL;
}
bool epd_cold_state_put(const epd_cold_state_spec_t *spec, const void *data, size_t size,
                        epd_cold_state_t *out)
{
    return submit(spec, data, size, true, out);
}
bool epd_cold_state_load(const epd_cold_state_spec_t *spec, epd_cold_state_t *out)
{
    return submit(spec, NULL, 0, false, out);
}
bool epd_cold_state_get(epd_cold_state_t id, epd_cold_state_snapshot_t *out)
{
    if (!out) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    job_t *job = find(id);
    bool ok = job && !job->preparing;
    if (ok) *out = job->view;
    rt_hw_interrupt_enable(level);
    return ok;
}
bool epd_cold_state_copy(epd_cold_state_t id, void *out, size_t capacity, size_t *size)
{
    if (!size) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    job_t *job = find(id);
    bool ok = job && !job->preparing && !job->copying && job->view.state != EPD_COLD_STATE_WORKING &&
              (job->view.saving || job->view.state == EPD_COLD_STATE_LOADED) &&
              capacity >= job->view.size && (out || !job->view.size);
    size_t length = ok ? job->view.size : 0;
    if (ok) job->copying = true;
    rt_hw_interrupt_enable(level);
    if (!ok) return false;
    if (length) memcpy(out, job->wire->payload.bytes, length);
    *size = length;
    level = rt_hw_interrupt_disable();
    job->copying = false;
    rt_hw_interrupt_enable(level);
    notify();
    return true;
}
bool epd_cold_state_retry(epd_cold_state_t id)
{
    rt_base_t level = rt_hw_interrupt_disable();
    job_t *job = find(id);
    bool ok = job && !job->preparing && !job->copying &&
              job->view.state == EPD_COLD_STATE_FAILED && next_sequence != UINT64_MAX;
    if (ok) { job->view.state = EPD_COLD_STATE_QUEUED; job->sequence = ++next_sequence; }
    rt_hw_interrupt_enable(level);
    if (ok) notify();
    return ok;
}
static bool retire(epd_cold_state_t id, bool abandon)
{
    wire_t *wire = NULL;
    epd_memory_owner_t owner = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    job_t *job = find(id);
    bool idle = job && !job->preparing && !job->copying && job->view.state != EPD_COLD_STATE_WORKING;
    bool ok = idle && (abandon ? (job->view.saving && (job->wire->payload.flags & COLD_OPTIONAL)) :
              (job->view.state == EPD_COLD_STATE_SAVED || job->view.state == EPD_COLD_STATE_LOADED ||
               (!job->view.saving && job->view.state == EPD_COLD_STATE_FAILED)));
    if (ok) { wire = job->wire; owner = job->owner; job->preparing = true; }
    rt_hw_interrupt_enable(level);
    if (ok) {
        epd_app_free(wire);
        epd_memory_owner_release_trusted(owner);
        level = rt_hw_interrupt_disable();
        job->id = 0;
        rt_hw_interrupt_enable(level);
    }
    return ok;
}
bool epd_cold_state_release(epd_cold_state_t id) { return retire(id, false); }
bool epd_cold_state_abandon(epd_cold_state_t id) { return retire(id, true); }

bool epd_cold_state_take(epd_cold_state_t id, epd_cold_state_blob_t *out)
{
    if (!out) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    job_t *job = find(id);
    bool ok = job && !job->preparing && !job->copying && job->view.saving &&
              job->view.state == EPD_COLD_STATE_FAILED;
    if (ok) {
        out->allocation = job->wire;
        out->data = job->wire->payload.bytes;
        out->size = job->view.size;
        out->owner = job->owner;
        job->id = 0;
    }
    rt_hw_interrupt_enable(level);
    return ok;
}
void epd_cold_state_blob_release(epd_cold_state_blob_t *blob)
{
    if (!blob || !blob->allocation) return;
    void *allocation = blob->allocation;
    epd_memory_owner_t owner = blob->owner;
    memset(blob, 0, sizeof(*blob));
    epd_app_free(allocation);
    epd_memory_owner_release_trusted(owner);
}

static bool path_for(unsigned slot, char *path, size_t size)
{
    int count = snprintf(path, size, EPD_COLD_STATE_ROOT "/%02u.bin", slot);
    return count > 0 && (size_t)count < size;
}
/* -1 is unavailable, 0 missing, 1 exists. Recover before testing existence. */
static int present(const char *path)
{
    struct stat info;
    storage_lock();
    int result = -1;
    if (storage_path_available(path) && storage_file_recover(path)) {
        if (!stat(path, &info)) result = 1;
        else if (errno == ENOENT || errno == -ENOENT) result = 0;
    }
    storage_unlock();
    return result;
}
static bool valid(const payload_t *payload)
{
    return payload->key[0] && memchr(payload->key, 0, sizeof(payload->key)) &&
           payload->format_version && payload->revision && payload->length <= EPD_COLD_STATE_BYTES_MAX &&
           !(payload->flags & ~COLD_OPTIONAL);
}
static epd_cold_state_error_t execute(job_t *job)
{
    payload_t stored;
    char path[STORAGE_PATH_MAX];
    int match = -1, free_slot = -1, optional_slot = -1;
    bool corrupt = false;
    /* Serialized worker access prevents a lower revision committing after a
     * higher one. Re-read disk on every attempt, including reboot/retry. */
    for (unsigned i = 0; i < EPD_COLD_STATE_SLOTS; ++i) {
        if (!path_for(i, path, sizeof(path))) return EPD_COLD_STATE_FORMAT;
        int exists = present(path);
        if (exists < 0) return EPD_COLD_STATE_IO;
        if (!exists) { if (free_slot < 0) free_slot = (int)i; continue; }
        epd_cold_lease_t lease;
        int admission = epd_cold_budget_begin(path, EPD_COLD_REQUIRED, 0, 0, &lease);
        if (admission != EPD_COLD_BUDGET_OK) return EPD_COLD_STATE_IO;
        bool read_ok = storage_record_load(path, COLD_MAGIC, COLD_WIRE_VERSION, &stored, sizeof(stored)) && valid(&stored);
        (void)epd_cold_budget_finish(lease, read_ok && (stored.flags & COLD_OPTIONAL) ?
                                   EPD_COLD_OPTIONAL : EPD_COLD_REQUIRED, read_ok);
        if (!read_ok) {
            corrupt = true;
            continue;
        }
        if (!strcmp(stored.key, job->wire->payload.key)) {
            match = (int)i;
            if (!job->view.saving) {
                if (stored.format_version != job->view.format_version) return EPD_COLD_STATE_FORMAT;
                job->wire->payload = stored;
                return EPD_COLD_STATE_OK;
            }
            if (stored.revision > job->view.revision) return EPD_COLD_STATE_STALE;
            if (stored.revision == job->view.revision) {
                /* Retry of an already committed write is idempotent. Equal
                 * revisions with different content are a conflict, not updates. */
                const payload_t *next = &job->wire->payload;
                if (stored.format_version != next->format_version || stored.length != next->length ||
                    stored.flags != next->flags || memcmp(stored.bytes, next->bytes, stored.length))
                    return EPD_COLD_STATE_STALE;
                return EPD_COLD_STATE_OK;
            }
            break;
        }
        if ((stored.flags & COLD_OPTIONAL) && optional_slot < 0) optional_slot = (int)i;
    }
    if (!job->view.saving) return corrupt ? EPD_COLD_STATE_FORMAT : EPD_COLD_STATE_NOT_FOUND;
    /* An unreadable record might be this key's newer revision. Preserve it and
     * the dirty job instead of creating an ambiguous duplicate elsewhere. */
    if (match < 0 && corrupt) return EPD_COLD_STATE_FORMAT;
    int slot = match >= 0 ? match : free_slot >= 0 ? free_slot : optional_slot;
    if (slot < 0) return EPD_COLD_STATE_FULL;
    if (!path_for((unsigned)slot, path, sizeof(path))) return EPD_COLD_STATE_FORMAT;
    wire_t *wire = job->wire;
    wire->magic = COLD_MAGIC;
    wire->version = COLD_WIRE_VERSION;
    wire->size = sizeof(wire->payload);
    wire->crc = storage_crc32(&wire->payload, sizeof(wire->payload));
    epd_cold_class_t kind = (wire->payload.flags & COLD_OPTIONAL) ? EPD_COLD_OPTIONAL : EPD_COLD_REQUIRED;
    epd_cold_lease_t lease;
    int admission = epd_cold_budget_begin(path, kind, sizeof(*wire), 0, &lease);
    if (admission != EPD_COLD_BUDGET_OK)
        return admission == EPD_COLD_BUDGET_FULL ? EPD_COLD_STATE_FULL : EPD_COLD_STATE_IO;
    bool saved = storage_file_replace_session(path, wire, sizeof(*wire), 0);
    /* The payload commit remains authoritative if index publication fails;
     * conservative REQUIRED metadata survives until the next successful sync. */
    (void)epd_cold_budget_finish(lease, kind, saved);
    return saved ? EPD_COLD_STATE_OK : EPD_COLD_STATE_IO;
}
size_t epd_cold_state_dispatch(size_t budget)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!ready || worker.thread != rt_thread_self() || dispatching) {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    dispatching = true;
    rt_hw_interrupt_enable(level);
    size_t count = 0;
    while (count < budget) {
        level = rt_hw_interrupt_disable();
        job_t *job = NULL;
        for (unsigned i = 0; i < EPD_COLD_STATE_JOBS; ++i) {
            job_t *candidate = &jobs[i];
            if (candidate->id && !candidate->preparing && !candidate->copying &&
                candidate->view.state == EPD_COLD_STATE_QUEUED &&
                (!job || candidate->sequence < job->sequence)) job = candidate;
        }
        if (!job) { rt_hw_interrupt_enable(level); break; }
        job->view.state = EPD_COLD_STATE_WORKING;
        rt_hw_interrupt_enable(level);
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(job->owner);
        epd_cold_state_error_t error = previous != EPD_MEMORY_OWNER_INVALID ? execute(job) : EPD_COLD_STATE_IO;
        if (previous != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_leave_trusted(previous);
        level = rt_hw_interrupt_disable();
        job->view.error = error;
        job->view.retry_required = job->view.saving && error != EPD_COLD_STATE_OK;
        job->view.state = error != EPD_COLD_STATE_OK ? EPD_COLD_STATE_FAILED :
                          job->view.saving ? EPD_COLD_STATE_SAVED : EPD_COLD_STATE_LOADED;
        if (!error && !job->view.saving) {
            job->view.revision = job->wire->payload.revision;
            job->view.size = job->wire->payload.length;
        }
        rt_hw_interrupt_enable(level);
        epd_power_notify();
        ++count;
    }
    bool pending = false;
    level = rt_hw_interrupt_disable();
    dispatching = false;
    for (unsigned i = 0; i < EPD_COLD_STATE_JOBS; ++i)
        if (jobs[i].id && !jobs[i].preparing && !jobs[i].copying &&
            jobs[i].view.state == EPD_COLD_STATE_QUEUED) pending = true;
    rt_hw_interrupt_enable(level);
    if (pending) notify();
    return count;
}

RTM_EXPORT(epd_cold_state_put);
RTM_EXPORT(epd_cold_state_load);
RTM_EXPORT(epd_cold_state_get);
RTM_EXPORT(epd_cold_state_copy);
RTM_EXPORT(epd_cold_state_retry);
RTM_EXPORT(epd_cold_state_release);
RTM_EXPORT(epd_cold_state_abandon);
RTM_EXPORT(epd_cold_state_take);
RTM_EXPORT(epd_cold_state_blob_release);
