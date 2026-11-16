/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DFU_BOARD_H
#define EPD_DFU_BOARD_H

#include <stdbool.h>

bool dfu_board_init(void);
void dfu_log(const char *text);
void dfu_reboot(void) __attribute__((noreturn));

#endif
