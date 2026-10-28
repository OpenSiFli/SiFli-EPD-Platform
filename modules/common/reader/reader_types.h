/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_TYPES_H
#define COMMON_READER_TYPES_H
#include "lvgl.h"
#include "reader_error.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#define READER_PAGE_MAX ((unsigned)INT_MAX - 1u)
#define READER_TOP 64
#define READER_BOTTOM 1152
typedef struct {
    const char *text;
    const lv_font_t *font;
    const char *title;
    unsigned page, pages, percent, margin;
    int content_width, top, bottom;
    int line_space;
    uint32_t revision;
    bool ready, indexing, saved;
    reader_error_t error_code;
    char error[96];
} reader_view_t;
typedef enum { READER_WORK_IDLE, READER_WORK_BACKGROUND, READER_WORK_TARGET } reader_work_t;
#endif
