/* SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#include "epd_input_test.h"
#include <stdio.h>
_Static_assert(EPD_INPUT_TEST_ENABLED == 0, "Input injection must default to off");
int main(void) { puts("PASS default off: links without LVGL, RTOS or shell"); return 0; }
