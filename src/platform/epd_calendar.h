#ifndef EPD_CALENDAR_H
#define EPD_CALENDAR_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct { int16_t year; uint8_t month, day; } epd_date_t;
typedef struct { epd_date_t date; bool leap; } epd_lunar_t;
typedef enum { EPD_REPEAT_NONE, EPD_REPEAT_DAILY, EPD_REPEAT_WEEKLY,
               EPD_REPEAT_MONTHLY, EPD_REPEAT_YEARLY } epd_repeat_t;
#define EPD_CALENDAR_EXCEPTIONS 32
typedef struct
{
    int32_t start_day, until_day;
    int16_t timezone_minutes;
    uint16_t minute;
    uint8_t repeat, weekdays, lunar_month, lunar_day;
    bool lunar_leap, month_end;
    uint8_t excluded_count;
    int32_t excluded[EPD_CALENDAR_EXCEPTIONS];
} epd_calendar_rule_t;

bool epd_date_valid(epd_date_t date);
int epd_date_month_days(int year, int month);
int32_t epd_date_day(epd_date_t date); /* Days since 1970-01-01; INT32_MIN on error. */
epd_date_t epd_date_from_day(int32_t day);
int epd_date_weekday(int32_t day); /* Sunday = 0. */
int32_t epd_date_utc_day(int64_t utc, int offset_minutes);
bool epd_date_to_lunar(epd_date_t date, epd_lunar_t *out);
bool epd_date_from_lunar(epd_lunar_t lunar, bool month_end, epd_date_t *out);
bool epd_calendar_rule_valid(const epd_calendar_rule_t *rule);
/* First occurrence at or after the UTC lower bound, including exclusions. */
bool epd_calendar_next(const epd_calendar_rule_t *rule, int64_t lower,
                       int64_t *utc, int32_t *day);
bool epd_calendar_last(const epd_calendar_rule_t *rule,int64_t lower,int64_t upper,int64_t *utc,int32_t *day);
/* Gregorian label priority: festival, supported solar term, lunar date. */
const char *epd_date_label(epd_date_t date, bool festivals, bool lunar);
bool epd_date_lunar_text(epd_date_t date, char *out, size_t size);
#endif
