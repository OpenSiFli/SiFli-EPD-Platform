/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_ACTIVITY_WORK_H
#define EPD_ACTIVITY_WORK_H
#include "epd_memory.h"
#include "epd_memory_manager.h"
#include "epd_resource.h"
typedef struct epd_activity_work epd_activity_work_t;
typedef struct {
    epd_memory_owner_t owner, credit;
} epd_activity_guard_t;
/* Worker-only admission of a real operation. Budgets are additional physical
 * peaks, including allocator overhead. Admission waits at most 500 ms, driven
 * by activity notifications. Caller-owned input buffers are not re-accounted. */
epd_activity_work_t *epd_activity_work_open(size_t psram, size_t sram);
/* Nonblocking admission for accepted queue work. The caller publishes the work
 * only after this succeeds, and closes it on replacement, cancellation or final
 * access. PAGE and NEARBY eligibility is validated by the resource runtime.
 * Nested submissions retain the enclosing PAGE/service/task lifetime rather
 * than the submitting work handle. End each independent queued operation.
 * Wake runs on epd_ops, outside manager locks. Its context must outlive deferred
 * observer retirement; component event callbacks normally use static state.
 * Both budgets include physical allocator overhead. File operations require
 * SRAM for DFS descriptors, paths and filesystem objects even when all payload
 * buffers use PSRAM. A zero SRAM budget does not permit those allocations. */
epd_activity_work_t *epd_activity_work_queue(epd_resource_kind_t kind,
    epd_resource_activity_t parent, size_t psram, size_t sram,
    void (*wake)(void *), void *context);
/* 0 waits for the wake event, 1 permits execution, -1 is terminal rejection. */
int epd_activity_work_ready(epd_activity_work_t *work);
/* Between worker slices, reserve unused capacity in both backends together.
 * 0 queues pressure recovery and waits for wake; 1 permits execution; -1 ends
 * this attempt. Leave all guards first. Existing payloads keep their ledger. */
int epd_activity_work_prepare(epd_activity_work_t *work, size_t psram, size_t sram);

typedef struct {
    uint32_t revision;
    size_t psram, sram; /* Currently uncommitted capacity after shared headroom/account budget. */
    bool ready;
} epd_activity_capacity_t;
/* Non-mutating capacity hint for a fresh queued attempt. OK supplies a coherent
 * snapshot, BUSY asks the caller to sample again; other errors reject context.
 * ready checks both requested heaps, not fair waiting or a physical contiguous
 * allocation. queue/prepare remain the only authority to admit execution.
 * Poll the cheap capacity epoch first; a changed epoch alone is not progress. */
int epd_activity_work_capacity(epd_resource_kind_t kind, epd_resource_activity_t parent,
    size_t psram, size_t sram, epd_activity_capacity_t *out);
typedef struct {
    size_t psram, sram; /* Last joint preparation target, including retained UNUSED. */
    epd_memory_credit_t credit; /* Borrowed, generation-tagged token; never a new grant. */
} epd_activity_demand_t;
/* Capture before close. A terminal attempt keeps its last actual requirements;
 * retrying it must not silently fall back to the original small queue peaks. */
bool epd_activity_work_demand(epd_activity_work_t *work, epd_activity_demand_t *out);
epd_resource_activity_t epd_activity_work_activity(const epd_activity_work_t *work);
/* An INTERACTIVE result is now displayed: end TASKS and return unused credit,
 * retaining only the validated PAGE activity as a NEARBY parent. Close after
 * the displayed binding ends. A settled handle cannot execute new work. */
bool epd_activity_work_settle(epd_activity_work_t *work);
/* Share one serial batch between its queue and selected job. Each retained
 * reference is closed once; only the last close ends the real task. */
bool epd_activity_work_retain(epd_activity_work_t *work);
bool epd_activity_work_enter(epd_activity_work_t *work, epd_activity_guard_t *guard);
void epd_activity_work_leave(epd_activity_guard_t *guard);
#if !defined(BUILD_DLMODULE) && !defined(RTT_IN_MODULE)
/* Resident device dispatch can execute an already accepted activity owned by
 * another account. Module guards stay within their caller's account. */
bool epd_activity_work_enter_trusted(epd_activity_work_t *work, epd_activity_guard_t *guard);
void epd_activity_work_leave_trusted(epd_activity_guard_t *guard);
#endif
/* Ensure at least bytes of unused credit in this backend before an explicit
 * growth attempt. Failure leaves existing allocations and credit unchanged. */
bool epd_activity_work_reserve(epd_activity_work_t *work, epd_app_memory_t region, size_t bytes);
/* Serial operation boundaries on a live synchronous work handle. Replenish
 * admits only the missing part of the original physical peaks, atomically
 * across backends, without changing the activity on failure. Trim returns
 * unused capacity after all allocation producers finish; retained buffers,
 * LIVE credit and TASKS protection remain intact. Neither operation waits. */
bool epd_activity_work_replenish(epd_activity_work_t *work);
/* Admit an operation's computed physical peaks on a live serial handle.
 * Existing blocks count toward each peak; both backend additions are atomic.
 * This does not change the original pause/resume peaks or trim live credit. */
bool epd_activity_work_replenish_to(epd_activity_work_t *work, size_t psram, size_t sram);
/* Release unused reservations from a quiescent admitted work handle. Leave
 * all guards first. Live allocations and the activity binding are retained. */
bool epd_activity_work_trim(epd_activity_work_t *work);
/* Serial worker operations on a synchronous work handle. Before pause, stop
 * producers and leave all guards; keep retained buffers stable until resume
 * returns. Pause returns unused reservations and keeps allocated block tokens.
 * Resume admits both backends together up to their original physical peaks.
 * Failure preserves retained data and leaves execution disabled. Neither call
 * is valid on the UI/resource executor or while another caller uses the handle. */
bool epd_activity_work_pause(epd_activity_work_t *work);
bool epd_activity_work_resume(epd_activity_work_t *work);
/* Call after all users/children have stopped. Observer retirement is deferred
 * to the resident worker; existing result blocks retain their original ledger. */
void epd_activity_work_close(epd_activity_work_t *work);
#endif
