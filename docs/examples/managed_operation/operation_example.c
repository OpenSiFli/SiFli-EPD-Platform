/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "operation_example.h"
#include "epd_operation_runtime.h"
#include <limits.h>
#include <string.h>

static void calculate(epd_operation_t operation, void *context)
{
    operation_example_t *job = context;
    epd_operation_snapshot_t state;
    if (!epd_operation_get(operation, &state)) return;
    if (state.cancel_requested) {
        (void)epd_operation_complete(operation, EPD_OPERATION_CANCELLED, 0, NULL);
        return;
    }
    size_t end = job->length - job->offset > 32u ? job->offset + 32u : job->length;
    while (job->offset < end) job->checksum += job->input[job->offset++];
    if (job->offset < job->length) (void)epd_operation_yield(operation);
    else (void)epd_operation_complete(operation, EPD_OPERATION_OK, 0, NULL);
}
static bool received(epd_operation_t operation, const epd_operation_snapshot_t *state,
                      void *result, void *context)
{
    (void)operation; (void)state; (void)result; (void)context;
    /* This example has no heap result or page pointer. finish reads job only
     * after the executor and this GUI callback have both returned. */
    return true;
}
static void release_scopes(operation_example_t *job)
{
    if (job->business) {
        epd_operation_scope_close(job->business);
        epd_operation_scope_release(job->business);
        job->business = 0;
    }
    if (job->cleanup) {
        epd_operation_scope_close(job->cleanup);
        epd_operation_scope_release(job->cleanup);
        job->cleanup = 0;
    }
}
bool operation_example_start(operation_example_t *job, epd_memory_owner_t task,
                             uint32_t generation, const void *bytes, size_t length,
                             uint32_t timeout_ticks)
{
    if (!job || job->operation || (!bytes && length) || length > sizeof(job->input) ||
        timeout_ticks > INT32_MAX) return false;
    epd_memory_owner_snapshot_t owner;
    if (!epd_memory_owner_get(task, &owner) || owner.closed || owner.kind != EPD_MEMORY_SCOPE_TASK) return false;
    epd_operation_spec_t spec = {0};
    if (!epd_operation_runtime_executors(&spec.execute, &spec.deliver, &spec.cleanup)) return false;
    memset(job, 0, sizeof(*job));
    if (!epd_operation_scope_open(task, generation, &job->business) ||
        !epd_operation_scope_open(task, generation, &job->cleanup)) {
        release_scopes(job); return false;
    }
    if (length) memcpy(job->input, bytes, length);
    job->length = length;
    spec.scope = job->business; spec.cleanup_scope = job->cleanup;
    spec.run = calculate; spec.receive = received; spec.user = job;
    spec.has_deadline = timeout_ticks != 0;
    spec.deadline_tick = (uint32_t)rt_tick_get() + timeout_ticks;
    if (epd_operation_submit(&spec, &job->operation)) return true;
    release_scopes(job); return false;
}
bool operation_example_cancel(operation_example_t *job)
{
    return job && job->operation && epd_operation_cancel(job->operation);
}
bool operation_example_finish(operation_example_t *job, uint32_t *checksum,
                              epd_operation_status_t *status)
{
    epd_operation_snapshot_t state;
    if (!job || !checksum || !status || !epd_operation_get(job->operation, &state) ||
        state.state != EPD_OPERATION_FINISHED) return false;
    *checksum = job->checksum; *status = state.status;
    epd_operation_release(job->operation); job->operation = 0;
    release_scopes(job); return true;
}
static void encode_u32(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (i * 8));
}
static uint32_t decode_u32(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}
bool operation_example_save_position(const epd_cold_state_spec_t *spec,
                                     uint32_t source_version, uint32_t position,
                                     epd_cold_state_t *out)
{
    unsigned char bytes[8]; encode_u32(bytes, source_version); encode_u32(bytes + 4, position);
    return epd_cold_state_put(spec, bytes, sizeof(bytes), out);
}
bool operation_example_restore_position(epd_cold_state_t state, uint32_t generation,
                                        uint32_t source_version, uint32_t *position)
{
    epd_cold_state_snapshot_t snapshot;
    unsigned char bytes[8]; size_t size;
    if (!position || !epd_cold_state_get(state, &snapshot) || snapshot.state != EPD_COLD_STATE_LOADED ||
        snapshot.instance_generation != generation || snapshot.size != sizeof(bytes) ||
        !epd_cold_state_copy(state, bytes, sizeof(bytes), &size) || size != sizeof(bytes) ||
        decode_u32(bytes) != source_version) return false;
    *position = decode_u32(bytes + 4); return true;
}
