/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UI_VOICE_INPUT_H
#define UI_VOICE_INPUT_H
#include <stdbool.h>
#include "lvgl.h"

/* GUI-thread only. Native textarea focus and keyboard binding supply the target. */
void ui_voice_input_init(void);
void ui_voice_input_process(bool blocked);
bool ui_voice_input_available(void);
void ui_voice_input_open(void);
/* The platform keyboard supplies its own target when several editors exist. */
void ui_voice_input_open_for(lv_obj_t *target);
void ui_voice_input_close(void);
bool ui_voice_input_visible(void);
bool ui_voice_input_recording(void);
#endif
