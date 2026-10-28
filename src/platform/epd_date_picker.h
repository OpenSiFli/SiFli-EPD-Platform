#ifndef EPD_DATE_PICKER_H
#define EPD_DATE_PICKER_H
#include "epd_calendar.h"
#include "lvgl.h"

typedef enum { EPD_PICK_DATE, EPD_PICK_TIME } epd_date_picker_mode_t;
typedef struct
{
    epd_date_t date;
    uint16_t minute;
    int16_t first_year, last_year;
    uint8_t first_weekday;
    bool hour24;
} epd_date_picker_value_t;

/* UI thread. The parent owns the widget. User selections emit VALUE_CHANGED;
 * set() updates without emitting an event. Read the value in the callback. */
lv_obj_t *epd_date_picker_create(lv_obj_t *parent, epd_date_picker_mode_t mode,
                                const epd_date_picker_value_t *value, int32_t width);
void epd_date_picker_get(lv_obj_t *picker, epd_date_picker_value_t *value);
void epd_date_picker_set(lv_obj_t *picker, const epd_date_picker_value_t *value);
int32_t epd_date_picker_height(epd_date_picker_mode_t mode);
#endif
