/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FRONTLIGHT_STORE_H
#define FRONTLIGHT_STORE_H
#include "frontlight.h"

typedef struct {
    uint32_t revision, load_revision;
    frontlight_store_status_t status;
    uint8_t percent;
    frontlight_mode_t mode;
    bool pending, failed;
} frontlight_store_snapshot_t;

/* UI producer, one resident storage worker, one replaceable pending snapshot. */
bool frontlight_store_load(void);
bool frontlight_store_save(uint8_t percent, frontlight_mode_t mode);
void frontlight_store_get(frontlight_store_snapshot_t *out);
#endif
