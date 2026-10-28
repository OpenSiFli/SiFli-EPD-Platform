#ifndef UI_READING_FONTS_H
#define UI_READING_FONTS_H

#include "lvgl.h"
#include <stdbool.h>

void reading_fonts_create(lv_obj_t *screen);
bool reading_fonts_open_install(void);
void reading_fonts_resume(void);
void reading_fonts_pause(void);
void reading_fonts_stop(void);
void reading_fonts_process(void);
void reading_fonts_restore_focus(void);
bool reading_fonts_action(int *target);
/* UI thread, after drawing finishes, before handing TF storage to USB. */
void reading_fonts_release_storage(void);

#endif
