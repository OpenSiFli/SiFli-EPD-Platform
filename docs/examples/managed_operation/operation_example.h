/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MANAGED_OPERATION_EXAMPLE_H
#define MANAGED_OPERATION_EXAMPLE_H
#include "epd_operation.h"
#include "epd_cold_state.h"

typedef struct {
    epd_operation_t operation;
    epd_operation_scope_t business, cleanup;
    size_t length, offset;
    uint32_t checksum;
    unsigned char input[256];
} operation_example_t;
/* Zero-initialize job. Keep job and module code alive until finish succeeds.
 * task is an actual scope returned by epd_app_task_begin, not an APP owner.
 * All source bytes are copied before successful admission. */
bool operation_example_start(operation_example_t *job, epd_memory_owner_t task,
                             uint32_t generation, const void *bytes, size_t length,
                             uint32_t timeout_ticks);
bool operation_example_cancel(operation_example_t *job);
/* Query from the existing application completion/maintenance entry. No waiting.
 * Only successful finish releases the operation and its two scope handles. */
bool operation_example_finish(operation_example_t *job, uint32_t *checksum,
                              epd_operation_status_t *status);
/* Example serialized position: two little-endian uint32 fields, no native pointers. */
bool operation_example_save_position(const epd_cold_state_spec_t *spec,
                                     uint32_t source_version, uint32_t position,
                                     epd_cold_state_t *out);
/* Reads only a completed compatible load. Failure leaves position untouched;
 * the caller retains the cold-state handle and its retry/release responsibility. */
bool operation_example_restore_position(epd_cold_state_t state, uint32_t generation,
                                        uint32_t source_version, uint32_t *position);
#endif
