#ifndef BOOKS_READER_GESTURE_H
#define BOOKS_READER_GESTURE_H
#include "../common/reader/reader_view_touch.h"
static inline reader_touch_action_t books_gesture_update(reader_view_touch_t *touch,
    reader_touch_phase_t phase, int32_t x, int32_t y)
{
    if (phase == READER_TOUCH_CANCEL) { reader_view_touch_reset(touch); return READER_TOUCH_NONE; }
    if (phase == READER_TOUCH_DOWN) {
        *touch = (reader_view_touch_t){.x=x, .y=y, .active=y<1184};
        return READER_TOUCH_NONE;
    }
    if (!touch->active) return READER_TOUCH_NONE;
    int32_t dx=x-touch->x, dy=y-touch->y;
    int32_t ax=dx<0?-dx:dx, ay=dy<0?-dy:dy;
    if (ax >= READER_TOUCH_SWIPE && ax > 2 * ay) {
        touch->active = false;
        return dx < 0 ? READER_TOUCH_NEXT : READER_TOUCH_PREVIOUS;
    }
    if (phase == READER_TOUCH_UP) {
        touch->active = false;
        if (dy <= -READER_TOUCH_SWIPE && ay > 2 * ax) return READER_TOUCH_SETTINGS;
    }
    return READER_TOUCH_NONE;
}
#endif
