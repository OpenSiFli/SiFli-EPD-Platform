#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdlib.h>
#include <stdio.h>
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_ASSERT_HANDLER fprintf(stderr, "LVGL assertion: %s:%d\n", __FILE__, __LINE__); exit(1);
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_DRAW_BUF_ALIGN 32
#define LV_DRAW_BUF_STRIDE_ALIGN 4
#endif
