/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DISPLAY_CHUNKS_H
#define EPD_DISPLAY_CHUNKS_H

#include <stdbool.h>
#include <stdint.h>
#include "epd_waveform.h"

#if defined(LCD_FB_USING_NONE) && defined(LCD_USING_EPD_E0470A03_V10_57)
#define EPD_DISPLAY_CHUNKS 1

#define EPD_LUT_PHASES_MAX 64u
#define EPD_LUT_PHASE_BYTES 64u
typedef enum
{
    EPD_FRAME_NORMAL, EPD_FRAME_APP_SWITCH, EPD_FRAME_CLEAR_FIRST
} epd_frame_transition_t;
/* Options for the next submitted frame. The UI validates and retains any LUT. */
void epd_display_stage_frame(const uint8_t *phases, uint8_t count, epd_frame_transition_t transition);

/* LVGL waits for the preceding SDK transfer before publishing the next chunk.
 * The LCD worker consumes this descriptor before returning that draw buffer. */
void epd_display_queue_chunk(const void *pixels, uint32_t stride,
                             bool first, bool last);

/* A queued UI update keeps the panel from being reported idle. */
void epd_display_set_pending(bool pending);

/* One preparation buffer. RGB chunks are copied before LVGL releases them.
 * A submitted buffer is writable again as soon as the LCD worker consumes it. */
bool epd_display_prepare_init(void);
bool epd_display_can_prepare(void);
bool epd_display_prepared_in_flight(void);
void epd_display_prepare_chunk(const uint8_t *pixels, uint32_t stride,
                               uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void epd_display_prepare_landscape(const uint8_t *pixels, uint32_t stride,
                                  uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void epd_display_discard_prepared(void);
void epd_display_submit_prepared(EpdDrawMode mode);
#endif

#endif
