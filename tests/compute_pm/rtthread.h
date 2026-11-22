#ifndef COMPUTE_PM_TEST_RTTHREAD_H
#define COMPUTE_PM_TEST_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
typedef int rt_bool_t;
typedef intptr_t rt_base_t;
typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef void *rt_thread_t;
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_EBUSY 7
#define INIT_ENV_EXPORT(fn) int test_pm_init(void) { return fn(); }
void rt_kprintf(const char *format, ...);
#endif
