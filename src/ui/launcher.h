#ifndef LAUNCHER_H
#define LAUNCHER_H
#include "ui_model.h"

typedef struct
{
    char time_text[24];
    char clock_text[16];
    ui_radio_state_t bluetooth;
    ui_radio_state_t wifi;
    bool headset_connected; /* A2DP audio headset connection. */
    int headset_battery_percent; /* -1: unknown, 0..100: reported percentage. */
    int battery_percent;
    bool charging;
    char recent_book[128];
    int reading_percent;
    char weather[64];
} launcher_status_t;

/* All launcher functions run on the LVGL owner thread. */
bool launcher_init(void);
void launcher_set_status(const launcher_status_t *status);
void launcher_process(void);
bool launcher_storage_process(void);
void launcher_open(ui_page_id_t page);
ui_page_id_t launcher_current_page(void);
unsigned launcher_focus_index(void);

#endif
