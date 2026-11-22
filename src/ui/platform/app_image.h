#ifndef EPD_APP_IMAGE_H
#define EPD_APP_IMAGE_H

#include "lvgl.h"
#include <stdbool.h>

/* UI thread only. Widgets share immutable, versioned EZIP pixels; each widget
 * independently pins its activity reference until drawing has finished.
 * A NULL path selects the built-in fallback. Identical sources do not redraw.
 * Release on ONPAUSE, set again on ONRESUME; deletion releases automatically. */
bool epd_app_image_set(lv_obj_t *image, const char *path, const lv_image_dsc_t *fallback);
void epd_app_image_release(lv_obj_t *image);

#endif
