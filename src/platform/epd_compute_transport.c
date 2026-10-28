/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_transport.h"
#ifdef EPD_COMPUTE_ACPU
#include <rthw.h>
#include "acpu_ctrl.h"

static struct rt_semaphore compute_completion;
static rt_thread_t transport_owner;
static rt_bool_t transport_active, completion_leased;

rt_sem_t __real_rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t __real_rt_sem_delete(rt_sem_t sem);

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

int epd_compute_transport_init(rt_thread_t owner)
{
    if (!owner || transport_owner) return -RT_EBUSY;
    rt_err_t result = rt_sem_init(&compute_completion, "cmpr", 0, RT_IPC_FLAG_FIFO);
    if (result == RT_EOK) transport_owner = owner;
    return result;
}

void epd_compute_transport_detach(void)
{
    RT_ASSERT(!transport_active && !completion_leased);
    if (transport_owner) rt_sem_detach(&compute_completion);
    transport_owner = RT_NULL;
}

int epd_compute_transport_run(void *parameter, uint32_t size, uint8_t *error)
{
    if (!transport_owner || rt_interrupt_get_nest() ||
        rt_thread_self() != transport_owner || transport_active)
        return -RT_EBUSY;
    RT_ASSERT(!completion_leased && compute_completion.value == 0);
    transport_active = RT_TRUE;
    acpu_run_task(ACPU_TASK_0, parameter, size, error);
    RT_ASSERT(!completion_leased && compute_completion.value == 0);
    transport_active = RT_FALSE;
    return RT_EOK;
}
#endif
