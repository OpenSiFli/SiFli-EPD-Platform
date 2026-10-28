#ifndef UI_NAV_GESTURE_H
#define UI_NAV_GESTURE_H
#include "lvgl.h"
#include <stdbool.h>

typedef enum { UI_NAV_NONE, UI_NAV_BACK, UI_NAV_HOME, UI_NAV_RECENT } ui_nav_action_t;

/* UI owner: callback queues navigation; it must not delete the active screen. */
void ui_nav_gesture_init(void (*dispatch)(ui_nav_action_t action));
void ui_nav_gesture_update(bool enabled, bool home_allowed, unsigned scale, unsigned scope);
void ui_nav_gesture_cancel(void);

#endif
