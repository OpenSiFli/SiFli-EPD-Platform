#ifndef FONT_CACHE_HOST_RTTHREAD_H
#define FONT_CACHE_HOST_RTTHREAD_H

#include <assert.h>
#include <stdint.h>
#include <windows.h>

#define RT_TICK_PER_SECOND 1000
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 1
#define RT_ALIGN(n, a) (((n) + (a) - 1) & ~((uintptr_t)(a) - 1))

typedef intptr_t rt_base_t;
typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef uint32_t rt_tick_t;
typedef struct rt_thread *rt_thread_t;
struct rt_mutex {
    CRITICAL_SECTION native;
    unsigned depth, max_depth;
};

static inline int rt_mutex_init(struct rt_mutex *m, const char *name, rt_uint8_t policy)
{
    (void)name; (void)policy;
    InitializeCriticalSection(&m->native);
    m->depth = m->max_depth = 0;
    return 0;
}

static inline int rt_mutex_take(struct rt_mutex *m, int32_t timeout)
{
    assert(timeout == RT_WAITING_FOREVER);
    EnterCriticalSection(&m->native);
    if (++m->depth > m->max_depth) m->max_depth = m->depth;
    return 0;
}

static inline int rt_mutex_release(struct rt_mutex *m)
{
    assert(m->depth != 0);
    --m->depth;
    LeaveCriticalSection(&m->native);
    return 0;
}

static inline rt_thread_t rt_thread_self(void) { return (rt_thread_t)(uintptr_t)GetCurrentThreadId(); }

static inline rt_tick_t rt_tick_get(void)
{
    static volatile LONG ticks;
    return (rt_tick_t)InterlockedIncrement(&ticks);
}

#endif
