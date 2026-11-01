/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_TIMING_PORT_H
#define BOOK_DOCUMENT_TIMING_PORT_H
#if defined(BOOK_DOCUMENT_TIMING_TEST)
uint32_t book_document_clock(void);
#define BOOK_DOCUMENT_CLOCK_HZ 1000u
#elif defined(__arm__)
/* rtthread.h in mobi_include is a vendor-only stub. Use the SDK types and
 * the public tick declaration without changing that include path. */
#include <rtdef.h>
extern rt_tick_t rt_tick_get(void);
static uint32_t book_document_clock(void) { return (uint32_t)rt_tick_get(); }
#define BOOK_DOCUMENT_CLOCK_HZ RT_TICK_PER_SECOND
#else
#include <time.h>
static uint32_t book_document_clock(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}
#define BOOK_DOCUMENT_CLOCK_HZ 1000u
#endif
#endif
