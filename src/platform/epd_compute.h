/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_H
#define EPD_COMPUTE_H

#include <stdbool.h>
#include <stddef.h>
#include "epd_compute_protocol.h"

typedef struct epd_compute_session *epd_compute_session_t;
typedef struct epd_compute_wait *epd_compute_wait_t;

/* After this many other batches, a queued request joins the oldest-first
 * service class. This bounds overtaking, not the duration of a remote batch. */
#define EPD_COMPUTE_MAX_BYPASSES 8u

typedef struct
{
    uint32_t index;
    epd_compute_registration_t registration;
    /* Dispatcher ready and board policy permits execution; inspect flags for
     * device validation separately. open rechecks the registered policy. */
    bool ready;
} epd_compute_capability_t;

typedef struct
{
    epd_compute_purpose_t purpose;
    uint32_t max_regions;
} epd_compute_requirements_t;

typedef struct
{
    const epd_compute_region_t *regions;
    uint32_t region_count;
    epd_compute_ref_t params;
    epd_compute_ref_t result;
    /* Checked by the submitting thread before enqueueing. During a wait, use
     * epd_compute_cancel() or epd_compute_wait_cancel() to signal cancellation. */
    bool (*cancelled)(void *context);
    void *context;
} epd_compute_request_t;

typedef struct
{
    void *data;
    void *allocation;
    uint32_t capacity;
} epd_compute_buffer_t;

int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *capability);
/* Power owner calls after restoring clocks and cancelling committed sleep. */
void epd_compute_resume_dispatch(void);
/* Synchronous operations require an ordinary thread with IRQs and scheduling
 * enabled. Invalid contexts are rejected before admitting or submitting work. */
/* Nonblocking admission (BUSY on conflict), followed by synchronous remote
 * open. Only mutually STEP_SHARED components may retain concurrent sessions.
 * Both open variants can return an error AND a non-NULL session when remote
 * OPEN acquired state but rollback CLOSE failed. That cancelled handle is
 * close-only: retain all OPEN buffers and retry epd_compute_close explicitly.
 * Only a NULL output or successful CLOSE releases the caller's ownership duty.
 * The return value preserves the OPEN error (or CANCELLED), not the CLOSE error. */
int epd_compute_open(const epd_compute_capability_t *capability,
                     const epd_compute_requirements_t *requirements,
                     const epd_compute_request_t *request, epd_compute_session_t *session);
/* Blocking admission, in arrival order, for a background/worker thread.
 * wait must be created before starting the call, so another thread can cancel
 * both admission and the remote open. A wait handle serves one call at a time;
 * cancellation is sticky. Keep it alive until open_wait has returned, and
 * synchronize all users before destroying it. A successful open transfers
 * cancellation responsibility to epd_compute_cancel(session). */
int epd_compute_wait_create(epd_compute_wait_t *wait);
void epd_compute_wait_cancel(epd_compute_wait_t wait);
int epd_compute_wait_destroy(epd_compute_wait_t wait);
int epd_compute_open_wait(const epd_compute_capability_t *capability,
                         const epd_compute_requirements_t *requirements,
                         const epd_compute_request_t *request,
                         epd_compute_wait_t wait, epd_compute_session_t *session);
/* Update the existing session, including queued/running work, without remote
 * reset or reopen. Normal order: continuous, foreground, background; aged
 * requests get a turn after MAX_BYPASSES. Component purpose policy is checked.
 * May race with step/reset/cancel, but the owner must keep the session alive. */
int epd_compute_set_purpose(epd_compute_session_t session, epd_compute_purpose_t purpose);
int epd_compute_step(epd_compute_session_t session, const epd_compute_request_t *request);
int epd_compute_reset(epd_compute_session_t session, const epd_compute_request_t *request);
/* Sticky cancellation may race with a step; the owner must keep session alive.
 * Queued work is removed immediately. Running work retains its buffers until
 * remote completion; cancellation is observed at component batch boundaries. */
void epd_compute_cancel(epd_compute_session_t session);
/* Call from the owner after its outstanding open/step has returned. A failure
 * retains the session and its borrowed buffers for a later explicit retry. */
int epd_compute_close(epd_compute_session_t session);
/* Transfer a failed-close session and ALL its borrowed buffers to resident
 * cleanup. Sole owner only, after outstanding calls/cancel users quiesce.
 * buffers must be a non-NULL array allocated with epd_app_alloc; each entry
 * must be from buffer_alloc. The array and session must never be used again.
 * Transfer allocates nothing, waits for nothing, and frees nothing. It is for
 * clients whose module/stack cannot retain a failed-close handle themselves. */
/* Pending cleanup blocks new OPENs of that component until CLOSE succeeds;
 * unrelated shared components may bypass its waiting admission requests. */
void epd_compute_close_later(epd_compute_session_t session, epd_compute_buffer_t *buffers, uint32_t count);
/* Attempt at most one resident-owned CLOSE. Failure retains all ownership;
 * success frees its buffers/array/session. Safe from a worker or recovery
 * event, not an interrupt. OPEN calls this once before admission. No timer,
 * polling loop, module callback or automatic retry-on-failure is installed. */
/* OK describes this attempt (or no eligible item), not an all-sessions drain. */
int epd_compute_retry_cleanup(void);

bool epd_compute_buffer_alloc(epd_compute_buffer_t *buffer, size_t size);
void epd_compute_buffer_free(epd_compute_buffer_t *buffer);
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *buffer,
                                              uint32_t length, uint32_t access);

#endif
