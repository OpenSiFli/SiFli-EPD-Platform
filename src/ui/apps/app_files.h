#ifndef UI_APP_FILES_H
#define UI_APP_FILES_H

#include "lvgl.h"
#include <stdbool.h>

/* Resident UI-thread callbacks only. The path is borrowed for the call; copy
 * it before returning. True accepts and closes the picker, false leaves it
 * open. No callback is made on cancellation. The callback must not navigate.
 * PNG/JPEG files and directories use the existing file browser. */
typedef bool (*files_picker_callback_t)(const char *path, void *context);
bool files_picker_open(files_picker_callback_t selected, void *context);
/* TF-only JSON selection, with the same owner/cancellation contract. */
bool files_picker_open_json(files_picker_callback_t selected, void *context);
/* Call before destroying owner-page state; deleting its screen also cancels. */
void files_picker_cancel(lv_obj_t *owner);

#endif
