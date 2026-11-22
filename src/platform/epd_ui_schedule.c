#include "epd_ui_schedule.h"
#include "ui/platform/epd_power.h"
#include <lvgl.h>
#include "src/misc/lv_timer_private.h"

static void timer_resumed(void *owner)
{
    /* The GUI thread rechecks its timers before it blocks. */
    if (rt_thread_self() != (rt_thread_t)owner) epd_power_notify();
}

static rt_err_t set_priority(rt_thread_t thread, rt_uint8_t priority)
{
    if (!thread) return -RT_ERROR;
    return rt_thread_control(thread, RT_THREAD_CTRL_CHANGE_PRIORITY, &priority);
}

rt_err_t epd_ui_schedule_start(void)
{
    rt_err_t result;
    /* Bluetooth (HIGH) and continuous audio (HIGH + 1) run before drawing. */
#if LV_USE_OS && (LV_USE_DRAW_SW || LV_USE_DRAW_EPIC)
    const rt_uint8_t draw_priority = RT_THREAD_PRIORITY_HIGH + 3;
#endif
#if LV_USE_OS && LV_USE_DRAW_SW
    /* All software draw workers use the SDK's fixed thread name. */
    struct rt_object_information *info = rt_object_get_information(RT_Object_Class_Thread);
    rt_list_t *node;
    unsigned workers = 0;
    rt_enter_critical();
    rt_list_for_each(node, &info->object_list)
    {
        rt_thread_t thread = (rt_thread_t)rt_list_entry(node, struct rt_object, list);
        if (rt_strncmp(thread->name, "swdraw", RT_NAME_MAX) != 0) continue;
        ++workers;
        result = set_priority(thread, draw_priority);
        if (result != RT_EOK)
        {
            rt_exit_critical();
            return result;
        }
    }
    rt_exit_critical();
    if (workers != LV_DRAW_SW_DRAW_UNIT_CNT) return -RT_ERROR;
#endif
#if LV_USE_OS && LV_USE_DRAW_EPIC
    result = set_priority(rt_thread_find("EPIC"), draw_priority);
    if (result != RT_EOK) return result;
#endif
    rt_thread_t owner = rt_thread_self();
    result = set_priority(owner, RT_THREAD_PRIORITY_MIDDLE);
    if (result == RT_EOK) lv_timer_handler_set_resume_cb(timer_resumed, owner);
    return result;
}

uint32_t epd_ui_schedule_next_delay(void)
{
    /* Deferred UI work can change timer readiness after lv_timer_handler(). */
    uint32_t delay = LV_NO_TIMER_READY;
    lv_lock();
    const uint32_t now = lv_tick_get();
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
    {
        if (lv_timer_get_paused(timer)) continue;
        const uint32_t elapsed = now - timer->last_run;
        if (elapsed >= timer->period)
        {
            delay = 0;
            break;
        }
        const uint32_t remaining = timer->period - elapsed;
        if (remaining < delay) delay = remaining;
    }
    lv_unlock();
    return delay;
}
