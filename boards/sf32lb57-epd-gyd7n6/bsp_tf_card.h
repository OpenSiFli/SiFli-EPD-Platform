/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_BSP_TF_CARD_H
#define EPD_BSP_TF_CARD_H

#include <stdbool.h>

#define BSP_TF_DETECT_PIN 51
#define BSP_TF_POWER_SETTLE_MS 10

void bsp_tf_card_set_power(bool enabled);
void bsp_tf_card_connect_bus(void);
void bsp_tf_card_setup_detect(void);

#endif
