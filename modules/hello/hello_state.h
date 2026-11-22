/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HELLO_STATE_H
#define HELLO_STATE_H
#include "platform/epd_cold_runtime.h"

/* Module lifetime, independent of page allocations. All calls run on the GUI. */
typedef struct {
    epd_checkpoint_t checkpoint;
    uint32_t count;
} hello_state_t;
bool hello_state_open(hello_state_t *state);
bool hello_state_increment(hello_state_t *state);
#endif
