/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_OPERATION_TEST_RTHW_H
#define EPD_OPERATION_TEST_RTHW_H

#include <rtthread.h>

rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t previous);

#endif
