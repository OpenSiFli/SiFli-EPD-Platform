/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UNIFIED_TEST_MEM_MAP_H
#define UNIFIED_TEST_MEM_MAP_H
#include <stddef.h>
#include <stdint.h>
extern unsigned char unified_test_storage[3][32768];
#define HPSYS_RAM0_BASE ((uintptr_t)unified_test_storage[0])
#define HPSYS_RAM_SIZE (sizeof(unified_test_storage[0]))
#define PSRAM_BASE ((uintptr_t)unified_test_storage[1])
#define PSRAM_SIZE (sizeof(unified_test_storage[1]) * 2)
#define PSRAM_DATA_START_ADDR PSRAM_BASE
#define PSRAM_DATA_SIZE (sizeof(unified_test_storage[1]))
#define ACPU_PSRAM_START_ADDR ((uintptr_t)unified_test_storage[2])
#define ACPU_PSRAM_SIZE (sizeof(unified_test_storage[2]))
#endif
