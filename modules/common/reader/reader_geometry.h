/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_GEOMETRY_H
#define COMMON_READER_GEOMETRY_H
#include "platform/epd_app.h"

typedef struct { int width, top, bottom; } reader_geometry_t;

static inline reader_geometry_t reader_geometry(void)
{
    lv_obj_t *screen = lv_screen_active();
    int width = screen ? lv_obj_get_width(screen) : 684;
    int height = screen ? lv_obj_get_height(screen) : 1216;
#ifdef BOOKS_READER_UI
    reader_geometry_t area = {width, 54, height - 48};
#else
    reader_geometry_t area = {width, epd_ui_touch(56) + epd_ui_px(8),
                             height - epd_ui_touch(64)};
#endif
    return area;
}
#endif
