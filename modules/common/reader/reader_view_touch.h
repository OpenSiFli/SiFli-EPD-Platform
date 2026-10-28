/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_VIEW_TOUCH_H
#define COMMON_READER_VIEW_TOUCH_H
#include <stdbool.h>
#include <stdint.h>

enum { READER_TOUCH_HOLD_MS = 3000, READER_TOUCH_SLOP = 12, READER_TOUCH_SWIPE = 48 };
typedef enum
{
    READER_TOUCH_DOWN, READER_TOUCH_MOVE, READER_TOUCH_UP, READER_TOUCH_CANCEL
} reader_touch_phase_t;
typedef enum
{
    READER_TOUCH_NONE, READER_TOUCH_SETTINGS, READER_TOUCH_PREVIOUS, READER_TOUCH_NEXT
} reader_touch_action_t;
typedef struct
{
    uint32_t started;
    int32_t x, y;
    bool active, moved, held;
} reader_view_touch_t;

static inline void reader_view_touch_reset(reader_view_touch_t *touch)
{
    *touch = (reader_view_touch_t){0};
}

static inline reader_touch_action_t reader_view_touch_update(reader_view_touch_t *touch,
    reader_touch_phase_t phase, uint32_t now, int32_t x, int32_t y)
{
    if (phase == READER_TOUCH_CANCEL)
    {
        reader_view_touch_reset(touch);
        return READER_TOUCH_NONE;
    }
    if (phase == READER_TOUCH_DOWN)
    {
        *touch = (reader_view_touch_t){.started = now, .x = x, .y = y, .active = true};
        return READER_TOUCH_NONE;
    }
    if (!touch->active) return READER_TOUCH_NONE;
    int32_t dx = x - touch->x, dy = y - touch->y;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > READER_TOUCH_SLOP || ay > READER_TOUCH_SLOP) touch->moved = true;
    if (phase == READER_TOUCH_UP) touch->active = false;
    if (touch->held) return READER_TOUCH_NONE;
    if (!touch->moved && (uint32_t)(now - touch->started) >= READER_TOUCH_HOLD_MS)
    {
        touch->held = true;
        return READER_TOUCH_SETTINGS;
    }
    if (phase == READER_TOUCH_UP && ax >= READER_TOUCH_SWIPE && ax > 2 * ay)
        return dx < 0 ? READER_TOUCH_NEXT : READER_TOUCH_PREVIOUS;
    return READER_TOUCH_NONE;
}
#endif
