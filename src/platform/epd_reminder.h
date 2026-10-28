#ifndef EPD_REMINDER_H
#define EPD_REMINDER_H
#include "epd_calendar.h"
#include "app_service.h"
#define EPD_REMINDER_MAX 1024u
#define EPD_REMINDER_DATA_MAX (2u * 1024u * 1024u)
typedef struct {
    uint32_t id;
    int32_t lead_minutes;
    bool date_only;
    epd_calendar_rule_t rule;
    char title[96];
} epd_reminder_t;
typedef enum { EPD_REMINDER_OK, EPD_REMINDER_EMPTY, EPD_REMINDER_INVALID,
    EPD_REMINDER_IO, EPD_REMINDER_MEMORY, EPD_REMINDER_UNAVAILABLE } epd_reminder_result_t;
/* Worker only. A single durable record contains the application's document,
 * rules and delivery cursors. commit copies all input before returning.
 * Data remains in internal storage independently of the module's volume. */
epd_reminder_result_t epd_reminder_commit(epd_service_t *owner,
    const void *data, size_t size, const epd_reminder_t *rules, size_t count);
/* Worker only. Returns a PSRAM allocation released with epd_app_free(). */
epd_reminder_result_t epd_reminder_load(epd_service_t *owner, void **data, size_t *size);
bool epd_reminder_available(void);
#endif
