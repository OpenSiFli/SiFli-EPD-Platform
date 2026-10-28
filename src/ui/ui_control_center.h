#ifndef UI_CONTROL_CENTER_H
#define UI_CONTROL_CENTER_H
#include <stdbool.h>

/* UI thread only. Bottom-edge gestures belong to the system shell. */
void ui_control_center_open(bool notifications);
void ui_control_center_close(void);
bool ui_control_center_visible(void);
void ui_control_center_process(void);
void ui_control_center_storage_changed(void);

#endif
