/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UNIFIED_FREESTANDING_STRING_H
#define UNIFIED_FREESTANDING_STRING_H
#include <stddef.h>
void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
#endif
