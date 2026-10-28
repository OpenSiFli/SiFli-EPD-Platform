/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_COMPUTE_CLIENT_H
#define READER_COMPUTE_CLIENT_H
#include "app_service.h"
#include "epd_reader_layout.h"
#include "platform/epd_app.h"

typedef struct reader_compute_job reader_compute_job_t;
/* UI owns one reference; the worker retains submitted jobs through completion. */
reader_compute_job_t *reader_compute_create(int *status);
epd_reader_batch_t *reader_compute_data(reader_compute_job_t *job);
void reader_compute_submit(reader_compute_job_t *job);
/* Explicit target pages use foreground admission; speculative pages/indexing
 * use background admission. Changing purpose reopens the remote session only
 * between completed batches. The local fallback algorithm is unchanged. */
void reader_compute_submit_target(reader_compute_job_t *job, bool foreground);
bool reader_compute_poll(reader_compute_job_t *job, int *status);
void reader_compute_release(reader_compute_job_t *job);
/* Consumes the caller's reference: do not poll, release or access job afterward.
 * For submitted jobs, the worker keeps its reference until completion. */
void reader_compute_cancel(reader_compute_job_t *job);
/* UI has no runnable or in-flight layout work. Retain buffers but release the
 * remote session on the existing worker; a new submit revokes this request. */
void reader_compute_idle(void);
/* Run one bounded batch on the existing document service, never the UI.
 * The service may also call this between published conversion records. */
bool reader_compute_worker_step(epd_service_t *service);
void reader_compute_shutdown(void);
#endif
