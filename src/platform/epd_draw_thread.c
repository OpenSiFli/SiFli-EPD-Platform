#include "epd_memory.h"
#include <lvgl.h>
#include "src/osal/lv_os_private.h"
#include <string.h>

#if LV_USE_OS == LV_OS_RTTHREAD
lv_result_t __real_lv_thread_init(lv_thread_t *thread, const char *name,
                                  lv_thread_prio_t priority, void (*entry)(void *),
                                  size_t stack_size, void *user);

lv_result_t __wrap_lv_thread_init(lv_thread_t *thread, const char *name,
                                  lv_thread_prio_t priority, void (*entry)(void *),
                                  size_t stack_size, void *user)
{
    if (!name || strcmp(name, "swdraw"))
        return __real_lv_thread_init(thread, name, priority, entry, stack_size, user);
    /* Software vector rendering needs a larger CPU-only stack. */
    thread->thread = epd_thread_create_psram(name, entry, user, stack_size, priority, 20);
    if (!thread->thread) return LV_RESULT_INVALID;
    if (rt_thread_startup(thread->thread) == RT_EOK) return LV_RESULT_OK;
    rt_thread_delete(thread->thread);
    thread->thread = RT_NULL;
    return LV_RESULT_INVALID;
}
#endif
