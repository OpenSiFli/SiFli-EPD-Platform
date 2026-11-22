#ifndef TEST_RTTHREAD_H
#define TEST_RTTHREAD_H
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <rtm.h>
#define rt_vsnprintf vsnprintf
#define RT_USING_CONSOLE 1
#define rt_malloc malloc
#define rt_calloc calloc
#define rt_free free
typedef intptr_t rt_base_t;
typedef int rt_bool_t;
typedef uint32_t rt_uint32_t;
typedef uint32_t rt_tick_t;
#define RT_TICK_PER_SECOND 1000
rt_tick_t rt_tick_get(void);
void rt_kprintf(const char *, ...);
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
#define RT_NULL NULL
rt_uint8_t rt_interrupt_get_nest(void);
rt_uint16_t rt_critical_level(void);
typedef struct rt_thread *rt_thread_t;
rt_thread_t rt_thread_self(void);
#ifdef COMPUTE_MEMORY_TEST
uint32_t rt_thread_mem_context_get(rt_thread_t);
int rt_thread_mem_context_set(rt_thread_t, uint32_t);
#endif
struct rt_semaphore { HANDLE handle; };
struct rt_thread { HANDLE handle; void (*entry)(void *); void *argument; };
#define RT_EOK 0
#define RT_ENOMEM 12
#define RT_ERROR 1
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_HIGH 10
#define RT_THREAD_TICK_DEFAULT 10
#define RT_ASSERT(x) assert(x)
#define ALIGN(x) __declspec(align(x))
#define INIT_APP_EXPORT(x)
int rt_sem_init(struct rt_semaphore *, const char *, unsigned, unsigned);
int rt_sem_take(struct rt_semaphore *, int);
int rt_sem_release(struct rt_semaphore *);
int rt_sem_detach(struct rt_semaphore *);
int rt_thread_init(rt_thread_t, const char *, void (*)(void *), void *, void *, size_t, unsigned, unsigned);
int rt_thread_startup(rt_thread_t);
int rt_thread_detach(rt_thread_t);
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t);
#endif
