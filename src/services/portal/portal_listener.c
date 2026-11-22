/* SPDX-License-Identifier: Apache-2.0 */
#include "portal_listener.h"
#include <errno.h>
#include <string.h>
#include <lwip/sockets.h>

#define LISTEN_SLICE_MS 200u
#define MEMORY_RETRY_MS 1000u

void *portal_listener_memory_open(portal_listener_t *listener, size_t bytes)
{
    memset(listener, 0, sizeof(*listener));
    if (!bytes) return NULL;
    size_t psram = epd_app_allocation_peak(bytes);
    size_t sram = epd_app_allocation_peak(sizeof(struct rt_semaphore));
    if (psram == SIZE_MAX || sram == SIZE_MAX) return NULL;
    listener->work = epd_activity_work_open(psram, sram);
    if (!listener->work) return NULL;
    epd_activity_guard_t guard;
    void *buffer = NULL;
    if (epd_activity_work_enter(listener->work, &guard)) {
        buffer = epd_app_alloc(bytes, EPD_APP_PSRAM);
        epd_activity_work_leave(&guard);
    }
    if (!buffer) portal_listener_memory_close(listener, NULL);
    return buffer;
}

void portal_listener_memory_close(portal_listener_t *listener, void *buffer)
{
    epd_app_free(buffer);
    epd_activity_work_close(listener->work);
    memset(listener, 0, sizeof(*listener));
}

int portal_listener_wait(portal_listener_t *listener, int socket)
{
    epd_activity_guard_t guard;
    if (!epd_activity_work_enter(listener->work, &guard)) return -RT_ENOMEM;
    fd_set readfds;
    FD_ZERO(&readfds); FD_SET(socket, &readfds);
    struct timeval timeout = {0, LISTEN_SLICE_MS * 1000u};
    int ready = select(socket + 1, &readfds, NULL, NULL, &timeout);
    int error = errno;
    epd_activity_work_leave(&guard);
    if (ready >= 0) {
        listener->memory_failures = 0;
        return ready;
    }
    if (error != ENOMEM && error != -ENOMEM) return -RT_EIO;
    rt_tick_t now = rt_tick_get();
    if (!listener->memory_failures) listener->memory_started = now;
    if (++listener->memory_failures > MEMORY_RETRY_MS / LISTEN_SLICE_MS ||
        (rt_tick_t)(now - listener->memory_started) >= rt_tick_from_millisecond(MEMORY_RETRY_MS))
        return -RT_ENOMEM;
    /* select failed before its normal wait. Leave the credit before sleeping;
     * do not let service events turn the allocation failure into a busy loop. */
    rt_thread_mdelay(LISTEN_SLICE_MS);
    return 0;
}
