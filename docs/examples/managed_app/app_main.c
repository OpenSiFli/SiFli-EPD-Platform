/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_app.h"
#include "platform/epd_i18n.h"
#include "platform/app_image.h"
#include "storage_file.h"
#include "managed_job.h"
#include "demo_state.h"
#include "app_id.h"
#include <stdio.h>

EPD_APP_DEFINE(DEMO_APP_ID);
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

static managed_job_t job;
static demo_state_t model;
static lv_timer_t *maintenance;
static lv_obj_t *status_label;
static uint32_t generation;
static bool counted;
typedef struct { lv_obj_t *image; } demo_page_t;

static void poll(lv_timer_t *timer)
{
    managed_job_poll(&job);
    if (job.phase == DEMO_DONE && !counted) {
        counted = job.result != EPD_OPERATION_OK || demo_state_increment(&model);
    }
    if (status_label) {
        const char *text = managed_job_active(&job) ? "Working" : job.phase != DEMO_DONE ? "Ready" :
            job.result == EPD_OPERATION_OK ? "Complete" :
            job.result == EPD_OPERATION_CANCELLED ? "Cancelled" : "Failed";
        char line[128];
        uint32_t checksum = job.phase == DEMO_DONE ? job.checksum : 0;
        snprintf(line, sizeof(line), "%s\nChecksum: %lu\nCompleted: %lu", text,
                 (unsigned long)checksum, (unsigned long)model.count);
        epd_app_text(status_label, line);
    } else if (!managed_job_active(&job)) {
        maintenance = NULL; lv_timer_delete(timer);
    }
}
static void start(lv_event_t *event)
{
    (void)event;
    if (managed_job_active(&job) || generation == UINT32_MAX) return;
    if (managed_job_start(&job, ++generation)) counted = false;
    else epd_app_text(status_label, "Task was not accepted");
}
static void cancel(lv_event_t *event) { (void)event; managed_job_cancel(&job); }
static void message(gui_app_msg_type_t type, void *parameter)
{
    (void)parameter;
    demo_page_t *page = gui_app_this_page_memory();
    if (type == GUI_APP_MSG_ONSTART) {
        gui_app_close_anim();
        lv_obj_t *screen = lv_screen_active();
        lv_obj_remove_style_all(screen); lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        epd_app_header(screen); epd_app_back_button(screen);
        epd_app_label(screen, "Managed task example", 100, 100, 540, epd_app_font_role(EPD_FONT_TITLE));
        page->image = lv_image_create(screen); lv_obj_set_pos(page->image, 80, 190);
        status_label = epd_app_label(screen, "Ready", 80, 350, 540, epd_app_font());
        lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(status_label, 220);
        epd_app_button(screen, 80, 570, 524, 100, "Start calculation", start, NULL);
        epd_app_button(screen, 80, 700, 524, 100, "Cancel calculation", cancel, NULL);
    } else if (type == GUI_APP_MSG_ONRESUME) {
        char path[256];
        if (storage_app_path(path, sizeof(path), DEMO_APP_ID, STORAGE_APP_CODE, "icon.ezip"))
            epd_app_image_set(page->image, path, NULL);
    } else if (type == GUI_APP_MSG_ONPAUSE) {
        epd_app_image_release(page->image);
    } else if (type == GUI_APP_MSG_ONSTOP) {
        epd_app_image_release(page->image);
        status_label = NULL;
        managed_job_cancel(&job);
        /* The application timer remains valid while the accepted TASK drains.
         * No queued callback retains the page or its widgets. */
    }
}
int app_main(intent_t intent)
{
    (void)intent;
    if (!demo_state_open(&model)) return -1;
    /* Created in the application entry context, not in a PAGE callback. */
    if (!maintenance) maintenance = lv_timer_create(poll, 50, NULL);
    if (!maintenance) return -1;
    int result = gui_app_regist_msg_handler_ext(DEMO_APP_ID, message, NULL, sizeof(demo_page_t));
    if (result && !managed_job_active(&job)) { lv_timer_delete(maintenance); maintenance = NULL; }
    return result;
}
