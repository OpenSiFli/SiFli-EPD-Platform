#pragma once
#include "rtthread.h"
#include "mem_section.h"
#define RT_EOK 0
#define RT_ENOMEM 12
#define RT_ENOSYS 38
#define RTM_EXPORT(name)
void *rt_thread_self(void);
void *rt_thread_find(const char *name);
