#ifndef HOST_RTTHREAD_H
#define HOST_RTTHREAD_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define rt_kprintf printf
#define RT_ERROR 1
#define RT_EINVAL 22
#define RT_ASSERT(x) do { if (!(x)) abort(); } while (0)
typedef int rt_base_t;
typedef int rt_err_t;
typedef int32_t rt_int32_t;
#endif
