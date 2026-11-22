/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PORTAL_LISTENER_H
#define EPD_PORTAL_LISTENER_H
#include "epd_activity_work.h"

typedef struct {
    epd_activity_work_t *work;
    rt_tick_t memory_started;
    unsigned memory_failures;
} portal_listener_t;

/* Worker-owned connection storage and the transient select semaphore share one
 * admitted lifetime. Request processing uses its own additional working set. */
void *portal_listener_memory_open(portal_listener_t *listener, size_t bytes);
void portal_listener_memory_close(portal_listener_t *listener, void *buffer);
/* One 200 ms listening slice. Temporary ENOMEM retains the socket and waits at
 * the same cadence, for at most one second. The caller checks cancellation and
 * admission after every slice. Negative results end this listening attempt. */
int portal_listener_wait(portal_listener_t *listener, int socket);
#endif
