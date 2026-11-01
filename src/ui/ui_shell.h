#ifndef UI_SHELL_H
#define UI_SHELL_H
#include "lvgl.h"

void ui_shell_init(void);
void ui_shell_process(void);
void ui_shell_storage_changed(void);
void ui_shell_power_menu(void);
void ui_shell_lock(void);
void ui_shell_prepare_lock(void);
void ui_shell_settings(void);
void ui_shell_status_changed(void);
bool ui_shell_locked(void);
bool ui_shell_modal_visible(void);
void ui_shell_navigation_failed(void);
/* UI-thread page declaration, removed automatically with its screen. */
void ui_shell_page_mode(lv_obj_t *screen, bool immersive);
/* Bottom edge available to ordinary page content, excluding the navigation
 * strip or the bottom gesture target. UI thread only. */
int32_t ui_shell_content_bottom(void);
#endif
