/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BUSINESS_CLEANUP_PORTS_H
#define BUSINESS_CLEANUP_PORTS_H
#ifndef EPD_MEMORY_MANAGER_H
typedef uint32_t epd_memory_owner_t;
#define EPD_MEMORY_OWNER_INVALID UINT32_MAX
#endif
static unsigned test_cleanup_holds, test_cleanup_references;
static bool test_cleanup_admit = true, test_cleanup_enter_fail;
static epd_memory_owner_t test_cleanup_context = 1;
bool epd_service_cleanup_hold(epd_service_t *service)
{
    if (!test_cleanup_admit || !epd_service_sleep_acquire(service)) return false;
    ++test_cleanup_holds; return true;
}
void epd_service_cleanup_release(epd_service_t *service)
{
    if (!service || !test_cleanup_holds) abort();
    --test_cleanup_holds;
}
epd_memory_owner_t epd_service_owner_acquire(epd_service_t *service, bool cleanup)
{
    if (!service || !cleanup || !test_cleanup_holds) return EPD_MEMORY_OWNER_INVALID;
    ++test_cleanup_references; return 2;
}
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{
    if (owner != 2 || test_cleanup_enter_fail) return EPD_MEMORY_OWNER_INVALID;
    epd_memory_owner_t previous = test_cleanup_context;
    test_cleanup_context = owner; return previous;
}
void epd_memory_owner_leave(epd_memory_owner_t previous) { test_cleanup_context = previous; }
void epd_memory_owner_release(epd_memory_owner_t owner)
{
    if (owner != 2 || !test_cleanup_references) abort();
    --test_cleanup_references;
}
#endif
