/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_fiber.h"
#include "epd_compute_memory.h"
#include <rtthread.h>
#include <string.h>

#define FIBER_STACK_MIN 2048u
#define FIBER_STACK_MAX (96u * 1024u)
#define FIBER_SLICE_MS 4u

struct epd_compute_fiber {
    struct rt_thread thread;
    struct rt_semaphore resume, yielded;
    epd_compute_fiber_entry_t entry;
    void *context;
    const epd_compute_wire_t *request;
    rt_tick_t started;
    int32_t result;
    bool finished, closing, active;
    uint8_t stack[];
};

static void run(void *context)
{
    epd_compute_fiber_t *f = context;
    rt_sem_take(&f->resume, RT_WAITING_FOREVER);
    epd_compute_phase(f->request, EPD_COMPUTE_PHASE_FIBER_RUN);
    f->result = f->closing ? EPD_COMPUTE_CANCELLED : f->entry(f, f->context);
    f->finished = true;
    rt_sem_release(&f->yielded);
    /* CLOSE detaches this suspended static thread before releasing its stack. */
    for (;;) rt_sem_take(&f->resume, RT_WAITING_FOREVER);
}

epd_compute_fiber_t *epd_compute_fiber_create(const char *name, size_t stack_size,
    epd_compute_fiber_entry_t entry, void *context)
{
    if (!entry || stack_size < FIBER_STACK_MIN || stack_size > FIBER_STACK_MAX) return NULL;
    size_t offset = RT_ALIGN(sizeof(epd_compute_fiber_t), 8);
    epd_compute_fiber_t *f = epd_compute_calloc(1, offset + stack_size);
    if (!f) return NULL;
    f->entry = entry;
    f->context = context;
    if (rt_sem_init(&f->resume, "cmp_go", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto fail;
    if (rt_sem_init(&f->yielded, "cmp_yld", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto fail_resume;
    /* The two semaphores transfer ownership; the worker never touches shared
     * buffers between signalling yielded and consuming resume. */
    if (rt_thread_init(&f->thread, name, run, f, (uint8_t *)f + offset,
                       stack_size, RT_THREAD_PRIORITY_HIGH, 1) != RT_EOK) goto fail_yielded;
    if (rt_thread_startup(&f->thread) != RT_EOK) {
        rt_thread_detach(&f->thread);
        goto fail_yielded;
    }
    return f;
fail_yielded:
    rt_sem_detach(&f->yielded);
fail_resume:
    rt_sem_detach(&f->resume);
fail:
    epd_compute_free(f);
    return NULL;
}

int32_t epd_compute_fiber_step(epd_compute_fiber_t *f, const epd_compute_wire_t *request)
{
    if (!f || !request || f->active || f->closing) return EPD_COMPUTE_INVALID;
    if (f->finished) return f->result;
    f->active = true;
    f->request = request;
    f->started = rt_tick_get();
    epd_compute_phase(request, EPD_COMPUTE_PHASE_FIBER_WAIT);
    rt_sem_release(&f->resume);
    rt_sem_take(&f->yielded, RT_WAITING_FOREVER);
    epd_compute_phase(request, EPD_COMPUTE_PHASE_EXECUTE);
    f->request = NULL;
    f->active = false;
    return f->finished ? f->result : EPD_COMPUTE_MORE;
}

bool epd_compute_fiber_cancelled(const epd_compute_fiber_t *f)
{
    return !f || f->closing || !f->request ||
        (epd_compute_signals(f->request) & EPD_COMPUTE_CANCEL) != 0;
}

bool epd_compute_fiber_yield(epd_compute_fiber_t *f)
{
    if (epd_compute_fiber_cancelled(f)) return false;
    epd_compute_phase(f->request, EPD_COMPUTE_PHASE_FIBER_YIELD);
    rt_sem_release(&f->yielded);
    rt_sem_take(&f->resume, RT_WAITING_FOREVER);
    epd_compute_phase(f->request, EPD_COMPUTE_PHASE_FIBER_RUN);
    return !epd_compute_fiber_cancelled(f);
}

bool epd_compute_fiber_should_yield(const epd_compute_fiber_t *f)
{
    if (epd_compute_fiber_cancelled(f)) return true;
    rt_tick_t budget = rt_tick_from_millisecond(FIBER_SLICE_MS);
    return (epd_compute_signals(f->request) & EPD_COMPUTE_YIELD) ||
        (rt_tick_t)(rt_tick_get() - f->started) >= (budget ? budget : 1);
}

const epd_compute_wire_t *epd_compute_fiber_request(const epd_compute_fiber_t *f)
{ return f ? f->request : NULL; }

void epd_compute_fiber_close(epd_compute_fiber_t *f)
{
    if (!f) return;
    RT_ASSERT(!f->active);
    f->closing = true;
    if (!f->finished) {
        rt_sem_release(&f->resume);
        rt_sem_take(&f->yielded, RT_WAITING_FOREVER);
        RT_ASSERT(f->finished);
    }
    rt_thread_detach(&f->thread);
    rt_sem_detach(&f->resume);
    rt_sem_detach(&f->yielded);
    epd_compute_free(f);
}
