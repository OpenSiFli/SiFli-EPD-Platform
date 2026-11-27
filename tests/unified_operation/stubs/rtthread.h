/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_OPERATION_TEST_RTTHREAD_H
#define EPD_OPERATION_TEST_RTTHREAD_H

#include <stddef.h>
#include <stdint.h>

typedef intptr_t rt_base_t;
typedef int rt_err_t;
typedef int32_t rt_int32_t;
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef struct rt_thread *rt_thread_t;

struct rt_thread { unsigned id; void (*entry)(void *); void *parameter; };
struct rt_event { uint32_t bits; unsigned sends; };
struct rt_mutex { unsigned held; };
struct rt_semaphore { unsigned count; };
struct rt_object_global_scope { rt_thread_t thread; struct rt_object_global_scope *previous; };

#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 1
#define RT_EVENT_FLAG_OR 0x02
#define RT_EVENT_FLAG_CLEAR 0x04
#ifndef RT_TICK_PER_SECOND
#define RT_TICK_PER_SECOND 1000
#endif

#define RT_USING_THREAD_MEM_CONTEXT
#define RT_USING_MODULE_OBJECT_CONTEXT
#define INIT_COMPONENT_EXPORT(fn) int test_init_##fn(void) { return fn(); }

rt_thread_t rt_thread_self(void);
uint32_t rt_tick_get(void);
rt_uint8_t rt_interrupt_get_nest(void);
rt_uint16_t rt_critical_level(void);
void rt_enter_critical(void);
void rt_exit_critical(void);
void rt_thread_yield(void);
int rt_thread_startup(rt_thread_t thread);
int rt_thread_delete(rt_thread_t thread);
int rt_event_init(struct rt_event *event, const char *name, int flags);
int rt_event_detach(struct rt_event *event);
int rt_event_send(struct rt_event *event, uint32_t bits);
int rt_event_recv(struct rt_event *event, uint32_t bits, int flags, int32_t wait, uint32_t *out);
int rt_mutex_init(struct rt_mutex *mutex, const char *name, int flags);
int rt_mutex_detach(struct rt_mutex *mutex);
int rt_mutex_take(struct rt_mutex *mutex, int wait);
int rt_mutex_release(struct rt_mutex *mutex);
int rt_sem_init(struct rt_semaphore *sem, const char *name, unsigned count, int flags);
int rt_sem_detach(struct rt_semaphore *sem);
int rt_sem_take(struct rt_semaphore *sem, int wait);
int rt_sem_release(struct rt_semaphore *sem);
int rt_object_global_scope_enter(struct rt_object_global_scope *scope);
int rt_object_global_scope_leave(struct rt_object_global_scope *scope);

#endif
