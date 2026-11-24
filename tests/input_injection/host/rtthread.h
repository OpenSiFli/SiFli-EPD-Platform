/* SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_RTTHREAD_H
#define TEST_RTTHREAD_H
typedef unsigned rt_base_t;
#define RT_EOK 0
#define RT_EFULL 3
#define RT_ENOSYS 6
#define RT_EINVAL 10
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
int rt_kprintf(const char *format, ...);
#endif
