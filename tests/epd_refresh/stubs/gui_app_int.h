#ifndef GUI_APP_INT_H
#define GUI_APP_INT_H
#include "lvgl.h"
typedef lv_obj_t *screen_t;
#define GUI_APP_ID_MAX_LEN 16
typedef struct { char id[GUI_APP_ID_MAX_LEN]; } gui_runing_app_t;
typedef struct { screen_t scr; const gui_runing_app_t *parent; } subpage_node_t;
gui_runing_app_t *gui_app_get_actived(void);
const subpage_node_t *app_schedule_get_this_page(void);
typedef enum {
    GUI_APP_MSG_ONSTART, GUI_APP_MSG_ONRESUME, GUI_APP_MSG_ONPAUSE, GUI_APP_MSG_ONSTOP
} gui_app_msg_type_t;
typedef void (*host_lifecycle_t)(const screen_t, gui_app_msg_type_t);
void app_schedule_set_anim_hook(host_lifecycle_t hook);
#endif
