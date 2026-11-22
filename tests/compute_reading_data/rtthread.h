#ifndef DATA_TEST_RTTHREAD_H
#define DATA_TEST_RTTHREAD_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define RT_ALIGN(n,a) (((n)+(a)-1)&~((size_t)(a)-1))
void host_fail(const char *, int, const char *);
#define CHECK(x) do { if (!(x)) host_fail(__FILE__, __LINE__, #x); } while (0)
#define RT_ASSERT(x) CHECK(x)
typedef uintptr_t rt_base_t;
void *rt_calloc(size_t, size_t);
void rt_free(void *);
void rt_kprintf(const char *, ...);
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t);
#endif
