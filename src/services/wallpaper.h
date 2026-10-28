#ifndef EPD_WALLPAPER_H
#define EPD_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>
#include <rtthread.h>
#include "lvgl.h"

typedef struct
{
    bool ready, busy, custom;
    uint32_t revision;
    rt_err_t error;
} wallpaper_status_t;

/* UI thread, after storage and the default display are initialized. Loading
 * runs in an app_service worker. Repeated calls do not reload a ready image. */
bool wallpaper_init(void);
void wallpaper_get_status(wallpaper_status_t *status);
/* UI thread. Copies the source path before returning. Success means accepted;
 * status.busy clears after conversion, persistence and UI publication finish.
 * A failed request retains both the saved selection and the current image. */
rt_err_t wallpaper_import(const char *path);
rt_err_t wallpaper_use_default(void);
/* Create/delete on the UI thread, outside drawing callbacks.
 * An opaque white, display-sized object with immutable content.
 * Returns NULL while initial loading is pending or allocation fails. Deleting
 * the object releases its decoded grayscale mask. Custom content is inflated
 * from the selected in-memory compressed resource when the object is created;
 * replacing the selection does not alter existing objects. No file access,
 * color-image copy, timers or refresh calls. */
lv_obj_t *wallpaper_view_create(lv_obj_t *parent);

#endif
