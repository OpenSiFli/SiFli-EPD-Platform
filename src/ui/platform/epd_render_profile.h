/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_RENDER_PROFILE_H
#define EPD_RENDER_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
enum { EPD_RENDER_DSC, EPD_RENDER_BITMAP, EPD_RENDER_SETTLE, EPD_RENDER_COPY,
       EPD_RENDER_PROFILE_COUNT };
typedef struct {
    uint32_t calls, ticks;
} epd_render_profile_item_t;
typedef struct {
    epd_render_profile_item_t stage[EPD_RENDER_PROFILE_COUNT];
} epd_render_profile_t;
/* HCPU cumulative elapsed ticks, including the active portion of a stage.
 * Snapshot differences clip work to the requested interval; nested calls of
 * the same stage count each call but charge elapsed time only once. */
void epd_render_profile_snapshot(epd_render_profile_t *out);
/* One global diagnostic window. Disable freezes counters and retires tokens. */
void epd_render_profile_window(bool enabled);
#ifndef BUILD_DLMODULE
extern volatile uint32_t epd_render_profile_generation;
uint32_t epd_render_profile_begin_active(unsigned stage);
void epd_render_profile_end_active(unsigned stage, uint32_t generation);
static inline uint32_t epd_render_profile_begin(unsigned stage)
{
    return epd_render_profile_generation ? epd_render_profile_begin_active(stage) : 0;
}
static inline void epd_render_profile_end(unsigned stage, uint32_t generation)
{
    if (generation && generation == epd_render_profile_generation)
        epd_render_profile_end_active(stage, generation);
}
#endif
#endif
