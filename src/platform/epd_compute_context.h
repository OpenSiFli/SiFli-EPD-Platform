/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_CONTEXT_H
#define EPD_COMPUTE_CONTEXT_H

#include <stdbool.h>
#include <rtthread.h>
#include <rthw.h>

static inline bool epd_compute_can_wait(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool allowed = level == 0 && rt_interrupt_get_nest() == 0 &&
                   rt_critical_level() == 0 && rt_thread_self() != RT_NULL;
    rt_hw_interrupt_enable(level);
    return allowed;
}

#endif
