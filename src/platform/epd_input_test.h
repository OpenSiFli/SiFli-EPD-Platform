/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_INPUT_TEST_H
#define EPD_INPUT_TEST_H

/* Enable explicitly in the firmware compiler definitions. No shell command,
 * storage or input hook is emitted by the default build. */
#ifndef EPD_INPUT_TEST_ENABLED
#define EPD_INPUT_TEST_ENABLED 0
#endif
#if EPD_INPUT_TEST_ENABLED != 0 && EPD_INPUT_TEST_ENABLED != 1
#error EPD_INPUT_TEST_ENABLED must be 0 or 1
#endif

#if EPD_INPUT_TEST_ENABLED
#include <stdbool.h>
#include <stdint.h>

#define EPD_INPUT_TEST_CAPACITY 4u
typedef enum {
    EPD_INPUT_TEST_TAP,
    EPD_INPUT_TEST_HOLD,
    EPD_INPUT_TEST_SWIPE
} epd_input_test_kind_t;
typedef struct {
    epd_input_test_kind_t kind;
    uint16_t x, y, end_x, end_y;
    uint32_t duration_ms; /* 40..10000; tap/hold require identical endpoints. */
    uint32_t end_hold_ms; /* Swipe only; duration + end hold must be <= 10000. */
} epd_input_test_request_t;

/* Resident test firmware only; none of these functions is a module export.
 * UI thread, before installing shell/navigation pointer observers. */
bool epd_input_test_init(void);
/* Thread-safe bounded enqueue. Coordinates are current display coordinates,
 * after rotation. Bounds and an idle pointer are checked on the UI thread.
 * Returns RT_EOK, -RT_EINVAL, -RT_EFULL or -RT_ENOSYS. */
int epd_input_test_submit(const epd_input_test_request_t *request, uint32_t *id);
/* UI thread: replace the ordinary physical-key processing call. Clears queued
 * key edges throughout a request, including its final released sample. */
void epd_input_test_process_keys(void);
#endif
#endif
