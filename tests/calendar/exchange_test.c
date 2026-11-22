#include "calendar_exchange.h"
#include "storage_file.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void)
{
    host_reset();cal_book_t a,b;cal_book_init(&a);cal_book_init(&b);cal_event_t e={0};
    strcpy(e.uid,"one@example.com");strcpy(e.title,"讨论换行、逗号与时区");strcpy(e.place,"南京; 一楼, A区");e.notes="第一行\n第二行\\路径🙂";
    e.rule.start_day=epd_date_day((epd_date_t){2026,10,30});e.rule.until_day=epd_date_day((epd_date_t){2099,12,31});e.rule.minute=23*60+30;e.duration_minutes=90;e.reminder_minutes=10;
    CHECK(cal_put(&a,&e));CHECK(cal_export_file(&a,"/flash/Exports/test.ics"));CHECK(cal_import_file(&b,"/flash/Exports/test.ics"));CHECK(b.count==1);
    CHECK(!strcmp(b.events[0]->notes,e.notes));CHECK(!strcmp(b.events[0]->place,e.place));CHECK(b.events[0]->duration_minutes==90);CHECK(cal_import_file(&b,"/flash/Exports/test.ics"));CHECK(b.count==1);
    CHECK(cal_export_file(&a,"/flash/Exports/test.calbak"));CHECK(cal_import_file(&b,"/flash/Exports/test.calbak"));CHECK(b.count==1);
    e.rule.repeat=EPD_REPEAT_WEEKLY;e.rule.weekdays=(1<<1)|(1<<5);e.rule.excluded_count=1;e.rule.excluded[0]=e.rule.start_day+3;e.id=1;CHECK(cal_put(&a,&e));
    CHECK(cal_export_file(&a,"/flash/Exports/test.ics"));CHECK(cal_import_file(&b,"/flash/Exports/test.ics"));CHECK(b.events[0]->rule.repeat==EPD_REPEAT_WEEKLY);CHECK(b.events[0]->rule.excluded_count==1);
    const char *invalid="BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:bad@example.com\r\nSUMMARY:Bad\r\nDTSTART;TZID=Asia/Shanghai:20261030T100000\r\nDTEND:20261030T110000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    CHECK(storage_file_replace("/flash/bad.ics",invalid,strlen(invalid)));CHECK(!cal_import_file(&b,"/flash/bad.ics"));CHECK(b.count==1);
    host_fail_after(10);CHECK(!cal_export_file(&a,"/flash/Exports/test.calbak"));host_fail_after(-1);cal_book_t c;cal_book_init(&c);CHECK(cal_import_file(&c,"/flash/Exports/test.calbak"));CHECK(c.events[0]->rule.repeat==EPD_REPEAT_NONE);
    cal_book_clear(&a);cal_book_clear(&b);cal_book_clear(&c);host_reset();puts("calendar exchange: Unicode, UTC/cross-day, recurrence, stable UID, backup and write failure passed");return 0;
}
