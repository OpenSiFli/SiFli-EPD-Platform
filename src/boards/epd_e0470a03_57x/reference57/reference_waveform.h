/* SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef READER57_EPD_WAVEFORM_H
#define READER57_EPD_WAVEFORM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Calibrated text/menu composite; not an index in the vendor BIN. */
#define EPD_WAVE_TEXT 16
void ref57_wave_init(void);
int ref57_wave_prepare_mode(int mode);
uint32_t ref57_wave_get_frames(void);
void ref57_wave_fill_lut(uint32_t *lut, uint32_t frame_num);

#ifdef __cplusplus
}
#endif
#endif
