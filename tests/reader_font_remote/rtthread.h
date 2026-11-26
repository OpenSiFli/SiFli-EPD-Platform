#ifndef READER_FONT_REMOTE_RTTHREAD_H
#define READER_FONT_REMOTE_RTTHREAD_H

#include <assert.h>
#include <stdint.h>
#include <windows.h>

typedef intptr_t rt_base_t;
typedef int rt_err_t;
typedef int32_t rt_int32_t;
typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef uint32_t rt_tick_t;
typedef struct test_thread *rt_thread_t;

#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 1
#define RT_TICK_PER_SECOND 1000
#define RT_THREAD_TICK_DEFAULT 10
#define RT_USING_THREAD_MEM_CONTEXT

struct rt_mutex { CRITICAL_SECTION cs; };
struct rt_semaphore { HANDLE event; };

rt_tick_t rt_tick_get(void);
void rt_kprintf(const char *format, ...);
int rt_mutex_init(struct rt_mutex *, const char *, int);
int rt_mutex_take(struct rt_mutex *, int);
int rt_mutex_release(struct rt_mutex *);
int rt_mutex_detach(struct rt_mutex *);
int rt_sem_init(struct rt_semaphore *, const char *, unsigned, int);
int rt_sem_take(struct rt_semaphore *, int);
int rt_sem_release(struct rt_semaphore *);
int rt_sem_detach(struct rt_semaphore *);
int rt_thread_startup(rt_thread_t);
int rt_thread_delete(rt_thread_t);

#endif
