/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EPD_DFU_MEDIA_H
#define EPD_DFU_MEDIA_H

#include <stdbool.h>
#include "ff.h"

#define DFU_MEDIA_ROOT "0:/"

typedef enum
{
    DFU_KEY_NONE = 0,
    DFU_KEY1,
    DFU_KEY2,
    DFU_KEY3,
} dfu_key_t;

/* Keep GPIO1_IRQn masked. The foreground polls the removal edge and pin level. */
bool dfu_media_present(void);
bool dfu_media_mounted(void);
/* Mount drive 0 immediately. Close old handles before remounting. */
FRESULT dfu_media_mount(void);
void dfu_media_unmount(void);

bool dfu_keys_init(void);
/* Poll regularly. A debounced press produces one event after release of the previous key. */
dfu_key_t dfu_key_poll(void);

#endif
