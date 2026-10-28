#ifndef APP_WALLPAPER_H
#define APP_WALLPAPER_H

#include "lvgl.h"
#include <stdbool.h>

void wallpaper_page_create(lv_obj_t *screen);
void wallpaper_page_resume(void);
void wallpaper_page_pause(void);
void wallpaper_page_stop(void);
void wallpaper_page_restore_focus(void);
bool wallpaper_page_action(int *target);

#endif
