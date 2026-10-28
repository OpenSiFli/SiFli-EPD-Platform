#include "ui_shell_model.h"

ui_shell_gesture_t ui_shell_edge_gesture(int x, int y, int dx, int dy,
                                        bool immersive, bool status_visible,
                                        bool center, bool locked)
{
    if (locked) return UI_SHELL_NONE;
    int horizontal = dx < 0 ? -dx : dx;
    if ((center || y >= 1184) && dy <= -36 && -dy > horizontal)
        return center ? UI_SHELL_CLOSE_CENTER : UI_SHELL_REVEAL_NAV;
    if (!center && y < (status_visible ? 64 : 24) && dy >= 36 && dy > horizontal)
    {
        if (immersive && !status_visible) return UI_SHELL_REVEAL_STATUS;
        return x < 342 ? UI_SHELL_NOTIFICATIONS : UI_SHELL_CONTROLS;
    }
    return UI_SHELL_NONE;
}
