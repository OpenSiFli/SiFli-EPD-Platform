/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DISPLAY_CHUNKS_H
#define EPD_DISPLAY_CHUNKS_H

#include <stdbool.h>
#include <stdint.h>

#if defined(LCD_FB_USING_NONE) && defined(LCD_USING_EPD_E0470A03_V10_57)
#define EPD_DISPLAY_CHUNKS 1

/* LVGL waits for the preceding SDK transfer before publishing the next chunk.
 * The LCD worker consumes this descriptor before returning that draw buffer. */
void epd_display_queue_chunk(const void *pixels, uint32_t stride,
                             bool first, bool last);

/* Keep existing power for a pending UI frame; this never powers an idle panel on.
 * Publish pending before waiting for the preceding transfer. Clear it after the
 * next frame's first chunk is queued, or when that update is cancelled. A committed
 * discharge completes normally; the first chunk requests power without blocking
 * conversion. */
void epd_display_set_pending(bool pending);
#endif

#endif
