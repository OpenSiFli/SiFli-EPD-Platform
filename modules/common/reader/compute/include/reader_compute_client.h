/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_COMPUTE_CLIENT_H
#define READER_COMPUTE_CLIENT_H
#include "app_service.h"
#include "epd_reader_layout.h"
#include "platform/epd_app.h"

typedef struct reader_compute_job reader_compute_job_t;
typedef enum {
    READER_COMPUTE_IDLE, READER_COMPUTE_WAITING, READER_COMPUTE_RUNNING,
    READER_COMPUTE_READY, READER_COMPUTE_CLOSING, READER_COMPUTE_STOPPING, READER_COMPUTE_STOPPED
} reader_compute_state_t;
typedef struct {
    uint64_t calls, tx_bytes, rx_bytes, rx_payload, verified_replies;
} reader_compute_wire_stats_t;
/* UI owns one reference; the worker retains submitted jobs through completion. */
reader_compute_job_t *reader_compute_create(int *status);
epd_reader_batch_t *reader_compute_data(reader_compute_job_t *job);
void reader_compute_submit(reader_compute_job_t *job);
/* Explicit target pages use foreground admission; speculative pages/indexing
 * use background admission. Purpose changes keep the remote session and page
 * state. Creation/execution require a ready version-2 ACPU capability. */
void reader_compute_submit_target(reader_compute_job_t *job, bool foreground);
bool reader_compute_poll(reader_compute_job_t *job, int *status);
void reader_compute_release(reader_compute_job_t *job);
/* Consumes the caller's reference: do not poll, release or access job afterward.
 * For submitted jobs, the worker keeps its reference until completion. */
void reader_compute_cancel(reader_compute_job_t *job);
/* UI has no runnable or in-flight layout work. Release the remote session on
 * the existing worker; a new submit revokes this request. */
void reader_compute_idle(void);
/* Run one bounded batch on the existing document service, never the UI.
 * The service may also call this between published conversion records. */
bool reader_compute_worker_step(epd_service_t *service);
/* Nonblocking: stop submission and signal both admission and running work.
 * Call before joining a worker that may be blocked in admission. */
void reader_compute_stop(void);
/* Owner calls after step returns. OK means all jobs, session and buffers are
 * released. A failure retains ownership and must be retried before unloading.
 * A later worker_step starts a new lifecycle only after all ownership is gone. */
int reader_compute_shutdown(void);
reader_compute_state_t reader_compute_state(int *close_status);
/* Current/last session wire totals; a successful new OPEN resets them. */
void reader_compute_wire_stats(reader_compute_wire_stats_t *stats);
#endif
