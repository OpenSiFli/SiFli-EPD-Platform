#ifndef MEMORY_RETRY_FIXTURE_H
#define MEMORY_RETRY_FIXTURE_H
#include "../../src/platform/app_memory_pressure.h"
#ifndef RT_TICK_PER_SECOND
#define RT_TICK_PER_SECOND 1000u
#endif
#ifndef RT_ALIGN_SIZE
#define RT_ALIGN_SIZE 8u
#define RT_ALIGN(n,a) (((n)+(a)-1u)&~((uintptr_t)(a)-1u))
#endif
struct rt_memheap_item { uint32_t words[7]; };
static unsigned memory_requests, memory_waits;
static size_t memory_need;
static bool memory_pending;
static void (*memory_delay_hook)(void);
size_t epd_app_psram_capacity(void) { return 4u*1024u*1024u; }
uint32_t app_memory_pressure_allocation_failed(size_t bytes)
{
    ++memory_requests; memory_need=bytes; memory_pending=true;
    return memory_requests;
}
app_memory_pressure_status_t app_memory_pressure_query(void)
{
    app_memory_pressure_status_t s={0};
    s.generation=memory_requests; s.pending=memory_pending;
    s.result=memory_pending?APP_MEMORY_PRESSURE_PENDING:APP_MEMORY_PRESSURE_EXHAUSTED;
    s.min_available=memory_need; s.available=3000000;
    return s;
}
static void rt_thread_mdelay(unsigned milliseconds)
{
    (void)milliseconds; ++memory_waits;
    if(memory_delay_hook) memory_delay_hook(); else memory_pending=false;
}
#include "memory_retry.inc"
#endif
