/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DFU_DISPLAY_H
#define DFU_DISPLAY_H

#include <stdbool.h>

enum
{
    DFU_DISPLAY_WIDTH = 684,
    DFU_DISPLAY_HEIGHT = 1216,
    DFU_DISPLAY_CHAR_WIDTH = 24,
    DFU_DISPLAY_LINE_HEIGHT = 36,
};

/* Initializes the controller. The first refresh clears the panel and draws the canvas. */
int dfu_display_init(void);

/* Drawing uses portrait coordinates. Clear fills the canvas with white. */
void dfu_display_clear(void);
void dfu_display_text(int x, int y, const char *text);
void dfu_display_rect(int x, int y, int w, int h, bool black);

/* Calls service while refreshing, if supplied. Returns 0 after the panel is idle. */
int dfu_display_refresh(void (*service)(void));

#endif
