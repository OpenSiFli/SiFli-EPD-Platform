#include "calendar_model.h"
#include "calendar_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"check failed: %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static int32_t day(int y,int m,int d){return epd_date_day((epd_date_t){(int16_t)y,(uint8_t)m,(uint8_t)d});}
static epd_calendar_rule_t rule(int y,int m,int d,int repeat)
{epd_calendar_rule_t r={0};r.start_day=day(y,m,d);r.until_day=day(2099,12,31);r.repeat=(uint8_t)repeat;r.minute=540;r.timezone_minutes=480;r.weekdays=2;return r;}
int main(void)
{
    CHECK(!epd_date_valid(epd_date_from_day(INT32_MIN)));CHECK(!epd_date_valid(epd_date_from_day(INT32_MAX)));
    CHECK(day(1970,1,1)==0);CHECK(epd_date_weekday(day(2026,10,30))==5);CHECK(epd_date_utc_day(-1,0)==-1);
    CHECK(!epd_date_valid((epd_date_t){1900,2,29}));CHECK(epd_date_valid((epd_date_t){2000,2,29}));CHECK(day(2026,2,29)==INT32_MIN);
    for(int32_t i=day(1901,1,1);i<=day(2099,12,31);++i){epd_date_t d=epd_date_from_day(i);CHECK(epd_date_day(d)==i);epd_lunar_t l;epd_date_t back;if(epd_date_to_lunar(d,&l)){CHECK(epd_date_from_lunar(l,false,&back));CHECK(epd_date_day(back)==i);}}
    epd_lunar_t lunar;CHECK(epd_date_to_lunar((epd_date_t){2026,2,17},&lunar));CHECK(lunar.date.month==1&&lunar.date.day==1&&!lunar.leap);
    CHECK(!strcmp(epd_date_label((epd_date_t){2026,10,23},true,true),"霜降"));
    int64_t t;int32_t d;epd_calendar_rule_t r=rule(2026,1,31,EPD_REPEAT_MONTHLY);
    CHECK(epd_calendar_next(&r,(int64_t)day(2026,2,1)*86400,&t,&d)&&d==day(2026,3,31));
    r=rule(2024,2,29,EPD_REPEAT_YEARLY);r.month_end=true;CHECK(epd_calendar_next(&r,(int64_t)day(2025,1,1)*86400,&t,&d)&&d==day(2025,2,28));
    r=rule(2026,1,1,EPD_REPEAT_WEEKLY);r.weekdays=(1<<1)|(1<<5);r.excluded_count=1;r.excluded[0]=day(2026,1,2);
    CHECK(epd_calendar_next(&r,(int64_t)day(2026,1,1)*86400,&t,&d)&&d==day(2026,1,5));
    r=rule(2024,1,1,EPD_REPEAT_DAILY);int64_t lo=(int64_t)day(2024,1,1)*86400,hi=(int64_t)day(2026,10,30)*86400+8*3600;
    CHECK(epd_calendar_last(&r,lo,hi,&t,&d)&&d==day(2026,10,30));CHECK(!epd_calendar_last(&r,t+1,hi,&t,&d));
    r.excluded_count=32;for(int i=0;i<32;++i)r.excluded[i]=day(2026,1,1)+i;CHECK(epd_calendar_next(&r,(int64_t)day(2026,1,1)*86400,&t,&d)&&d==day(2026,2,2));
    CHECK(cal_utf8("日历🙂",3));CHECK(!cal_utf8("日历🙂",2));CHECK(!cal_utf8("\xf0\x80\x80\x80",9));CHECK(!cal_utf8("\xed\xa0\x80",9));
    cal_book_t b;cal_book_init(&b);cal_event_t e={0};strcpy(e.uid,"test@calendar");strcpy(e.title,"项目评审");e.rule=rule(2026,10,30,EPD_REPEAT_DAILY);e.rule.until_day=day(2026,11,3);e.duration_minutes=60;e.reminder_minutes=10;e.notes="测试换行\n和逗号,分号;";
    CHECK(cal_put(&b,&e));CHECK(b.count==1);cal_occurrence_t rows[3];size_t count=cal_query(&b,day(2026,10,30),day(2026,11,3),480,NULL,-1,rows,3,0);CHECK(count==3);CHECK(rows[2].day==day(2026,11,1));
    count=cal_query(&b,day(2026,10,30),day(2026,11,3),480,NULL,-1,rows,3,3);CHECK(count==2&&rows[0].day==day(2026,11,2));
    CHECK(cal_delete(&b,1,day(2026,10,31),true));CHECK(cal_query(&b,day(2026,10,31),day(2026,10,31),480,NULL,-1,rows,3,0)==0);
    e.rule.start_day=day(2026,11,1);strcpy(e.title,"本次改期");CHECK(cal_override(&b,1,day(2026,11,1),&e));CHECK(b.count==2);
    CHECK(cal_query(&b,day(2026,11,1),day(2026,11,1),480,NULL,-1,rows,3,0)==1);CHECK(rows[0].id==2);
    void *bytes=NULL;size_t size;CHECK(cal_encode(&b,&bytes,&size));cal_book_t copy;cal_book_init(&copy);CHECK(cal_decode(&copy,bytes,size));CHECK(copy.count==2);CHECK(!strcmp(copy.events[0]->notes,e.notes));
    CHECK(!cal_decode(&copy,bytes,size-1));CHECK(copy.count==2);((unsigned char *)bytes)[0]=0;CHECK(!cal_decode(&copy,bytes,size));CHECK(copy.count==2);cal_free(bytes);
    CHECK(cal_delete(&copy,1,0,false));CHECK(copy.count==0);cal_book_clear(&copy);cal_book_clear(&b);
    cal_book_init(&b);e.rule=rule(2026,10,30,EPD_REPEAT_NONE);e.rule.minute=23*60+30;e.duration_minutes=120;strcpy(e.title,"跨日事项");CHECK(cal_put(&b,&e));
    CHECK(cal_busy_days(&b,day(2026,10,29),5,480)==6);
    CHECK(cal_query(&b,day(2026,10,31),day(2026,10,31),480,NULL,-1,rows,3,0)==1);
    CHECK(rows[0].original_day==day(2026,10,30));
    e.title[0]=0;e.parent=7;e.original_day=day(2026,10,29);void *document=NULL;size_t dn;CHECK(cal_document_encode(&b,&e,day(2026,10,30),true,&document,&dn));
    cal_event_t draft={0};int32_t original;bool instance;CHECK(cal_document_decode(&copy,&draft,&original,&instance,document,dn));
    CHECK(draft.parent==7&&draft.original_day==day(2026,10,29));
    CHECK(copy.count==1&&draft.title[0]==0&&draft.duration_minutes==120&&instance&&original==day(2026,10,30));
    CHECK(!cal_document_decode(&copy,&draft,&original,&instance,document,dn-1));CHECK(copy.count==1&&draft.duration_minutes==120);
    cal_free(document);cal_event_clear(&draft);cal_book_clear(&copy);cal_book_clear(&b);
    puts("calendar: full date/lunar range, recurrence, exclusions, query pagination and atomic decode passed");return 0;
}
