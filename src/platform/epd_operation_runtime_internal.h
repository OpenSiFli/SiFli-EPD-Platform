/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_OPERATION_RUNTIME_INTERNAL_H
#define EPD_OPERATION_RUNTIME_INTERNAL_H
#include "epd_operation.h"

/* Resident bridge only; these functions are not dynamic-module exports. */
typedef struct {
    epd_memory_owner_t owner;
    uint32_t active;
} epd_operation_task_snapshot_t;
/* Bounded nonallocating snapshot, callable under the publication guard. */
size_t epd_operation_tasks(epd_operation_task_snapshot_t out[EPD_OPERATION_CAPACITY]);
void epd_operation_task_wake(epd_memory_owner_t owner);
/* 0: fact published, 1: wait for worker event, negative: admission failed. */
int epd_operation_runtime_task_prepare(epd_memory_owner_t owner);
#endif
