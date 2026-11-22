/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "ui/platform/app_memory.h"
#include <rthw.h>
#include <stdint.h>

/* Share the SDK heap also used by images and the dynamic module loader. */
extern struct rt_memheap app_image_psram_memheap;

#include "epd_input_test.h"

#ifndef EPD_APP_PSRAM_BOUNDED_FIT
#define EPD_APP_PSRAM_BOUNDED_FIT 1
#endif
#ifndef EPD_APP_PSRAM_FIT_TRACE
#define EPD_APP_PSRAM_FIT_TRACE EPD_INPUT_TEST_ENABLED
#endif

#if !defined(RT_USING_MEMHEAP2) && (EPD_APP_PSRAM_BOUNDED_FIT || EPD_APP_PSRAM_FIT_TRACE)
unsigned rt_memheap_fit_flags(const struct rt_memheap *heap)
{
    if (heap != &app_image_psram_memheap) return 0;
    return (EPD_APP_PSRAM_BOUNDED_FIT ? RT_MEMHEAP_FIT_BOUNDED_16 : 0u) |
           (EPD_APP_PSRAM_FIT_TRACE ? RT_MEMHEAP_FIT_TRACE : 0u);
}
#endif

#if !defined(RT_USING_MEMHEAP2) && EPD_APP_PSRAM_FIT_TRACE
#include "bf0_hal.h"
#include <mem_section.h>

L2_RET_BSS_SECT_BEGIN(epd_app_psram_fit)
static struct {
    uint32_t samples, failed, changed, max_nodes, max_extra, max_cycles;
    uint64_t nodes, extra_nodes, cycles;
} app_psram_fit L2_RET_BSS_SECT(epd_app_psram_fit);
L2_RET_BSS_SECT_END

rt_uint32_t rt_memheap_fit_clock(void)
{
    return HAL_DBG_DWT_GetCycles();
}

void rt_memheap_fit_report(const struct rt_memheap *heap, const struct rt_memheap_fit_sample *sample)
{
    if (heap != &app_image_psram_memheap) return;
    rt_base_t level = rt_hw_interrupt_disable();
    ++app_psram_fit.samples;
    app_psram_fit.failed += !sample->allocated;
    app_psram_fit.changed += sample->changed;
    app_psram_fit.nodes += sample->nodes;
    app_psram_fit.extra_nodes += sample->extra_nodes;
    app_psram_fit.cycles += sample->lock_cycles;
    if (sample->nodes > app_psram_fit.max_nodes) app_psram_fit.max_nodes = sample->nodes;
    if (sample->extra_nodes > app_psram_fit.max_extra) app_psram_fit.max_extra = sample->extra_nodes;
    if (sample->lock_cycles > app_psram_fit.max_cycles) app_psram_fit.max_cycles = sample->lock_cycles;
    rt_hw_interrupt_enable(level);
}

static int app_psram_fit_init(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&app_psram_fit, 0, sizeof(app_psram_fit));
    rt_hw_interrupt_enable(level);
    if (!HAL_DBG_DWT_IsInit()) HAL_DBG_DWT_Init();
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(app_psram_fit_init);

#ifdef RT_USING_FINSH
#include <finsh.h>
static void mem_fit(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t samples = app_psram_fit.samples, failed = app_psram_fit.failed;
    uint32_t changed = app_psram_fit.changed, max_nodes = app_psram_fit.max_nodes;
    uint32_t max_extra = app_psram_fit.max_extra, max_cycles = app_psram_fit.max_cycles;
    uint64_t total_nodes = app_psram_fit.nodes, total_extra = app_psram_fit.extra_nodes;
    uint64_t total_cycles = app_psram_fit.cycles;
    rt_hw_interrupt_enable(level);
    uint32_t nodes = samples ? total_nodes / samples : 0;
    uint32_t extra = samples ? total_extra / samples : 0;
    uint32_t cycles = samples ? total_cycles / samples : 0;
    rt_kprintf("mem_fit policy=%u samples=%u failed=%u changed=%u nodes_avg=%u nodes_max=%u extra_avg=%u extra_max=%u lock_elapsed_cycles_avg=%u lock_elapsed_cycles_max=%u clock=%u\n",
        EPD_APP_PSRAM_BOUNDED_FIT, samples, failed, changed, nodes, max_nodes, extra, max_extra,
        cycles, max_cycles, (unsigned)HAL_DBG_DWT_IsInit());
}
MSH_CMD_EXPORT(mem_fit, Report application PSRAM placement statistics);
#endif
#endif

#if defined(RT_USING_HOOK) && !defined(RT_USING_MEMHEAP2)
void rt_memheap_alloc_failed_hook(struct rt_memheap *heap,
                                  const struct rt_memheap_alloc_failure *failure)
{
    rt_thread_t thread = rt_thread_self();
    bool image_psram = heap == &app_image_psram_memheap;
    bool wlan = thread && (rt_strncmp(thread->name, "aicwf", 5) == 0 ||
                           rt_strncmp(thread->name, "wlan_app", RT_NAME_MAX) == 0);
    if (!image_psram && !wlan) return;
    rt_err_t saved_errno = rt_get_errno();
    if (image_psram && failure->phase != RT_MEMHEAP_FAIL_LOCK) {
        size_t header = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
        app_memory_load_failed(failure->aligned > SIZE_MAX - header ? SIZE_MAX : failure->aligned + header);
    }
    const char *phase = failure->phase == RT_MEMHEAP_FAIL_SEARCH ? "search" :
                        failure->phase == RT_MEMHEAP_FAIL_TOTAL ? "total_gate" : "lock";
    rt_kprintf("[%s] alloc_fail heap=%p thread=%.*s tick=%u phase=%s "
               "requested=%u aligned=%u gate_free=%u snapshot=%u free=%u largest=%u "
               "blocks=%u free_sum=%u lock=%d\n",
        image_psram ? "psram" : "wlan", heap, RT_NAME_MAX, thread ? thread->name : "none",
        (unsigned)failure->tick, phase,
        (unsigned)failure->requested, (unsigned)failure->aligned, (unsigned)failure->gate_available,
        (unsigned)failure->snapshot_valid, (unsigned)failure->available, (unsigned)failure->largest,
        (unsigned)failure->free_blocks, (unsigned)failure->free_sum, (int)failure->lock_result);
    rt_set_errno(saved_errno);
}
#endif

void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    if (!size)
    {
        return RT_NULL;
    }
    return region == EPD_APP_PSRAM ?
           rt_memheap_alloc(&app_image_psram_memheap, size) : rt_malloc(size);
}

void *epd_app_realloc(void *memory, size_t size, epd_app_memory_t region)
{
    if (!size)
    {
        epd_app_free(memory);
        return RT_NULL;
    }
    if (!memory)
    {
        return epd_app_alloc(size, region);
    }
    if (region == EPD_APP_PSRAM)
    {
        return rt_memheap_realloc(&app_image_psram_memheap, memory, size);
    }
    return rt_realloc(memory, size);
}

void epd_app_free(void *memory)
{
    /* RT_USING_MEMHEAP_AS_HEAP: each allocation records its owning heap. */
    rt_free(memory);
}

size_t epd_app_psram_available(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    size_t available = app_image_psram_memheap.available_size;
    rt_hw_interrupt_enable(level);
    return available;
}

size_t epd_app_psram_capacity(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    size_t pool_size = app_image_psram_memheap.pool_size;
    rt_hw_interrupt_enable(level);
    /* Both RT-Thread memheap implementations reserve a head and a tail. */
    size_t overhead = 2u * RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    return pool_size > overhead ? pool_size - overhead : 0;
}

rt_thread_t epd_thread_create_psram(const char *name, void (*entry)(void *),
                                  void *parameter, rt_uint32_t stack_size,
                                  rt_uint8_t priority, rt_uint32_t tick)
{
    if (!name || !entry || stack_size < 256 || priority >= RT_THREAD_PRIORITY_MAX)
        return RT_NULL;
    void *stack = epd_app_alloc(stack_size, EPD_APP_PSRAM);
    if (!stack) return RT_NULL;
    rt_thread_t thread = rt_malloc(sizeof(*thread));
    if (!thread)
    {
        epd_app_free(stack);
        return RT_NULL;
    }
    if (rt_thread_init(thread, name, entry, parameter, stack, stack_size, priority, tick) != RT_EOK)
    {
        rt_thread_detach(thread);
        epd_app_free(stack);
        rt_free(thread);
        return RT_NULL;
    }
    /* Both allocations are heap-owned. Remove the static-object flag set by
     * rt_thread_init so the kernel's deferred deletion frees the stack and
     * control block; rt_free dispatches to each allocation's owning memheap. */
    rt_base_t level = rt_hw_interrupt_disable();
    thread->type &= ~RT_Object_Class_Static;
    rt_hw_interrupt_enable(level);
    return thread;
}

RTM_EXPORT(epd_app_alloc);
RTM_EXPORT(epd_app_realloc);
RTM_EXPORT(epd_app_free);
RTM_EXPORT(epd_app_psram_available);
RTM_EXPORT(epd_app_psram_capacity);
RTM_EXPORT(epd_thread_create_psram);
