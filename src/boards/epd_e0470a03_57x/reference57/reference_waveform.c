/* SPDX-License-Identifier: Apache-2.0
 * E0470A03 only. 28 C is the validated table setting, not a sensor reading.
 */
#include <rtthread.h>
#include "mem_map.h"
#include "reference_waveform.h"
#include "epd_waveform_bin_reader.h"

#define TEXT_MAX_FRAMES 64
#define TEXT_ERASE_FRAMES 30

static int ready;
static int selected_mode = 3;
/* Prepared before HV; four two-bit drive codes share each cached byte. */
static uint8_t text_luts[TEXT_MAX_FRAMES][256 / 4];
static uint32_t text_frames;

static void text_lut_set(uint32_t frame, unsigned index, unsigned code)
{
    unsigned shift = (index & 3u) * 2u;
    uint8_t *packed = &text_luts[frame][index >> 2];
    *packed = (uint8_t)((*packed & ~(3u << shift)) | ((code & 3u) << shift));
}

static int prepare_text(void)
{
    uint32_t raw[256], gray_frames, du_frames, f, i;
    unsigned white_count[16] = {0}, black_count = 0;
    uint8_t white_code = 0, black_code = 0;
    int white_done = 0, black_done = 0;

    if (text_frames) return 0;
    gray_frames = waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)5);
    if (gray_frames < TEXT_ERASE_FRAMES + 2 || gray_frames > TEXT_MAX_FRAMES)
        return -RT_ERROR;
    for (f = 0; f < gray_frames; ++f)
    {
        waveform_bin_reader_fill_lut(raw, f);
        for (i = 0; i < 256; ++i)
        {
            unsigned old = i >> 4, next = i & 15;
            text_lut_set(f, i, old == next || next == 0 || next == 15
                              ? 0 : ((raw[i] >> 3) & 3));
        }
    }

    du_frames = waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)1);
    if (!du_frames || du_frames > gray_frames) return -RT_ERROR;
    for (f = 0; f < du_frames; ++f)
    {
        uint8_t white, black;
        waveform_bin_reader_fill_lut(raw, f);
        white = (raw[0x0f] >> 3) & 3;
        black = (raw[0xf0] >> 3) & 3;
        if (!f)
        {
            white_code = white; black_code = black;
            if (!white || !black || white == 3 || black == 3 || white == black)
                return -RT_ERROR;
        }
        /* The fitted pulse assumes a single leading DU pulse, then neutral. */
        if ((white && white_done) || (black && black_done)) return -RT_ERROR;
        if (!white) white_done = 1;
        if (!black) black_done = 1;
        if (black) ++black_count;
        for (i = 0; i < 16; ++i)
        {
            unsigned b = (raw[i << 4] >> 3) & 3;
            unsigned w = (raw[(i << 4) | 15] >> 3) & 3;
            if ((b && b != black_code) || (w && w != white_code)) return -RT_ERROR;
            if (i) text_lut_set(f, i << 4, b);
            if (w) ++white_count[i];
        }
    }
    if (white_count[0] != 18 || black_count != 18) return -RT_ERROR;
    /* Preserve the DU's relative erase duration for lighter old gray levels. */
    for (i = 0; i < 15; ++i)
    {
        unsigned count;
        if (!white_count[i] || white_count[i] > white_count[0]) return -RT_ERROR;
        count = (white_count[i] * TEXT_ERASE_FRAMES + white_count[0] / 2) / white_count[0];
        for (f = 0; f < count; ++f) text_lut_set(f, (i << 4) | 15, white_code);
    }
    text_frames = gray_frames;
    return 0;
}

void ref57_wave_init(void)
{
    text_frames = 0;
    selected_mode = 3;
    ready = waveform_bin_reader_init(CUSTOM_EPD_WAVE_TABLE_SIZE) == 0;
}

int ref57_wave_prepare_mode(int mode)
{
    selected_mode = -1;
    if (!ready) return -RT_ERROR;
    if (mode != 0 && mode != 1 && mode != 3 && mode != EPD_WAVE_TEXT)
        return -RT_EINVAL;
    selected_mode = mode;
    return mode == EPD_WAVE_TEXT ? prepare_text() : 0;
}

uint32_t ref57_wave_get_frames(void)
{
    if (!ready || selected_mode < 0) return 0;
    if (selected_mode == EPD_WAVE_TEXT) return text_frames;
    return waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)selected_mode);
}

void ref57_wave_fill_lut(uint32_t *lut, uint32_t frame_num)
{
    unsigned i;
    if (selected_mode == EPD_WAVE_TEXT)
    {
        if (frame_num >= text_frames)
        {
            for (i = 0; i < 256; ++i) lut[i] = 0xff000000u;
            return;
        }
        const uint8_t *codes = text_luts[frame_num];
        for (i = 0; i < 256; i += 4)
        {
            unsigned packed = *codes++;
            lut[i]     = 0xff000000u | (packed & 3u);
            lut[i + 1] = 0xff000000u | ((packed >> 2) & 3u);
            lut[i + 2] = 0xff000000u | ((packed >> 4) & 3u);
            lut[i + 3] = 0xff000000u | ((packed >> 6) & 3u);
        }
        return;
    }
    waveform_bin_reader_fill_lut(lut, frame_num);
    for (i = 0; i < 256; ++i)
    {
        /* Vendor bits 4:3 -> EPD_8BIT/F2_SWAP bits 1:0. */
        lut[i] = (lut[i] >> 3) | 0xff000000u;
        if (selected_mode == 1 && (i >> 4) == (i & 15)) lut[i] = 0xff000000u;
    }
}
