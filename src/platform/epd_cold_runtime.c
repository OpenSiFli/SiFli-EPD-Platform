/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_cold_runtime.h"
#include "epd_operation_runtime.h"
#include "../services/app_background.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <rtm.h>
#include <mem_section.h>
#include <stdio.h>
#include <string.h>

#define CHECKPOINT_MAGIC UINT32_C(0x43504b31)
#define ENVELOPE_BYTES 12u
typedef enum {
    LOAD_QUEUED, LOADING, RESTORE_READY, IDLE, CAPTURE_QUEUED, SAVING,
    LOAD_FAILED, RESTORE_FAILED, CAPTURE_FAILED, SAVE_FAILED, SAVE_ABANDONED
} phase_t;
typedef struct {
    epd_checkpoint_t id;
    epd_memory_owner_t owner, cleanup;
    epd_checkpoint_spec_t spec;
    epd_checkpoint_restore_edits_t restore_edits;
    char key[EPD_COLD_STATE_KEY_MAX + 1u];
    epd_cold_state_t job;
    epd_cold_runtime_snapshot_t view;
    phase_t phase;
    uint32_t requested_policy, requested_dirty, edits, capture_revision;
    bool busy, known_revision, model_dirty, requested, closing;
} checkpoint_t;
L2_RET_BSS_SECT_BEGIN(epd_checkpoints)
static checkpoint_t records[EPD_CHECKPOINT_CAPACITY] L2_RET_BSS_SECT(epd_checkpoints);
static unsigned char gui_bytes[EPD_COLD_STATE_BYTES_MAX] L2_RET_BSS_SECT(epd_checkpoints);
L2_RET_BSS_SECT_END
static uint32_t next_checkpoint;
static unsigned gui_cursor, worker_cursor;
static bool gui_pumping, worker_pumping;

static int checkpoint_metadata_init(void)
{
    memset(records, 0, sizeof(records));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(checkpoint_metadata_init);
static checkpoint_t *find(epd_checkpoint_t id)
{
    if (id) for (unsigned i = 0; i < EPD_CHECKPOINT_CAPACITY; ++i)
        if (records[i].id == id) return &records[i];
    return NULL;
}
static checkpoint_t *find_owner(epd_memory_owner_t owner)
{
    for (unsigned i = 0; i < EPD_CHECKPOINT_CAPACITY; ++i)
        if (records[i].id && records[i].owner == owner) return &records[i];
    return NULL;
}
static checkpoint_t *find_current(epd_checkpoint_t id)
{
    checkpoint_t *record = find(id);
    epd_memory_owner_snapshot_t current, root;
    if (!record || !epd_memory_owner_get(epd_memory_owner_current(), &current) || current.closed ||
        current.account != record->owner || !epd_memory_owner_get(current.account, &root) ||
        root.closed || root.kind != EPD_MEMORY_SCOPE_APP || root.owner == EPD_MEMORY_OWNER_SYSTEM)
        return NULL;
    return record;
}
static bool lane(bool gui)
{
    epd_operation_executor_t worker, deliver, cleanup;
    return epd_operation_runtime_executors(&worker, &deliver, &cleanup) &&
           rt_thread_self() == (gui ? deliver.thread : worker.thread);
}
static void changed(void)
{
    epd_power_notify();
    epd_operation_runtime_wake();
}
static void snapshot(checkpoint_t *record, epd_cold_runtime_snapshot_t *out)
{
    *out = record->view;
    out->registered = true;
    out->closing = record->closing;
    out->busy = record->busy || record->phase == LOAD_QUEUED || record->phase == LOADING ||
                record->phase == RESTORE_READY || record->phase == CAPTURE_QUEUED || record->phase == SAVING;
    out->restoring = record->phase == LOAD_QUEUED || record->phase == LOADING || record->phase == RESTORE_READY;
    out->failed = record->phase >= LOAD_FAILED;
    out->durable = record->known_revision && !record->model_dirty &&
                   record->view.saved_revision != 0 && !out->failed &&
                   record->view.captured_policy_revision != 0 && !out->busy;
}
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_owner(owner);
    bool found = record != NULL;
    if (record) snapshot(record, out);
    rt_hw_interrupt_enable(level);
    return found;
}
bool epd_checkpoint_get(epd_checkpoint_t id, epd_cold_runtime_snapshot_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!lane(true)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_current(id);
    bool found = record != NULL;
    if (record) snapshot(record, out);
    rt_hw_interrupt_enable(level);
    return found;
}
static bool checkpoint_register(const epd_checkpoint_spec_t *spec,
                                 epd_checkpoint_restore_edits_t restore_edits, epd_checkpoint_t *out)
{
    if (!lane(true) || !spec || !out || !spec->key || !*spec->key || !spec->format_version ||
        !spec->capture || !spec->restore || spec->maximum_bytes > EPD_CHECKPOINT_BYTES_MAX) return false;
    epd_memory_owner_snapshot_t current, root;
    if (!epd_memory_owner_get(epd_memory_owner_current(), &current) || current.closed ||
        !epd_memory_owner_get(current.account, &root) || root.closed ||
        root.kind != EPD_MEMORY_SCOPE_APP || root.owner == EPD_MEMORY_OWNER_SYSTEM) return false;
    epd_app_background_snapshot_t background;
    if (app_background_snapshot(root.owner, &background) != EPD_MEMORY_OK || background.stopping) return false;
    char key[EPD_COLD_STATE_KEY_MAX + 1u];
    int size = snprintf(key, sizeof(key), "%.*s/%s", EPD_MEMORY_APPID_SIZE, root.appid, spec->key);
    if (size <= 0 || (size_t)size >= sizeof(key) || !epd_memory_owner_retain_trusted(root.owner)) return false;
    epd_memory_owner_t cleanup = epd_memory_scope_create_trusted(root.owner, EPD_MEMORY_SCOPE_CLEANUP);
    if (cleanup == EPD_MEMORY_OWNER_INVALID) { epd_memory_owner_release_trusted(root.owner); return false; }
    checkpoint_t *record = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    bool duplicate = false;
    for (unsigned i = 0; i < EPD_CHECKPOINT_CAPACITY; ++i) {
        if (records[i].id && (records[i].owner == root.owner || !strcmp(records[i].key, key))) duplicate = true;
        if (!records[i].id && !record) record = &records[i];
    }
    bool accepted = record && !duplicate && next_checkpoint != UINT32_MAX;
    if (accepted) {
        memset(record, 0, sizeof(*record));
        record->id = ++next_checkpoint;
        record->owner = root.owner; record->cleanup = cleanup; record->spec = *spec;
        record->restore_edits = restore_edits;
        memcpy(record->key, key, (size_t)size + 1u);
        record->spec.key = record->key;
        record->phase = LOAD_QUEUED;
        record->view.result = EPD_MEMORY_WAITING;
        *out = record->id;
    }
    rt_hw_interrupt_enable(level);
    if (!accepted) {
        epd_memory_owner_close_trusted(cleanup); epd_memory_owner_release_trusted(cleanup);
        epd_memory_owner_release_trusted(root.owner);
    } else changed();
    return accepted;
}
bool epd_checkpoint_register(const epd_checkpoint_spec_t *spec, epd_checkpoint_t *out)
{
    return checkpoint_register(spec, NULL, out);
}
bool epd_checkpoint_register_edits(const epd_checkpoint_spec_t *spec,
                                    epd_checkpoint_restore_edits_t restore_edits, epd_checkpoint_t *out)
{
    return restore_edits && checkpoint_register(spec, restore_edits, out);
}
bool epd_checkpoint_changed(epd_checkpoint_t id)
{
    if (!lane(true)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_current(id);
    bool ok = record && !record->closing && record->edits != UINT32_MAX &&
              (!record->known_revision || record->view.data_revision != UINT32_MAX);
    epd_memory_owner_t owner = ok ? record->owner : EPD_MEMORY_OWNER_INVALID;
    rt_hw_interrupt_enable(level);
    if (!ok) return false;
    epd_app_background_snapshot_t background;
    if (app_background_snapshot(owner, &background) != EPD_MEMORY_OK || background.stopping ||
        app_background_dirty(owner, NULL) != EPD_MEMORY_OK) return false;
    level = rt_hw_interrupt_disable();
    ++record->edits;
    if (record->known_revision) ++record->view.data_revision;
    record->model_dirty = true;
    rt_hw_interrupt_enable(level);
    changed(); return true;
}
bool epd_checkpoint_restore(epd_checkpoint_t id)
{
    if (!lane(true)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_current(id);
    bool ok = record && !record->closing && !record->busy && !record->job &&
              !record->model_dirty && record->phase == IDLE;
    if (ok) { record->phase = LOAD_QUEUED; record->edits = 0; record->view.result = EPD_MEMORY_WAITING; }
    rt_hw_interrupt_enable(level);
    if (ok) changed();
    return ok;
}
bool epd_checkpoint_retry(epd_checkpoint_t id)
{
    if (!lane(true)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_current(id);
    bool ok = record && !record->busy && record->phase >= LOAD_FAILED && record->phase <= SAVE_FAILED;
    epd_cold_state_t job = ok ? record->job : 0;
    phase_t phase = ok ? record->phase : IDLE;
    if (ok) record->busy = true;
    rt_hw_interrupt_enable(level);
    if (!ok) return false;
    if (job && (phase == LOAD_FAILED || phase == SAVE_FAILED)) ok = epd_cold_state_retry(job);
    level = rt_hw_interrupt_disable();
    if (ok) {
        record->view.result = EPD_MEMORY_WAITING;
        record->phase = phase == SAVE_FAILED ? SAVING : phase == CAPTURE_FAILED ? CAPTURE_QUEUED :
                        phase == RESTORE_FAILED ? RESTORE_READY : job ? LOADING : LOAD_QUEUED;
    }
    record->busy = false;
    rt_hw_interrupt_enable(level);
    if (ok) changed();
    return ok;
}
/* Explicit stop rejects new business work, but prepare_close still has to
 * settle the current checkpoint before the loader closes the provider. */
static bool checkpoint_current(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    epd_app_background_snapshot_t state;
    return policy && app_background_snapshot(owner, &state) == EPD_MEMORY_OK &&
           !state.foreground && !state.tasks && state.revision == policy && state.dirty_revision == dirty;
}

bool epd_cold_runtime_discardable(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_owner(owner);
    bool ok = record && record->spec.optional && record->phase == SAVE_FAILED && record->job &&
              record->requested && !record->busy && !record->closing &&
              (record->view.storage_error == EPD_COLD_STATE_IO || record->view.storage_error == EPD_COLD_STATE_FULL) &&
              record->capture_revision == record->view.data_revision &&
              record->requested_policy == policy && record->view.captured_policy_revision == policy &&
              record->requested_dirty == dirty && record->view.captured_dirty_revision == dirty &&
              checkpoint_current(owner, policy, dirty);
    rt_hw_interrupt_enable(level);
    return ok;
}
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{
    if (!checkpoint_current(owner, policy, dirty)) return EPD_MEMORY_BUSY;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_owner(owner);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    bool wake = false;
    if (record && !record->closing) {
        epd_cold_runtime_snapshot_t view; snapshot(record, &view);
        if (!view.busy && !view.failed && record->known_revision && !record->model_dirty &&
            view.saved_revision && (view.captured_dirty_revision == dirty)) {
            /* A foreground round trip changes policy, not the durable bytes. */
            record->view.captured_policy_revision = policy;
            record->view.captured_dirty_revision = dirty;
            record->view.result = EPD_MEMORY_OK;
            result = EPD_MEMORY_OK;
        }
        else if (view.failed && record->requested_policy == policy && record->requested_dirty == dirty)
            result = record->view.result;
        else {
            wake = !record->requested || record->requested_policy != policy || record->requested_dirty != dirty;
            record->requested = true; record->requested_policy = policy; record->requested_dirty = dirty;
            if (!record->busy && !record->job && record->known_revision &&
                (record->phase == IDLE || record->phase == CAPTURE_FAILED)) {
                record->phase = CAPTURE_QUEUED; record->view.result = EPD_MEMORY_WAITING;
                wake = true;
            }
            result = view.failed && record->job ? record->view.result : EPD_MEMORY_WAITING;
            /* Repeated polls must let the GUI sleep while the save worker runs. */
            wake = wake && (!view.failed || record->phase == CAPTURE_QUEUED);
        }
    }
    rt_hw_interrupt_enable(level);
    if (wake) changed();
    return result;
}
bool epd_checkpoint_unregister(epd_checkpoint_t id)
{
    if (!lane(true)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_current(id);
    bool ok = record && (!record->model_dirty || record->spec.optional);
    if (ok) { record->closing = true; record->requested = false; }
    rt_hw_interrupt_enable(level);
    if (ok) changed();
    return ok;
}
void epd_cold_runtime_close_owner(epd_memory_owner_t owner)
{
    rt_base_t level = rt_hw_interrupt_disable();
    checkpoint_t *record = find_owner(owner);
    if (record) { record->closing = true; record->requested = false; }
    rt_hw_interrupt_enable(level);
    if (record) changed();
}
bool epd_cold_runtime_owner_busy(epd_memory_owner_t owner)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = find_owner(owner) != NULL;
    rt_hw_interrupt_enable(level);
    return busy;
}
static epd_cold_state_spec_t storage_spec(checkpoint_t *record)
{
    return (epd_cold_state_spec_t){record->key, record->cleanup, record->owner,
        record->spec.format_version, record->capture_revision, record->spec.optional};
}
static epd_memory_result_t error_result(epd_cold_state_error_t error)
{
    return error == EPD_COLD_STATE_FULL ? EPD_MEMORY_NO_CAPACITY :
           error == EPD_COLD_STATE_STALE ? EPD_MEMORY_INVALID : EPD_MEMORY_BUSY;
}
size_t epd_cold_runtime_worker_pump(size_t budget)
{
    if (!lane(false)) return 0;
    rt_base_t level = rt_hw_interrupt_disable();
    if (worker_pumping) { rt_hw_interrupt_enable(level); return 0; }
    worker_pumping = true; rt_hw_interrupt_enable(level);
    size_t count = 0;
    for (unsigned n = 0; n < EPD_CHECKPOINT_CAPACITY && count < budget; ++n) {
        unsigned at = (worker_cursor + n) % EPD_CHECKPOINT_CAPACITY;
        level = rt_hw_interrupt_disable();
        checkpoint_t *record = &records[at];
        phase_t phase = record->phase;
        bool retire_load = record->closing && record->job &&
                           (phase == LOAD_FAILED || phase == RESTORE_FAILED);
        bool abandon = record->closing && record->job && record->spec.optional && phase == SAVE_FAILED;
        if (!record->id || record->busy || (!retire_load && !abandon &&
            phase != LOAD_QUEUED && phase != LOADING && phase != SAVING)) {
            rt_hw_interrupt_enable(level); continue;
        }
        record->busy = true; epd_cold_state_t job = record->job;
        rt_hw_interrupt_enable(level);
        epd_cold_state_snapshot_t state = {0};
        bool acted = false, accepted = false;
        if (retire_load || abandon) {
            accepted = abandon ? epd_cold_state_abandon(job) : epd_cold_state_release(job);
            acted = accepted;
        } else if (phase == LOAD_QUEUED) {
            epd_cold_state_spec_t spec = storage_spec(record);
            /* The shared worker must submit within the checkpoint's account.
             * The public storage API validates the caller before retaining it. */
            epd_memory_owner_t previous = epd_memory_owner_enter_trusted(spec.cleanup_owner);
            if (previous != EPD_MEMORY_OWNER_INVALID) {
                accepted = epd_cold_state_load(&spec, &job);
                epd_memory_owner_leave_trusted(previous);
            }
            acted = true;
        } else if (epd_cold_state_get(job, &state)) {
            acted = state.state == EPD_COLD_STATE_LOADED || state.state == EPD_COLD_STATE_SAVED ||
                    state.state == EPD_COLD_STATE_FAILED;
        }
        bool release = acted && phase == SAVING && state.state == EPD_COLD_STATE_SAVED;
        if (acted && phase == LOADING && state.state == EPD_COLD_STATE_FAILED && state.error == EPD_COLD_STATE_NOT_FOUND)
            release = true;
        if (release) (void)epd_cold_state_release(job);
        level = rt_hw_interrupt_disable();
        if (acted) {
            if (retire_load || abandon) {
                record->job = 0; record->phase = abandon ? SAVE_ABANDONED : IDLE;
            } else if (phase == LOAD_QUEUED) {
                record->job = accepted ? job : 0;
                record->phase = accepted ? LOADING : LOAD_FAILED;
                record->view.result = accepted ? EPD_MEMORY_WAITING : EPD_MEMORY_NO_CAPACITY;
            } else if (phase == LOADING && state.state == EPD_COLD_STATE_LOADED) {
                record->phase = RESTORE_READY;
            } else if (phase == LOADING && release) {
                record->job = 0; record->known_revision = true;
                record->view.data_revision = record->edits; record->view.saved_revision = 0;
                record->view.result = EPD_MEMORY_OK; record->view.storage_error = EPD_COLD_STATE_NOT_FOUND;
                record->phase = record->requested ? CAPTURE_QUEUED : IDLE;
            } else if (phase == SAVING && release) {
                record->job = 0; record->view.saved_revision = record->capture_revision;
                record->model_dirty = record->view.data_revision != record->capture_revision;
                record->view.result = EPD_MEMORY_OK; record->view.storage_error = EPD_COLD_STATE_OK;
                record->phase = record->requested && record->model_dirty ? CAPTURE_QUEUED : IDLE;
            } else {
                record->phase = phase == SAVING ? SAVE_FAILED : LOAD_FAILED;
                record->view.result = error_result(state.error); record->view.storage_error = state.error;
            }
        }
        record->busy = false; rt_hw_interrupt_enable(level);
        if (acted) { ++count; changed(); }
    }
    level = rt_hw_interrupt_disable();
    worker_cursor = (worker_cursor + 1u) % EPD_CHECKPOINT_CAPACITY;
    worker_pumping = false; rt_hw_interrupt_enable(level);
    if (budget && count == budget) epd_operation_runtime_wake();
    return count;
}
static void put_u32(unsigned char *out, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (i * 8)); }
static uint32_t get_u32(const unsigned char *bytes)
{ return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24; }
size_t epd_cold_runtime_ui_pump(size_t budget)
{
    if (!lane(true)) return 0;
    rt_base_t level = rt_hw_interrupt_disable();
    if (gui_pumping) { rt_hw_interrupt_enable(level); return 0; }
    gui_pumping = true; rt_hw_interrupt_enable(level);
    size_t count = 0;
    for (unsigned n = 0; n < EPD_CHECKPOINT_CAPACITY && count < budget; ++n) {
        unsigned at = (gui_cursor + n) % EPD_CHECKPOINT_CAPACITY;
        level = rt_hw_interrupt_disable();
        checkpoint_t *record = &records[at];
        bool detach = record->closing && !record->job && (!record->model_dirty || record->spec.optional);
        if (!record->id || record->busy || (!detach && record->phase != CAPTURE_QUEUED && record->phase != RESTORE_READY)) {
            rt_hw_interrupt_enable(level); continue;
        }
        record->busy = true;
        phase_t phase = record->phase;
        epd_memory_owner_t owner = record->owner, cleanup = record->cleanup;
        epd_checkpoint_spec_t spec = record->spec;
        epd_cold_state_t job = record->job;
        /* Reserve the first revision before a reentrant capture can mark edits. */
        if (!detach && phase == CAPTURE_QUEUED && !record->view.data_revision)
            record->view.data_revision = 1;
        uint32_t edits = record->edits, revision = record->view.data_revision;
        uint32_t policy = record->requested_policy, dirty = record->requested_dirty;
        rt_hw_interrupt_enable(level);
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
        bool entered = previous != EPD_MEMORY_OWNER_INVALID;
        bool ok = entered, accepted = false, stale = false;
        epd_cold_state_snapshot_t stored = {0};
        size_t size = 0;
        if (detach) {
            if (entered && spec.detach) spec.detach(spec.user);
        } else if (phase == CAPTURE_QUEUED) {
            stale = record->closing || !checkpoint_current(owner, policy, dirty);
            if (ok && !stale) {
                ok = spec.capture(spec.user, gui_bytes + ENVELOPE_BYTES, spec.maximum_bytes, &size) && size <= spec.maximum_bytes;
                if (ok) {
                    put_u32(gui_bytes, CHECKPOINT_MAGIC); put_u32(gui_bytes + 4, spec.source_version);
                    put_u32(gui_bytes + 8, (uint32_t)size);
                    epd_cold_state_spec_t storage = storage_spec(record);
                    storage.revision = revision;
                    accepted = epd_cold_state_put(&storage, gui_bytes, size + ENVELOPE_BYTES, &job);
                }
            }
        } else {
            ok = epd_cold_state_get(job, &stored) && epd_cold_state_copy(job, gui_bytes, sizeof(gui_bytes), &size) &&
                 size >= ENVELOPE_BYTES && get_u32(gui_bytes) == CHECKPOINT_MAGIC &&
                 get_u32(gui_bytes + 4) == spec.source_version && get_u32(gui_bytes + 8) == size - ENVELOPE_BYTES &&
                 size - ENVELOPE_BYTES <= spec.maximum_bytes && stored.instance_generation == owner;
            if (ok && edits > UINT32_MAX - stored.revision) ok = false;
            if (ok) {
                /* Establish the validated baseline before a restore callback can
                 * report an edit. Overflow then rejects the edit before publish. */
                level = rt_hw_interrupt_disable();
                record->known_revision = true;
                record->view.data_revision = stored.revision + record->edits;
                rt_hw_interrupt_enable(level);
            }
            if (ok && !record->closing) {
                if (!edits) ok = entered && spec.restore(spec.user, gui_bytes + ENVELOPE_BYTES, size - ENVELOPE_BYTES);
                else if (record->restore_edits)
                    ok = entered && record->restore_edits(spec.user, gui_bytes + ENVELOPE_BYTES, size - ENVELOPE_BYTES, edits);
            }
            if (ok) (void)epd_cold_state_release(job);
        }
        if (entered) epd_memory_owner_leave_trusted(previous);
        if (detach && entered) {
            epd_memory_owner_close_trusted(cleanup); epd_memory_owner_release_trusted(cleanup);
            epd_memory_owner_release_trusted(owner);
        }
        level = rt_hw_interrupt_disable();
        if (detach) {
            if (entered) record->id = 0;
        } else if (phase == CAPTURE_QUEUED) {
            if (stale) { record->phase = IDLE; record->requested = false; record->view.result = EPD_MEMORY_CANCELLED; }
            else if (accepted) {
                record->job = job; record->capture_revision = revision;
                record->view.captured_policy_revision = policy;
                record->view.captured_dirty_revision = dirty;
                record->phase = SAVING; record->view.result = EPD_MEMORY_WAITING;
            } else { record->phase = CAPTURE_FAILED; record->view.result = ok ? EPD_MEMORY_NO_CAPACITY : EPD_MEMORY_INVALID; }
        } else if (ok) {
            record->job = 0; record->known_revision = true;
            record->view.saved_revision = stored.revision;
            record->view.data_revision = stored.revision + record->edits;
            record->model_dirty = record->edits != 0;
            record->view.result = EPD_MEMORY_OK; record->view.storage_error = EPD_COLD_STATE_OK;
            record->phase = record->requested ? CAPTURE_QUEUED : IDLE;
        } else { record->phase = RESTORE_FAILED; record->view.result = EPD_MEMORY_INVALID; record->view.storage_error = EPD_COLD_STATE_FORMAT; }
        record->busy = false; rt_hw_interrupt_enable(level);
        ++count; changed();
    }
    level = rt_hw_interrupt_disable();
    gui_cursor = (gui_cursor + 1u) % EPD_CHECKPOINT_CAPACITY;
    gui_pumping = false; rt_hw_interrupt_enable(level);
    if (budget && count == budget) epd_power_notify();
    return count;
}
RTM_EXPORT(epd_checkpoint_register);
RTM_EXPORT(epd_checkpoint_register_edits);
RTM_EXPORT(epd_checkpoint_changed);
RTM_EXPORT(epd_checkpoint_get);
RTM_EXPORT(epd_checkpoint_restore);
RTM_EXPORT(epd_checkpoint_retry);
RTM_EXPORT(epd_checkpoint_unregister);
