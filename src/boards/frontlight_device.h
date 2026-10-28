/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_FRONTLIGHT_DEVICE_H
#define EPD_FRONTLIGHT_DEVICE_H

typedef enum {
    FRONTLIGHT_MIXED = 0,
    FRONTLIGHT_COOL,
    FRONTLIGHT_WARM,
    FRONTLIGHT_MODE_COUNT
} frontlight_mode_t;

/* lcdlight control arguments point to frontlight_mode_t. */
#define FRONTLIGHT_CTRL_GET_MODE 0x1001
#define FRONTLIGHT_CTRL_SET_MODE 0x1002

#endif
