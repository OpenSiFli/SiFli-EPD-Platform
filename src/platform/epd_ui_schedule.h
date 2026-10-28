#ifndef EPD_UI_SCHEDULE_H
#define EPD_UI_SCHEDULE_H

#include <rtthread.h>

/* Apply after LVGL has created its workers and before the UI event loop. */
rt_err_t epd_ui_schedule_start(void);

#endif
