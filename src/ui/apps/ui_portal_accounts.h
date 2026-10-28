#ifndef UI_PORTAL_ACCOUNTS_H
#define UI_PORTAL_ACCOUNTS_H
#include "lvgl.h"
#include <stdbool.h>

typedef struct ui_portal_accounts ui_portal_accounts_t;
ui_portal_accounts_t *ui_portal_accounts_create(lv_obj_t *screen);
void ui_portal_accounts_destroy(ui_portal_accounts_t *view);
void ui_portal_accounts_open(ui_portal_accounts_t *view);
bool ui_portal_accounts_visible(const ui_portal_accounts_t *view);
void ui_portal_accounts_process(ui_portal_accounts_t *view, bool layout);
bool ui_portal_accounts_action(ui_portal_accounts_t *view, int target);
#endif
