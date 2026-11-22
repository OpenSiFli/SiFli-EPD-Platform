/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UNIFIED_TEST_RTCONFIG_H
#define UNIFIED_TEST_RTCONFIG_H
#include <stdint.h>
#define RT_USING_ARCH_DATA_TYPE
typedef int8_t rt_int8_t;
typedef int16_t rt_int16_t;
typedef int32_t rt_int32_t;
typedef int64_t rt_int64_t;
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
#define RT_NAME_MAX 16
#define RT_ALIGN_SIZE 4
#define RT_THREAD_PRIORITY_MAX 32
#ifndef RT_TICK_PER_SECOND
#define RT_TICK_PER_SECOND 1000
#endif
#define RT_USING_SEMAPHORE
#define RT_USING_MEMHEAP
#define RT_USING_MEMHEAP_AS_HEAP
#define RT_USING_HEAP
#ifndef EPD_MEMORY_TEST_HOOK_DISABLED
#define RT_USING_HOOK
#endif
#define RT_USING_SIGNALS
#define RT_USING_CONSOLE
#define RT_DEBUG
#define RT_DEBUG_CONTEXT_CHECK 0
#ifndef EPD_MEMORY_TEST_OWNER_DISABLED
#define RT_USING_MEMHEAP_OWNER
#endif
#define RT_USING_THREAD_MEM_CONTEXT
#define TINY_TTF_CACHE_IN_APP_PSRAM
#endif
