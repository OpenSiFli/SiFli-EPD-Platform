/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_APP_WORK_H
#define EPD_APP_WORK_H
#include "app_service.h"

/* Serialize with the application's job/model mutex. One aggregate hold covers
 * accepted work until all overlapping accesses finish. Idle workers hold none. */
typedef struct { epd_service_t *service; bool normal, cleanup; } app_work_t;
static inline bool app_work_acquire(app_work_t *work, epd_service_t *service)
{
    if (!service || (work->service && work->service != service) ||
        !epd_service_reclaim_hold(service)) return false;
    if (!epd_service_sleep_acquire(service)) {
        epd_service_reclaim_release(service);
        return false;
    }
    /* Validate every admission against the latest policy, including work
     * joining an existing batch; retain only the aggregate lifetime reference. */
    if (work->normal) epd_service_reclaim_release(service);
    else { work->service = service; work->normal = true; }
    return true;
}
/* A flush callback grants one aggregate responsibility to finish existing data.
 * It does not admit another user command or change the background policy. */
static inline bool app_work_cleanup_acquire(app_work_t *work, epd_service_t *service)
{
    if (!service || (work->service && work->service != service)) return false;
    if (work->cleanup) return true;
    if (!epd_service_cleanup_hold(service)) return false;
    work->service = service;
    work->cleanup = true;
    return true;
}
/* A failed attempt may return its cleanup grant while ordinary work continues. */
static inline void app_work_cleanup_release(app_work_t *work)
{
    if (!work->cleanup) return;
    epd_service_t *service = work->service;
    work->cleanup = false;
    if (!work->normal) work->service = NULL;
    epd_service_cleanup_release(service);
}
static inline void app_work_release(app_work_t *work)
{
    epd_service_t *service = work->service;
    bool normal = work->normal, cleanup = work->cleanup;
    work->service = NULL;
    work->normal = work->cleanup = false;
    if (normal) epd_service_reclaim_release(service);
    if (cleanup) epd_service_cleanup_release(service);
}
#endif
