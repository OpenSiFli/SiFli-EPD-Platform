#ifndef UI_AI_SETTINGS_H
#define UI_AI_SETTINGS_H
#include "lvgl.h"
#include <stdbool.h>

/* Bind the resident UI owner during launcher initialization. */
void ui_ai_settings_init(void);
void ui_ai_settings_create(lv_obj_t *screen);
void ui_ai_settings_resume(void);
void ui_ai_settings_pause(void);
void ui_ai_settings_stop(void);
void ui_ai_settings_restore_focus(void);
bool ui_ai_settings_action(int *target);
#endif
