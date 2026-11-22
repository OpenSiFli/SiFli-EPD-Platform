#include "calendar_exchange.h"
#include "storage_file.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static unsigned decoded,inserted;
static bool unlocked_decode(cal_book_t *b,const void *data,size_t size)
{CHECK(!host_storage_depth());++decoded;return cal_decode(b,data,size);}
static bool unlocked_put(cal_book_t *b,const cal_event_t *event)
{CHECK(!host_storage_depth());++inserted;return cal_put(b,event);}
#define cal_decode unlocked_decode
#define cal_put unlocked_put
#include "../../modules/calendr/calendar_exchange.c"
#undef cal_decode
#undef cal_put
int main(void)
{
    host_reset();cal_book_t source,loaded;cal_book_init(&source);cal_book_init(&loaded);
    cal_event_t event={0};strcpy(event.title,"event");strcpy(event.uid,"lock-test@example.com");
    event.rule.start_day=epd_date_day((epd_date_t){2026,10,30});event.rule.until_day=event.rule.start_day;event.duration_minutes=30;event.notes="notes";
    CHECK(cal_put(&source,&event));
    CHECK(cal_export_file(&source,"/flash/test.ics"));CHECK(cal_import_file(&loaded,"/flash/test.ics"));
    CHECK(inserted&&loaded.count==1&&!host_storage_depth());
    CHECK(cal_export_file(&source,"/flash/test.calbak"));inserted=0;
    CHECK(cal_import_file(&loaded,"/flash/test.calbak"));CHECK(decoded&&inserted&&!host_storage_depth());
    CHECK(storage_file_replace("/flash/bad.ics","bad\0data",8));CHECK(!cal_import_file(&loaded,"/flash/bad.ics"));CHECK(!host_storage_depth());
    cal_book_clear(&source);cal_book_clear(&loaded);host_reset();puts("calendar import: ICS parsing and backup decoding/merge run outside storage lock");return 0;
}
