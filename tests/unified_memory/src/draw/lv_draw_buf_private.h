/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UNIFIED_TEST_DRAW_H
#define UNIFIED_TEST_DRAW_H
#include "lvgl.h"
typedef struct _lv_draw_buf_handlers_t {
    void *(*buf_malloc_cb)(size_t, lv_color_format_t);
    void (*buf_free_cb)(void *);
} lv_draw_buf_handlers_t;
lv_draw_buf_handlers_t *lv_draw_buf_get_font_handlers(void);
#endif
