#ifndef EPD_KEYBOARD_INTERNAL_H
#define EPD_KEYBOARD_INTERNAL_H
#include "lvgl.h"
#include <stdbool.h>
/* UI thread. Applies the enclosing platform keyboard's input policy. */
bool epd_keyboard_allows_voice(lv_obj_t *native_keyboard);
/* Dismiss the frontmost visible keyboard inside this input scope. */
bool epd_keyboard_back(lv_obj_t *root);
#endif
