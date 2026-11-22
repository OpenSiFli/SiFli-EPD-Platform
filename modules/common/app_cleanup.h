/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_APP_CLEANUP_H
#define EPD_APP_CLEANUP_H
#include "app_work.h"

/* Serialize access to work with the business mutex. The installed thread
 * context retains the cleanup owner until leave, independently of work. */
static inline bool app_work_cleanup_enter(const app_work_t *work, epd_memory_owner_t *previous)
{
    *previous = EPD_MEMORY_OWNER_INVALID;
    if (!work->cleanup) return true;
    epd_memory_owner_t owner = epd_service_owner_acquire(work->service, true);
    if (owner == EPD_MEMORY_OWNER_INVALID) return false;
    *previous = epd_memory_owner_enter(owner);
    epd_memory_owner_release(owner);
    return *previous != EPD_MEMORY_OWNER_INVALID;
}
static inline void app_work_cleanup_leave(epd_memory_owner_t previous)
{
    if (previous != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_leave(previous);
}
#endif
