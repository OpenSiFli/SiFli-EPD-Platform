/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EPD_BSP_EPD_PMIC_H
#define EPD_BSP_EPD_PMIC_H

#include <stdbool.h>
#include <stdint.h>

static inline bool bsp_epd_pmic_supported(uint8_t revid)
{
    /* TI SLVSAQ8G, Table 19: VERSION is 5 for TPS65185 and 6 for TPS651851. */
    uint8_t version = revid & 0x0fu;
    return version == 5 || version == 6;
}

#endif
