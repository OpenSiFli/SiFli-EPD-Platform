#include "rtthread.h"
static inline rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static inline void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
