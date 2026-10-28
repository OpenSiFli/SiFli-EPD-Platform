/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_H
#define EPD_COMPUTE_H

#include <stdbool.h>
#include <stddef.h>
#include "epd_compute_protocol.h"

typedef struct epd_compute_session *epd_compute_session_t;

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
    /* Executed only by the submitting thread during synchronous waits. */
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
int epd_compute_open(const epd_compute_capability_t *capability,
                     const epd_compute_requirements_t *requirements,
                     const epd_compute_request_t *request, epd_compute_session_t *session);
int epd_compute_step(epd_compute_session_t session, const epd_compute_request_t *request);
int epd_compute_reset(epd_compute_session_t session, const epd_compute_request_t *request);
/* Cancellation may race with a step; the owner must keep session alive. */
void epd_compute_cancel(epd_compute_session_t session);
/* Call from the owner after its outstanding step has returned. */
int epd_compute_close(epd_compute_session_t session);

bool epd_compute_buffer_alloc(epd_compute_buffer_t *buffer, size_t size);
void epd_compute_buffer_free(epd_compute_buffer_t *buffer);
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *buffer,
                                              uint32_t length, uint32_t access);

#endif
