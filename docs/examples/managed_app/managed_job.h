/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MANAGED_JOB_H
#define MANAGED_JOB_H
#include "platform/epd_operation.h"
#include "platform/epd_resource_runtime.h"
#include "services/app_background.h"

typedef enum {
    DEMO_IDLE, DEMO_ACQUIRE, DEMO_FIRST, DEMO_RESIZE, DEMO_SECOND,
    DEMO_RELEASE, DEMO_DONE
} managed_job_phase_t;
typedef struct {
    epd_app_task_group_t group;
    epd_memory_owner_t task, execution;
    epd_operation_scope_t business, cleanup;
    epd_operation_t operation;
    epd_resource_activity_t capacity;
    uint64_t revision;
    uint32_t generation, checksum;
    unsigned char *buffer;
    size_t size, target, offset;
    managed_job_phase_t phase;
    epd_operation_status_t result;
    bool cancelled;
} managed_job_t;

/* UI lane only. Keep this zero-initialized model and poll entry alive after
 * page close. Accepted work owns a TASK until poll reports DEMO_DONE. */
bool managed_job_start(managed_job_t *job, uint32_t generation);
void managed_job_cancel(managed_job_t *job);
void managed_job_poll(managed_job_t *job);
bool managed_job_active(const managed_job_t *job);
#endif
