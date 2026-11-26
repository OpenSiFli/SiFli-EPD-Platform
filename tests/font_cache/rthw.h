#ifndef FONT_CACHE_HOST_RTHW_H
#define FONT_CACHE_HOST_RTHW_H

#include "rtthread.h"

static SRWLOCK font_irq_lock = SRWLOCK_INIT;
static inline rt_base_t rt_hw_interrupt_disable(void)
{
    AcquireSRWLockExclusive(&font_irq_lock);
    return 0;
}
static inline void rt_hw_interrupt_enable(rt_base_t level)
{
    (void)level;
    ReleaseSRWLockExclusive(&font_irq_lock);
}

#endif
