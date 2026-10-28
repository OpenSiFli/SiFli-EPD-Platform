/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UI_SPEECH_SETTINGS_H
#define UI_SPEECH_SETTINGS_H
#include "lvgl.h"
#include <stdbool.h>
void ui_speech_settings_create(lv_obj_t *screen);
void ui_speech_settings_resume(void);
void ui_speech_settings_pause(void);
void ui_speech_settings_stop(void);
void ui_speech_settings_restore_focus(void);
bool ui_speech_settings_action(int *target);
#endif
