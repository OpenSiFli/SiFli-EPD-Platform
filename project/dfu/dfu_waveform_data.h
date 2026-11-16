/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Generated E0470A03 INIT and DU frames. */
#ifndef DFU_WAVEFORM_DATA_H
#define DFU_WAVEFORM_DATA_H

#include <stdint.h>

typedef struct { uint8_t frames, codes; } dfu_wave_run_t;
typedef struct { int8_t minimum; uint8_t count; uint16_t offset; } dfu_wave_zone_t;

static const dfu_wave_run_t dfu_wave_runs[] = {
    { 72, 0x55 },
    { 3, 0x00 },
    { 72, 0xaa },
    { 2, 0x00 },
    { 56, 0x55 },
    { 3, 0x00 },
    { 56, 0xaa },
    { 2, 0x00 },
    { 42, 0x55 },
    { 3, 0x00 },
    { 42, 0xaa },
    { 2, 0x00 },
    { 30, 0x55 },
    { 3, 0x00 },
    { 30, 0xaa },
    { 2, 0x00 },
    { 24, 0x55 },
    { 3, 0x00 },
    { 24, 0xaa },
    { 2, 0x00 },
    { 21, 0x55 },
    { 3, 0x00 },
    { 21, 0xaa },
    { 2, 0x00 },
    { 18, 0x55 },
    { 3, 0x00 },
    { 18, 0xaa },
    { 2, 0x00 },
    { 15, 0x55 },
    { 3, 0x00 },
    { 15, 0xaa },
    { 2, 0x00 },
    { 12, 0x55 },
    { 3, 0x00 },
    { 12, 0xaa },
    { 2, 0x00 },
    { 72, 0x18 },
    { 2, 0x00 },
    { 56, 0x18 },
    { 2, 0x00 },
    { 42, 0x18 },
    { 2, 0x00 },
    { 30, 0x18 },
    { 2, 0x00 },
    { 24, 0x18 },
    { 2, 0x00 },
    { 21, 0x18 },
    { 2, 0x00 },
    { 18, 0x18 },
    { 2, 0x00 },
    { 12, 0x18 },
    { 2, 0x00 },
};

static const dfu_wave_zone_t dfu_wave_init[] = {
    { -128, 4, 0 },
    { 0, 4, 0 },
    { 3, 4, 4 },
    { 6, 4, 8 },
    { 9, 4, 12 },
    { 12, 4, 12 },
    { 15, 4, 16 },
    { 18, 4, 20 },
    { 21, 4, 24 },
    { 24, 4, 24 },
    { 27, 4, 24 },
    { 30, 4, 28 },
    { 33, 4, 32 },
    { 38, 4, 32 },
};

static const dfu_wave_zone_t dfu_wave_du[] = {
    { -128, 2, 36 },
    { 0, 2, 36 },
    { 3, 2, 38 },
    { 6, 2, 40 },
    { 9, 2, 42 },
    { 12, 2, 42 },
    { 15, 2, 44 },
    { 18, 2, 46 },
    { 21, 2, 48 },
    { 24, 2, 48 },
    { 27, 2, 48 },
    { 30, 2, 48 },
    { 33, 2, 48 },
    { 38, 2, 50 },
};

#endif
