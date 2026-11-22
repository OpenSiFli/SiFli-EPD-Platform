/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DEMO_STATE_H
#define DEMO_STATE_H
#include "platform/epd_cold_runtime.h"

/* Module lifetime, independent of page allocations. All calls run on the GUI. */
typedef struct {
    epd_checkpoint_t checkpoint;
    uint32_t count;
} demo_state_t;
bool demo_state_open(demo_state_t *state);
bool demo_state_increment(demo_state_t *state);
#endif
