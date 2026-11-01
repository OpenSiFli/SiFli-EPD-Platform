#ifndef CRF_FONT_DIAGNOSTICS_H
#define CRF_FONT_DIAGNOSTICS_H
#include <stdint.h>
#include "font_ftz.h"
/* Latest completed source session, retained even on ACPU images without a
 * console. An even sequence identifies a complete debugger/readback snapshot. */
typedef struct {
    volatile uint32_t sequence;
    crf_font_identity font;
    crf_stats engine;
    crf_ftz_stats ftz;
    uint32_t ftz_workspace, ticks_per_second;
} crf_session_diagnostics;
extern crf_session_diagnostics epd_reader_font_last_session;
#ifdef CRF_HOST_TEST
#include <time.h>
#include <stdio.h>
#define CRF_TICKS_PER_SECOND CLOCKS_PER_SEC
#define crf_log printf
static inline uint32_t crf_ticks(void) { return (uint32_t)clock(); }
#else
#include <rtthread.h>
#define CRF_TICKS_PER_SECOND RT_TICK_PER_SECOND
#define crf_log rt_kprintf
static inline uint32_t crf_ticks(void) { return (uint32_t)rt_tick_get(); }
#endif
/* Console totals saturate; in-memory counters retain 64 bits. */
static inline unsigned long crf_log32(uint64_t n)
{ return (unsigned long)(n > UINT32_MAX ? UINT32_MAX : n); }
#endif
