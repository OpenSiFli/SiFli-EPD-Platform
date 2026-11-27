/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_CONTEXT_H
#define EPD_COMPUTE_CONTEXT_H

#include <stdbool.h>
#include <rtthread.h>
#include <rthw.h>
#include "epd_compute.h"

/* Resident local components borrow the wait token without queuing an ACPU
 * session. Finish pairs with each successful begin and returns the terminal
 * admission status before a local handle may be published. */
int epd_compute_wait_begin_local(epd_compute_wait_t wait);
int epd_compute_wait_finish_local(epd_compute_wait_t wait, int status);

static inline bool epd_compute_can_wait(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool allowed = level == 0 && rt_interrupt_get_nest() == 0 &&
                   rt_critical_level() == 0 && rt_thread_self() != RT_NULL;
    rt_hw_interrupt_enable(level);
    return allowed;
}

#endif
