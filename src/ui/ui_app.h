#ifndef UI_APP_H
#define UI_APP_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

rt_err_t ui_app_init(void);
void ui_app_process(void);
void ui_app_process_deferred(void);
uint32_t ui_app_next_delay(uint32_t delay);
void ui_app_status_changed(void);

/* Service callbacks may publish data from another thread. No LVGL calls occur here. */
void ui_app_set_recent_reading(const char *title, int percent);

#endif
