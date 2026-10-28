/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rthw.h>
#include "../epd_waveform.h"
#include "reference_waveform.h"

#define MODE_QUEUE_SIZE 2u
static EpdDrawMode modes[MODE_QUEUE_SIZE];
static unsigned head, count, since_full;
static int period = 10, forced, manual = -1;
static int displayed;

void epd_wave_table(void)
{
    head = count = since_full = 0;
    forced = displayed = 0;
    manual = -1;
    ref57_wave_init();
}

void epd_wave_set_part_times(int value)
{
    rt_base_t level = rt_hw_interrupt_disable();
    value = value > 0 ? value : 1;
    if (period != value) { period = value; since_full = 0; }
    rt_hw_interrupt_enable(level);
}

int epd_wave_get_part_times(void) { return period; }
int epd_wave_get_mode(void) { return manual; }
void epd_wave_set_mode(int mode)
{
    if (mode == -1 || mode == 0 || mode == 1 || mode == 3 || mode == EPD_WAVE_TEXT)
        manual = mode;
}

/* The composite is fitted to the BIN's [27,30) table, selected at 28 C. */
int epd_wave_get_tempzone(void) { return 10; }
void epd_wave_set_tempzone(int zone) { (void)zone; }

void epd_wave_request_full(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    forced = 1;
    rt_hw_interrupt_enable(level);
}

void epd_wave_submit_frame(EpdDrawMode mode)
{
    rt_base_t level = rt_hw_interrupt_disable();
    RT_ASSERT(count < MODE_QUEUE_SIZE);
    if (forced) { mode = EPD_DRAW_MODE_FULL; forced = 0; }
    modes[(head + count) % MODE_QUEUE_SIZE] = mode;
    ++count;
    rt_hw_interrupt_enable(level);
}

int epd_wave_skip_unchanged_frame(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int skip = displayed && manual < 0 && !forced && count &&
               modes[head] != EPD_DRAW_MODE_FULL;
    if (skip) { head = (head + 1) % MODE_QUEUE_SIZE; --count; }
    rt_hw_interrupt_enable(level);
    return skip;
}

uint32_t epd_wave_table_get_frames(int temperature, EpdDrawMode mode)
{
    (void)temperature;
    rt_base_t level = rt_hw_interrupt_disable();
    if (count)
    {
        mode = modes[head];
        head = (head + 1) % MODE_QUEUE_SIZE;
        --count;
    }
    else if (forced) { mode = EPD_DRAW_MODE_FULL; forced = 0; }
    int selected = manual;
    if (selected < 0)
    {
        int full = !displayed || mode == EPD_DRAW_MODE_FULL;
        if (!full && mode == EPD_DRAW_MODE_AUTO)
            full = ++since_full >= (unsigned)period;
        selected = full ? 0 : EPD_WAVE_TEXT;
    }
    if (selected == 0) since_full = 0;
    if (selected != 3) displayed = 1;
    rt_hw_interrupt_enable(level);
    if (ref57_wave_prepare_mode(selected)) return 0;
    return ref57_wave_get_frames();
}

void epd_wave_table_fill_lut(uint32_t *lut, uint32_t frame)
{
    ref57_wave_fill_lut(lut, frame);
}
