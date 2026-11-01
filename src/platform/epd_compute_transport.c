/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_transport.h"
#ifdef EPD_COMPUTE_ACPU
#include <rthw.h>
#include "acpu_ctrl.h"
#include "epd_compute_context.h"
#include "epd_compute_protocol.h"
#include "epd_compute_park.h"
#include "bf0_hal.h"
#include "drv_flash.h"

static struct rt_semaphore compute_completion;
static rt_thread_t transport_owner;
static rt_bool_t transport_active, completion_leased;
static const epd_compute_wire_t *transport_request;
static epd_compute_park_t park_control;
static rt_bool_t park_ready;
static uint32_t startup_vtor;
static rt_bool_t startup_pending;

void __real_HAL_RCC_ReleaseACPU(uint32_t vtor);
void __wrap_HAL_RCC_ReleaseACPU(uint32_t vtor)
{
    /* Start the core only after the dispatcher has acquired the NOR lock. */
    RT_ASSERT(!park_ready && !startup_pending);
    startup_vtor = vtor;
    startup_pending = RT_TRUE;
}

static void wait_parked(void)
{
    rt_tick_t started = rt_tick_get();
    while (park_control.parked != park_control.requested)
    {
        __DMB();
        RT_ASSERT(rt_tick_get() - started < RT_TICK_PER_SECOND);
    }
    __DMB();
}

rt_sem_t __real_rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t __real_rt_sem_delete(rt_sem_t sem);
rt_err_t __real_rt_sem_take(rt_sem_t sem, rt_int32_t timeout);

static rt_bool_t owns_call(void)
{
    return rt_interrupt_get_nest() == 0 && rt_thread_self() == transport_owner &&
           transport_active;
}

rt_sem_t __wrap_rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    if (!owns_call()) return __real_rt_sem_create(name, value, flag);
    /* Only the dispatcher's synchronous SDK call borrows this static object. */
    RT_ASSERT(!completion_leased && value == 0 && flag == RT_IPC_FLAG_FIFO);
    RT_ASSERT(compute_completion.value == 0);
    completion_leased = RT_TRUE;
    return &compute_completion;
}

rt_err_t __wrap_rt_sem_delete(rt_sem_t sem)
{
    if (sem != &compute_completion) return __real_rt_sem_delete(sem);
    RT_ASSERT(owns_call() && completion_leased);
    RT_ASSERT(compute_completion.value == 0);
    completion_leased = RT_FALSE;
    return RT_EOK;
}

rt_err_t __wrap_rt_sem_take(rt_sem_t sem, rt_int32_t timeout)
{
    rt_err_t result = __real_rt_sem_take(sem, timeout);
    if (result != RT_EOK && sem == &compute_completion && owns_call() && transport_request)
    {
        const epd_compute_wire_t *r = transport_request;
        rt_kprintf("cmp transport wait_failed=%d cap=%u op=%u session=%u seq=%u completed=%u status=%d phase=%u\n",
                   result, (unsigned)r->capability, (unsigned)r->operation,
                   (unsigned)r->session, (unsigned)r->sequence,
                   (unsigned)r->completed_sequence, (int)r->status,
                   (unsigned)((*(volatile const uint32_t *)&r->diagnostics.flags & EPD_COMPUTE_PHASE_MASK) >> EPD_COMPUTE_PHASE_SHIFT));
    }
    return result;
}

int epd_compute_transport_init(rt_thread_t owner)
{
    if (!owner || transport_owner) return -RT_EBUSY;
    rt_err_t result = rt_sem_init(&compute_completion, "cmpr", 0, RT_IPC_FLAG_FIFO);
    if (result == RT_EOK) transport_owner = owner;
    return result;
}

rt_bool_t epd_compute_transport_busy(void)
{
    return transport_active;
}

rt_bool_t epd_compute_transport_parked(void)
{
    __DMB();
    return park_ready && !transport_active &&
           park_control.parked == park_control.requested;
}

void epd_compute_transport_start(void)
{
    RT_ASSERT(rt_thread_self() == transport_owner && !park_ready);
    rt_flash_lock(FLASH2_BASE_ADDR);
    transport_active = RT_TRUE;
    park_control.requested = 1;
    park_control.parked = 0;
    RT_ASSERT(startup_pending);
    __real_HAL_RCC_ReleaseACPU(startup_vtor);
    startup_pending = RT_FALSE;
    uint8_t error = ACPU_ERR_OK;
    acpu_run_task(ACPU_TASK_1, &park_control, sizeof(park_control), &error);
    RT_ASSERT(error == ACPU_ERR_OK);
    wait_parked();
    park_ready = RT_TRUE;
    transport_active = RT_FALSE;
    rt_flash_unlock(FLASH2_BASE_ADDR);
}

void epd_compute_transport_detach(void)
{
    RT_ASSERT(!transport_active && !completion_leased);
    if (transport_owner) rt_sem_detach(&compute_completion);
    transport_owner = RT_NULL;
    park_ready = RT_FALSE;
}

int epd_compute_transport_run(void *parameter, uint32_t size, uint8_t *error)
{
    if (!epd_compute_can_wait() || !transport_owner ||
        rt_thread_self() != transport_owner || transport_active)
        return -RT_EBUSY;
    RT_ASSERT(!completion_leased && compute_completion.value == 0);
    /* The ACPU code and font pack share this NOR with the filesystem. */
    rt_flash_lock(FLASH2_BASE_ADDR);
    transport_request = size == sizeof(epd_compute_wire_t) ? parameter : RT_NULL;
    transport_active = RT_TRUE;
    ++park_control.requested;
    __DSB();
    __SEV();
    acpu_run_task(ACPU_TASK_0, parameter, size, error);
    RT_ASSERT(!completion_leased && compute_completion.value == 0);
    /* The IPC reply precedes the final instructions that enter SRAM. */
    wait_parked();
    transport_active = RT_FALSE;
    transport_request = RT_NULL;
    rt_flash_unlock(FLASH2_BASE_ADDR);
    return RT_EOK;
}
#endif
