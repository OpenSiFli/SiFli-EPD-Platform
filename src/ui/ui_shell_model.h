#ifndef UI_SHELL_MODEL_H
#define UI_SHELL_MODEL_H
#include <stdbool.h>

typedef enum {
    UI_SHELL_NONE, UI_SHELL_REVEAL_STATUS, UI_SHELL_NOTIFICATIONS,
    UI_SHELL_CONTROLS, UI_SHELL_REVEAL_NAV, UI_SHELL_CLOSE_CENTER
} ui_shell_gesture_t;

/* Center dismissal accepts upward swipes anywhere; page gestures start at an edge. */
ui_shell_gesture_t ui_shell_edge_gesture(int x, int y, int dx, int dy,
                                        bool immersive, bool status_visible,
                                        bool center, bool locked);
#endif
