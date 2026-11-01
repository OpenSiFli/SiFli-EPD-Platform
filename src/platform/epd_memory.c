/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include <rthw.h>

/* Share the SDK heap also used by images and the dynamic module loader. */
extern struct rt_memheap app_image_psram_memheap;

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
