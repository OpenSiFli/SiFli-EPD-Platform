#ifndef EPD_FILE_PICKER_H
#define EPD_FILE_PICKER_H
#include "lvgl.h"
#include <stdbool.h>

typedef bool (*epd_file_selected_t)(const char *path, void *context);
/* UI thread. Extensions are a semicolon-separated list such as ".mp3;.wav".
 * Copy the borrowed path in the callback. Return true to accept without navigating.
 * Cancel before releasing callback context; deleting owner also detaches it. */
bool epd_file_picker_open(lv_obj_t *owner, const char *extensions, epd_file_selected_t selected, void *context);
void epd_file_picker_cancel(lv_obj_t *owner);
#endif
