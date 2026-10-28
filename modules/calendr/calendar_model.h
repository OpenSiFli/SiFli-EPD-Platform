#ifndef CALENDAR_MODEL_H
#define CALENDAR_MODEL_H
#include "epd_calendar.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define CAL_LIMIT 1000
#define CAL_TEXT_BYTES 6000
typedef enum { CAL_EVENT, CAL_BIRTHDAY, CAL_ANNIVERSARY, CAL_COUNTDOWN } cal_kind_t;
typedef struct {
    uint32_t id,parent;
    int32_t original_day;
    epd_calendar_rule_t rule;
    uint32_t duration_minutes;
    uint8_t kind;
    bool all_day, inclusive;
    int32_t reminder_minutes;
    char uid[128],title[241],place[361];
    char *notes;
} cal_event_t;
typedef struct {
    cal_event_t **events;
    size_t count;
    uint32_t next_id;
    uint16_t default_duration;
    uint8_t first_weekday,default_view;
    bool lunar,festivals;
} cal_book_t;
typedef struct { uint32_t id;int32_t day,original_day;int64_t utc;bool all_day; } cal_occurrence_t;
void *cal_alloc(size_t size);
void cal_free(void *value);
bool cal_utf8(const char *text,size_t max_chars);
bool cal_event_valid(const cal_event_t *event);
void cal_event_clear(cal_event_t *event);
bool cal_event_copy(cal_event_t *to,const cal_event_t *from);
void cal_book_init(cal_book_t *book);
void cal_book_clear(cal_book_t *book);
cal_event_t *cal_find(cal_book_t *book,uint32_t id);
bool cal_put(cal_book_t *book,const cal_event_t *event);
bool cal_delete(cal_book_t *book,uint32_t id,int32_t day,bool instance);
bool cal_override(cal_book_t *book,uint32_t parent,int32_t day,const cal_event_t *event);
size_t cal_query(const cal_book_t *book,int32_t first,int32_t last,int timezone,
                 const char *query,int kind,cal_occurrence_t *out,size_t capacity,size_t offset);
uint64_t cal_busy_days(const cal_book_t *book,int32_t first,unsigned days,int timezone);
size_t cal_search(const cal_book_t *book,int32_t today,int timezone,const char *query,int kind,cal_occurrence_t *out,size_t capacity,size_t offset);
bool cal_encode(const cal_book_t *book,void **bytes,size_t *size);
bool cal_decode(cal_book_t *book,const void *bytes,size_t size);
#endif
