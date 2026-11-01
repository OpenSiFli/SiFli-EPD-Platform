/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_PIPE_H
#define EPD_COMPUTE_PIPE_H
#include "epd_compute_protocol.h"

#define EPD_COMPUTE_PIPE_VERSION 1u
#define EPD_COMPUTE_PIPE_BYTES (16u * 1024u)
#define EPD_COMPUTE_PIPE_PATH 1024u

enum epd_compute_pipe_operation {
    EPD_PIPE_IDLE = 0, EPD_PIPE_OPEN, EPD_PIPE_READ, EPD_PIPE_STAT,
    EPD_PIPE_CLOSE, EPD_PIPE_DIR_OPEN, EPD_PIPE_DIR_NEXT, EPD_PIPE_DIR_CLOSE,
    EPD_PIPE_OUTPUT, EPD_PIPE_COMMAND, EPD_PIPE_DONE
};

/* Fixed-width IPC only. Handles are integers owned by the HCPU provider.
 * A request/response sequence pairs each yielded operation exactly once. */
typedef struct {
    uint32_t version, operation, sequence, response_sequence;
    int32_t status;
    uint32_t handle, length, flags;
    uint64_t offset, size;
    uint32_t values[8];
    char path[EPD_COMPUTE_PIPE_PATH];
} epd_compute_pipe_message_t;

#ifdef CORE_ACPU
#include "epd_compute_fiber.h"
typedef struct {
    epd_compute_pipe_message_t *message;
    uint8_t *data;
    epd_compute_fiber_t *fiber;
    uint32_t sequence;
} epd_compute_pipe_remote_t;
bool epd_compute_pipe_bind(epd_compute_pipe_remote_t *pipe, const epd_compute_wire_t *request);
bool epd_compute_pipe_validate(const epd_compute_pipe_remote_t *pipe, const epd_compute_wire_t *request);
bool epd_compute_pipe_exchange(epd_compute_pipe_remote_t *pipe, uint32_t operation);
bool epd_compute_pipe_checkpoint(epd_compute_pipe_remote_t *pipe);
#else
#include "epd_compute.h"
typedef struct epd_compute_pipe_host *epd_compute_pipe_host_t;
typedef int (*epd_compute_pipe_service_t)(void *user, epd_compute_pipe_message_t *, uint8_t *data);
int epd_compute_pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_pipe_host_t *out);
/* Both open variants may return an error with a non-NULL terminal pipe when
 * rollback CLOSE fails. Retain that handle and call pipe_shutdown until CLOSE
 * succeeds; no commands may be submitted to the terminal pipe.
 * Worker-only admission in the compute scheduler's waiting queue. Create and
 * publish wait before this call so cancellation can use epd_compute_wait_cancel
 * while *out is still NULL. The caller owns wait until this call returns; then
 * destroy it and use pipe_cancel for an admitted pipe. Check the caller's stop
 * state after return to cover cancellation concurrent with ownership transfer.
 * The ordinary open retains nonblocking BUSY admission for UI callers. */
int epd_compute_pipe_open_wait(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_wait_t wait, epd_compute_pipe_host_t *out);
int epd_compute_pipe_step(epd_compute_pipe_host_t pipe, epd_compute_pipe_service_t service, void *user);
int epd_compute_pipe_command(epd_compute_pipe_host_t pipe, const epd_compute_pipe_message_t *message,
    const void *data, size_t length);
const epd_compute_pipe_message_t *epd_compute_pipe_result(epd_compute_pipe_host_t pipe);
const uint8_t *epd_compute_pipe_payload(epd_compute_pipe_host_t pipe);
void epd_compute_pipe_cancel(epd_compute_pipe_host_t pipe);
int epd_compute_pipe_purpose(epd_compute_pipe_host_t pipe, epd_compute_purpose_t purpose);
void epd_compute_pipe_close(epd_compute_pipe_host_t pipe);
/* Owner calls after step/command and other cancellation callers have returned.
 * Cancel then attempt CLOSE once. Failure preserves the handle and buffers for
 * cleanup retry; success clears the handle. An already NULL handle succeeds. */
int epd_compute_pipe_shutdown(epd_compute_pipe_host_t *pipe);
#endif
#endif
