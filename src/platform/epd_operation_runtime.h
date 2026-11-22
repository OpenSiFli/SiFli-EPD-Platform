/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_OPERATION_RUNTIME_H
#define EPD_OPERATION_RUNTIME_H
#include "epd_operation.h"

/* Scheduling/lifecycle entry points in this header are resident-only. Dynamic
 * components import executors(), then use operation/cold-state business APIs. */

#define EPD_OPERATION_RUNTIME_CLIENTS 4u
typedef struct {
    const void *key;
    void *user;
    /* Resident firmware callbacks only. Executed on the shared worker, never
     * under its publication lock. process returns the number of handled items.
     * A return >= budget requests another fair pass without sleeping.
     * next_timeout_ticks returns min(maximum_ticks, ticks until next deadline);
     * UINT32_MAX means no deadline, zero means work is ready now. No ms values. */
    size_t (*process)(size_t budget, void *user);
    uint32_t (*next_timeout_ticks)(uint32_t maximum_ticks, void *user);
} epd_operation_runtime_client_t;
/* May register before init. Copies one fixed record; key/user/code must have
 * firmware lifetime. Re-registering the same unchanged key is idempotent. */
bool epd_operation_runtime_register(const epd_operation_runtime_client_t *client);
/* Worker-thread callback aggregation; actual units are RT ticks. */
uint32_t epd_operation_runtime_next_timeout_ticks(uint32_t maximum_ticks);

/* Call on the GUI owner thread after epd_power_init. Starts a resident worker
 * with PSRAM stack;
 * repeated calls are accepted only on that same GUI thread. */
bool epd_operation_runtime_init(void);
void epd_operation_runtime_close_owner(epd_memory_owner_t owner);
bool epd_operation_runtime_owner_drained(epd_memory_owner_t owner);
/* Fixed callback budget, GUI thread only. Insert in the existing GUI loop.
 * A full pass re-notifies the GUI so coalesced wake events cannot strand work. */
size_t epd_operation_runtime_ui_pump(size_t callback_budget);
/* Public module export. execute/cleanup are the resident worker, deliver is the registered GUI.
 * Use worker for receive as well for completions that need no UI callback. */
bool epd_operation_runtime_executors(epd_operation_executor_t *execute,
                                     epd_operation_executor_t *deliver,
                                     epd_operation_executor_t *cleanup);
/* Coalescible wake for external completion/resource events. */
void epd_operation_runtime_wake(void);
/* One bounded worker pass; only the resident worker may execute it. Returns
 * completed dispatch actions, not completed business operations. */
size_t epd_operation_runtime_worker_pump(size_t operation_budget, size_t cold_budget);
#endif
