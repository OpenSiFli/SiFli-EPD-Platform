/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_operation.h"
#include "epd_operation_runtime.h"
#include "epd_operation_runtime_internal.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <rtm.h>
#include <string.h>
#include <limits.h>
#include <mem_section.h>

typedef struct {
    epd_operation_scope_t id;
    epd_operation_scope_snapshot_t view;
    bool auto_release;
} scope_t;
typedef struct {
    epd_operation_t id;
    epd_operation_spec_t spec;
    epd_operation_snapshot_t view;
    epd_memory_owner_t owner, cleanup_owner;
    void *result;
    bool observer, callback_active, delivered, user_disposed;
    bool managed_task, task_intent, run_active;
} operation_t;
L2_RET_BSS_SECT_BEGIN(epd_operations)
static scope_t scopes[EPD_OPERATION_SCOPE_CAPACITY] L2_RET_BSS_SECT(epd_operations);
static operation_t operations[EPD_OPERATION_CAPACITY] L2_RET_BSS_SECT(epd_operations);
L2_RET_BSS_SECT_END
static uint32_t next_scope, next_operation;
static unsigned cursor;

static int operation_metadata_init(void)
{
    memset(scopes, 0, sizeof(scopes));
    memset(operations, 0, sizeof(operations));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(operation_metadata_init);

static scope_t *scope_find(epd_operation_scope_t id)
{
    if (id) for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY; ++i)
        if (scopes[i].id == id) return &scopes[i];
    return NULL;
}
static operation_t *find(epd_operation_t id)
{
    if (id) for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i)
        if (operations[i].id == id) return &operations[i];
    return NULL;
}
static void wake(epd_operation_executor_t executor)
{
    if (executor.wake && executor.wake_bits) rt_event_send(executor.wake, executor.wake_bits);
    else if (executor.thread && !executor.wake) epd_power_notify();
    if (executor.thread) epd_operation_runtime_wake();
}
size_t epd_operation_tasks(epd_operation_task_snapshot_t out[EPD_OPERATION_CAPACITY])
{
    size_t count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (!op->id || !op->managed_task || !op->task_intent ||
            (op->view.execution_done && !op->run_active)) continue;
        size_t at;
        for (at = 0; at < count; ++at) if (out[at].owner == op->owner) break;
        if (at == count) out[count++] = (epd_operation_task_snapshot_t){op->owner, 0};
        ++out[at].active;
    }
    rt_hw_interrupt_enable(level);
    return count;
}
void epd_operation_task_wake(epd_memory_owner_t owner)
{
    epd_operation_executor_t signals[EPD_OPERATION_CAPACITY];
    unsigned count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (op->id && op->owner == owner && op->managed_task && op->task_intent &&
            op->view.state == EPD_OPERATION_QUEUED) signals[count++] = op->spec.execute;
    }
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < count; ++i) wake(signals[i]);
}
static bool executor_valid(epd_operation_executor_t executor)
{
    return executor.thread && (!executor.wake || executor.wake_bits);
}
/* The core owner tree has bounded depth. Queries are nonallocating snapshots;
 * calling them inside the publication guard serializes submit against close. */
static bool owner_open(epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t snapshot;
    return epd_memory_owner_get(owner, &snapshot) && !snapshot.closed;
}
static bool owner_below(epd_memory_owner_t owner, epd_memory_owner_t root)
{
    for (unsigned depth = 0; depth < 32u; ++depth) {
        if (owner == root) return true;
        epd_memory_owner_snapshot_t snapshot;
        if (owner == EPD_MEMORY_OWNER_SYSTEM || !epd_memory_owner_get(owner, &snapshot) ||
            snapshot.parent == owner) return false;
        owner = snapshot.parent;
    }
    return false;
}
static void reap_scopes(void)
{
    epd_memory_owner_t owners[EPD_OPERATION_SCOPE_CAPACITY];
    unsigned count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY; ++i)
        if (scopes[i].id && scopes[i].auto_release && !scopes[i].view.operations) {
            owners[count++] = scopes[i].view.owner;
            scopes[i].id = 0;
        }
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < count; ++i) epd_memory_owner_release_trusted(owners[i]);
}
static bool expired(operation_t *op, uint32_t now)
{
    return op->spec.has_deadline && (int32_t)(now - op->spec.deadline_tick) >= 0;
}
static bool scope_closed(operation_t *op)
{
    scope_t *scope = scope_find(op->spec.scope);
    return !scope || scope->view.closing;
}
/* Caller holds the short publication guard. No callbacks/allocators/I/O here. */
static void request_stop(operation_t *op, epd_operation_status_t reason)
{
    if (op->view.execution_done || op->view.cancel_requested) return;
    op->view.cancel_requested = true;
    op->view.status = reason;
    if (op->view.state == EPD_OPERATION_QUEUED) {
        op->view.execution_done = true;
        op->view.state = EPD_OPERATION_SETTLING;
    }
}

bool epd_operation_scope_open(epd_memory_owner_t owner, uint32_t generation,
                              epd_operation_scope_t *out)
{
    if (!out || !epd_memory_owner_retain(owner)) return false;
    bool accepted = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (owner_open(owner) && next_scope != UINT32_MAX) for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY; ++i)
        if (!scopes[i].id) {
            scope_t *scope = &scopes[i];
            memset(scope, 0, sizeof(*scope));
            scope->id = ++next_scope;
            scope->view.owner = owner;
            scope->view.instance_generation = generation;
            *out = scope->id;
            accepted = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    if (!accepted) epd_memory_owner_release_trusted(owner);
    return accepted;
}
bool epd_operation_scope_get(epd_operation_scope_t id, epd_operation_scope_snapshot_t *out)
{
    if (!out) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    scope_t *scope = scope_find(id);
    bool found = scope != NULL;
    if (scope) *out = scope->view;
    rt_hw_interrupt_enable(level);
    return found;
}
bool epd_operation_scope_close(epd_operation_scope_t id)
{
    epd_operation_executor_t signals[EPD_OPERATION_CAPACITY * 3u];
    unsigned count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    scope_t *scope = scope_find(id);
    bool found = scope != NULL;
    if (scope) {
        scope->view.closing = true;
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
            operation_t *op = &operations[i];
            if (!op->id || op->spec.scope != id || op->view.state == EPD_OPERATION_FINISHED) continue;
            request_stop(op, EPD_OPERATION_CANCELLED);
            op->view.delivery_blocked = false;
            signals[count++] = op->spec.execute;
            signals[count++] = op->spec.cleanup;
            signals[count++] = op->spec.deliver;
        }
    }
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < count; ++i) wake(signals[i]);
    return found;
}
bool epd_operation_scope_release(epd_operation_scope_t id)
{
    epd_memory_owner_t owner = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    scope_t *scope = scope_find(id);
    bool ok = scope && scope->view.closing && !scope->view.operations;
    if (ok) { owner = scope->view.owner; scope->id = 0; }
    rt_hw_interrupt_enable(level);
    if (ok) {
        epd_memory_owner_release_trusted(owner);
        epd_operation_runtime_wake();
    }
    return ok;
}
void epd_operation_close_owner(epd_memory_owner_t owner)
{
    if (owner == EPD_MEMORY_OWNER_SYSTEM || owner == EPD_MEMORY_OWNER_INVALID) return;
    (void)epd_memory_owner_close_trusted(owner);
    epd_operation_executor_t signals[EPD_OPERATION_CAPACITY * 3u];
    unsigned count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < EPD_OPERATION_SCOPE_CAPACITY; ++i)
        if (scopes[i].id && owner_below(scopes[i].view.owner, owner))
            scopes[i].view.closing = scopes[i].auto_release = true;
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (!op->id || op->view.state == EPD_OPERATION_FINISHED ||
            (!owner_below(op->owner, owner) && !owner_below(op->cleanup_owner, owner))) continue;
        request_stop(op, EPD_OPERATION_CANCELLED);
        op->view.delivery_blocked = false;
        signals[count++] = op->spec.execute;
        signals[count++] = op->spec.cleanup;
        signals[count++] = op->spec.deliver;
    }
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < count; ++i) wake(signals[i]);
    reap_scopes();
}
bool epd_operation_owner_drained(epd_memory_owner_t owner)
{
    if (owner == EPD_MEMORY_OWNER_INVALID) return false;
    bool drained = true;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (op->id && op->view.state != EPD_OPERATION_FINISHED &&
            (owner_below(op->owner, owner) || owner_below(op->cleanup_owner, owner))) { drained = false; break; }
    }
    rt_hw_interrupt_enable(level);
    return drained;
}
bool epd_operation_submit(const epd_operation_spec_t *spec, epd_operation_t *out)
{
    if (!spec || !out || !spec->run || !spec->receive ||
        spec->scope == spec->cleanup_scope || !executor_valid(spec->execute) ||
        !executor_valid(spec->deliver) || !executor_valid(spec->cleanup)) return false;
    bool ok = false;
    rt_base_t level = rt_hw_interrupt_disable();
    scope_t *scope = scope_find(spec->scope), *cleanup = scope_find(spec->cleanup_scope);
    if (scope && cleanup && !scope->view.closing && !cleanup->view.closing &&
        epd_memory_owner_is_current_account(scope->view.owner) &&
        epd_memory_owner_is_current_account(cleanup->view.owner) &&
        owner_open(scope->view.owner) && owner_open(cleanup->view.owner) && next_operation != UINT32_MAX)
        for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) if (!operations[i].id) {
            operation_t *op = &operations[i];
            memset(op, 0, sizeof(*op));
            op->id = ++next_operation;
            op->spec = *spec;
            op->owner = scope->view.owner;
            epd_memory_owner_snapshot_t owner_state;
            if (epd_memory_owner_get(op->owner, &owner_state))
                op->managed_task = owner_state.kind == EPD_MEMORY_SCOPE_TASK ||
                                   owner_state.kind == EPD_MEMORY_SCOPE_SERVICE;
            op->cleanup_owner = cleanup->view.owner;
            op->view.instance_generation = scope->view.instance_generation;
            op->view.state = EPD_OPERATION_QUEUED;
            op->observer = true;
            ++scope->view.operations;
            ++cleanup->view.operations;
            *out = op->id;
            ok = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    if (ok) wake(spec->execute);
    return ok;
}
bool epd_operation_cancel(epd_operation_t id)
{
    epd_operation_executor_t execute = {0}, cleanup = {0}, deliver = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op && !op->view.execution_done;
    if (ok) {
        request_stop(op, EPD_OPERATION_CANCELLED);
        execute = op->spec.execute;
        cleanup = op->spec.cleanup;
        deliver = op->spec.deliver;
    }
    rt_hw_interrupt_enable(level);
    if (ok) { wake(execute); wake(cleanup); wake(deliver); }
    return ok;
}
bool epd_operation_complete(epd_operation_t id, epd_operation_status_t status,
                            int error, void *result)
{
    if ((unsigned)status > EPD_OPERATION_FAILED) return false;
    epd_operation_executor_t cleanup = {0}, deliver = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op && op->view.state == EPD_OPERATION_RUNNING && !op->view.execution_done &&
              (!result || op->spec.dispose_result);
    if (ok) {
        if (expired(op, (uint32_t)rt_tick_get())) request_stop(op, EPD_OPERATION_EXPIRED);
        if (!op->view.cancel_requested) op->view.status = status;
        op->view.error = error;
        op->result = result;
        op->view.result_pending = result != NULL;
        op->view.execution_done = true;
        op->view.state = EPD_OPERATION_SETTLING;
        cleanup = op->spec.cleanup;
        deliver = op->spec.deliver;
    }
    rt_hw_interrupt_enable(level);
    if (ok) { wake(cleanup); wake(deliver); }
    return ok;
}
bool epd_operation_yield(epd_operation_t id)
{
    epd_operation_executor_t execute = {0}, cleanup = {0}, deliver = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op && op->callback_active && op->spec.execute.thread == rt_thread_self() &&
              op->view.state == EPD_OPERATION_RUNNING && !op->view.execution_done;
    if (ok) {
        if (expired(op, (uint32_t)rt_tick_get())) request_stop(op, EPD_OPERATION_EXPIRED);
        op->view.state = EPD_OPERATION_QUEUED;
        if (op->view.cancel_requested) {
            op->view.execution_done = true;
            op->view.state = EPD_OPERATION_SETTLING;
        }
        execute = op->spec.execute;
        cleanup = op->spec.cleanup;
        deliver = op->spec.deliver;
    }
    rt_hw_interrupt_enable(level);
    if (ok) { wake(execute); wake(cleanup); wake(deliver); }
    return ok;
}
bool epd_operation_get(epd_operation_t id, epd_operation_snapshot_t *out)
{
    if (!out) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op != NULL;
    if (op) *out = op->view;
    rt_hw_interrupt_enable(level);
    return ok;
}
bool epd_operation_retry_delivery(epd_operation_t id)
{
    epd_operation_executor_t executor = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op && op->view.state == EPD_OPERATION_READY && op->view.delivery_blocked;
    if (ok) { op->view.delivery_blocked = false; executor = op->spec.deliver; }
    rt_hw_interrupt_enable(level);
    if (ok) wake(executor);
    return ok;
}
bool epd_operation_release(epd_operation_t id)
{
    rt_base_t level = rt_hw_interrupt_disable();
    operation_t *op = find(id);
    bool ok = op && op->observer;
    bool freed = ok && op->view.state == EPD_OPERATION_FINISHED;
    if (ok) {
        op->observer = false;
        if (op->view.state == EPD_OPERATION_FINISHED) op->id = 0;
    }
    rt_hw_interrupt_enable(level);
    if (freed) epd_operation_runtime_wake();
    return ok;
}
void epd_operation_poll(void)
{
    epd_operation_executor_t signals[EPD_OPERATION_CAPACITY * 3u];
    unsigned count = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (op->id && !op->view.execution_done && !op->view.cancel_requested && expired(op, now)) {
            request_stop(op, EPD_OPERATION_EXPIRED);
            signals[count++] = op->spec.execute;
            signals[count++] = op->spec.cleanup;
            signals[count++] = op->spec.deliver;
        }
    }
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < count; ++i) wake(signals[i]);
}
uint32_t epd_operation_next_timeout(uint32_t maximum_ticks)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < EPD_OPERATION_CAPACITY; ++i) {
        operation_t *op = &operations[i];
        if (op->id && !op->view.execution_done && !op->view.cancel_requested && op->spec.has_deadline) {
            int32_t delta = (int32_t)(op->spec.deadline_tick - now);
            uint32_t delay = delta > 0 ? (uint32_t)delta : 0;
            if (delay < maximum_ticks) maximum_ticks = delay;
        }
    }
    rt_hw_interrupt_enable(level);
    return maximum_ticks;
}

typedef enum { ACTION_NONE, ACTION_RUN, ACTION_DISPOSE, ACTION_DELIVER, ACTION_RETIRE } action_t;
size_t epd_operation_dispatch(size_t budget)
{
    epd_operation_poll();
    size_t done = 0;
    rt_thread_t thread = rt_thread_self();
    while (done < budget) {
        action_t action = ACTION_NONE;
        operation_t *op = NULL;
        rt_base_t level = rt_hw_interrupt_disable();
        for (unsigned n = 0; n < EPD_OPERATION_CAPACITY; ++n) {
            unsigned at = (cursor + n) % EPD_OPERATION_CAPACITY;
            operation_t *candidate = &operations[at];
            if (!candidate->id || candidate->callback_active || candidate->view.state == EPD_OPERATION_FINISHED) continue;
            if (candidate->view.state == EPD_OPERATION_QUEUED && candidate->spec.execute.thread == thread)
                action = ACTION_RUN;
            else if (candidate->view.execution_done) {
                bool failed = candidate->view.status != EPD_OPERATION_OK;
                bool closed = !failed && scope_closed(candidate);
                bool discard = failed || closed;
                if (candidate->result && discard && candidate->spec.cleanup.thread == thread)
                    action = ACTION_DISPOSE;
                else if ((!candidate->result || !discard) && !candidate->delivered) {
                    if (failed ? scope_closed(candidate) : closed) {
                        candidate->delivered = true;
                    } else if (candidate->spec.deliver.thread == thread && !candidate->view.delivery_blocked)
                        action = ACTION_DELIVER;
                }
                if (action == ACTION_NONE && candidate->delivered && candidate->spec.cleanup.thread == thread)
                    action = ACTION_RETIRE;
            }
            if (action != ACTION_NONE) { op = candidate; cursor = (at + 1u) % EPD_OPERATION_CAPACITY; break; }
        }
        if (!op) { rt_hw_interrupt_enable(level); break; }
        epd_operation_state_t prior_state = op->view.state;
        op->callback_active = true;
        if (action == ACTION_RUN) {
            op->view.state = EPD_OPERATION_RUNNING;
            op->run_active = true;
            op->task_intent = op->managed_task;
        }
        if (action == ACTION_DELIVER) op->view.state = EPD_OPERATION_DELIVERING;
        epd_operation_spec_t spec = op->spec;
        bool managed_task = op->managed_task;
        epd_operation_snapshot_t snapshot = op->view;
        epd_operation_t id = op->id;
        void *result = op->result;
        epd_memory_owner_t owner = action == ACTION_RUN || action == ACTION_DELIVER ? op->owner : op->cleanup_owner;
        rt_hw_interrupt_enable(level);

        if (action == ACTION_RUN && managed_task) {
            int admission = epd_operation_runtime_task_prepare(owner);
            if (admission) {
                if (admission < 0) (void)epd_operation_complete(id, EPD_OPERATION_FAILED, admission, NULL);
                level = rt_hw_interrupt_disable();
                op->callback_active = op->run_active = false;
                if (!op->view.execution_done) {
                    op->view.state = EPD_OPERATION_QUEUED;
                    if (op->view.cancel_requested) {
                        op->view.execution_done = true;
                        op->view.state = EPD_OPERATION_SETTLING;
                    }
                }
                bool settled = op->view.execution_done;
                rt_hw_interrupt_enable(level);
                if (settled) { wake(spec.cleanup); wake(spec.deliver); }
                if (admission > 0) break;
                ++done;
                continue;
            }
        }
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
        if (previous == EPD_MEMORY_OWNER_INVALID) {
            level = rt_hw_interrupt_disable();
            op->callback_active = false;
            op->run_active = false;
            /* A concurrent completion/close may have advanced the operation. */
            if (action == ACTION_RUN && !op->view.execution_done) {
                op->view.state = prior_state;
                if (op->view.cancel_requested) { op->view.execution_done = true; op->view.state = EPD_OPERATION_SETTLING; }
            }
            else if (action == ACTION_DELIVER) op->view.state = prior_state;
            rt_hw_interrupt_enable(level);
            break;
        }
        bool accepted = false;
        if (action == ACTION_RUN) spec.run(id, spec.user);
        else if (action == ACTION_DISPOSE) spec.dispose_result(result, spec.user);
        else if (action == ACTION_DELIVER) accepted = spec.receive(id, &snapshot, result, spec.user);
        else if (action == ACTION_RETIRE && spec.dispose_user) spec.dispose_user(spec.user);
        epd_memory_owner_leave_trusted(previous);

        epd_operation_executor_t signal = {0}, signal_other = {0};
        level = rt_hw_interrupt_disable();
        op->callback_active = false;
        op->run_active = false;
        if (action == ACTION_DISPOSE) {
            op->result = NULL;
            op->view.result_pending = false;
            op->view.state = EPD_OPERATION_READY;
            signal = spec.deliver;
            signal_other = spec.cleanup;
        } else if (action == ACTION_DELIVER) {
            op->view.state = EPD_OPERATION_READY;
            op->view.delivery_blocked = !accepted;
            if (accepted) { op->result = NULL; op->view.result_pending = false; op->delivered = true; }
            signal = spec.cleanup;
        } else if (action == ACTION_RETIRE) {
            scope_t *scope = scope_find(spec.scope), *cleanup = scope_find(spec.cleanup_scope);
            if (scope) --scope->view.operations;
            if (cleanup) --cleanup->view.operations;
            op->user_disposed = true;
            op->view.state = EPD_OPERATION_FINISHED;
            if (!op->observer) op->id = 0;
            signal = spec.deliver;
        } else if (op->view.execution_done) { signal = spec.cleanup; signal_other = spec.deliver; }
        else if (op->view.state == EPD_OPERATION_QUEUED) signal = spec.execute;
        rt_hw_interrupt_enable(level);
        wake(signal);
        wake(signal_other);
        if (action == ACTION_RUN && managed_task) epd_operation_runtime_wake();
        if (action == ACTION_RETIRE) reap_scopes();
        ++done;
    }
    return done;
}

RTM_EXPORT(epd_operation_scope_open);
RTM_EXPORT(epd_operation_scope_get);
RTM_EXPORT(epd_operation_scope_close);
RTM_EXPORT(epd_operation_scope_release);
RTM_EXPORT(epd_operation_owner_drained);
RTM_EXPORT(epd_operation_submit);
RTM_EXPORT(epd_operation_cancel);
RTM_EXPORT(epd_operation_yield);
RTM_EXPORT(epd_operation_complete);
RTM_EXPORT(epd_operation_get);
RTM_EXPORT(epd_operation_retry_delivery);
RTM_EXPORT(epd_operation_release);
