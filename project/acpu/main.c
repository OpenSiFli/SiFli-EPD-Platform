/* SPDX-License-Identifier: Apache-2.0 */
#include "acpu_ctrl.h"
#include "epd_compute_protocol.h"
#include "epd_compute_park.h"
#include "bf0_hal.h"
#include <rtthread.h>

static epd_compute_park_t *park_control;

/* The linker copies .data.* to SRAM before starting the ACPU. */
__attribute__((section(".data.epd_acpu_park"), noinline))
static void compute_park(epd_compute_park_t *control, uint32_t completed)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t tick = SysTick->CTRL;
    uint32_t scr = SCB->SCR;
    SysTick->CTRL = tick & ~(SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
    SCB->SCR = scr | SCB_SCR_SEVONPEND_Msk;
    __DSB();
    control->parked = completed;
    __DSB();
    while (control->requested == completed)
        __WFE();
    __DMB();
    SCB->SCR = scr;
    SysTick->CTRL = tick;
    __set_PRIMASK(primask);
}

void acpu_main(uint8_t task, void *parameter)
{
    if (task == ACPU_TASK_1)
        park_control = parameter;
    else
        RT_ASSERT(task == ACPU_TASK_0 && park_control);
    uint32_t completed = park_control->requested;
    if (task == ACPU_TASK_0)
        epd_compute_remote_dispatch(parameter);
    /* Component errors live in the shared result, not the IPC error field. */
    acpu_send_result(ACPU_ERR_OK, 0);
    compute_park(park_control, completed);
}
