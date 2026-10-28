#ifndef SETTINGS_WIRELESS_H
#define SETTINGS_WIRELESS_H

#include "ui_model.h"
#include "ui_settings_list.h"

/* These pages own current->feature. Route them before generic settings pages. */
bool settings_wireless_handles(ui_page_id_t page);
void settings_wireless_create(lv_obj_t *screen);
void settings_wireless_resume(void);
void settings_wireless_process(void);
void settings_wireless_stop(void);
void settings_wireless_delete(void);
bool settings_wireless_action(int *request);
void settings_wireless_restore_focus(void);
ui_settings_list_t *settings_wireless_list(void);

#endif
