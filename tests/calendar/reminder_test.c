#include "rtthread.h"
#include "host_fs.h"
#include "storage.h"
bool storage_file_recover(const char *path);
static struct tm *gmtime_r(const time_t *t,struct tm *out){return gmtime_s(out,t)?NULL:out;}
#include "storage_file.h"
#define EPD_STORAGE_FILE_H
#define EPD_SYSTEM_TIME_H
#define EPD_SYSTEM_RESET_H
#include "epd_memory.h"
#include "epd_reminder.h"
#include "platform/epd_system_internal.h"
#include <string.h>
#include <limits.h>
typedef struct {int16_t timezone_minutes;bool automatic,hour24;} system_time_settings_t;
extern int64_t fake_now;
static int timezone_minutes=480;
bool alarm_fail;
static bool reset_blocked;
int64_t system_time_get_utc(void);
bool system_time_utc_valid(int64_t t){return t>=1704067200LL&&t<4102444800LL;}
void system_time_get_settings(system_time_settings_t *s){s->timezone_minutes=timezone_minutes;}
bool system_reset_blocks_apps(void){return reset_blocked;}
#ifdef REMINDER_OWNER_TEST
#include "owner_ports.h"
#else
void *epd_app_alloc(size_t n,epd_app_memory_t r){return calloc(1,n?n:1);}
void epd_app_free(void *p){free(p);}
#endif
rt_thread_t epd_thread_create_psram(const char *n,void (*e)(void*),void*p,rt_uint32_t s,rt_uint8_t pri,rt_uint32_t t){return (void*)1;}
bool epd_service_is_worker(epd_service_t *s){return s==(void*)1;}
static const char *service_app="calendr";
bool epd_service_identity(epd_service_t *s,char *id,size_t n){if(!s||n<=strlen(service_app))return false;strcpy(id,service_app);return true;}
static epd_notification_t published;static unsigned publications;
bool epd_notification_publish_resident(const epd_notification_t *n){if(strcmp(n->id,published.id))++publications;published=*n;return true;}
bool epd_notification_dismiss(const char *a,const char *id){if(!strcmp(published.id,id))published.id[0]=0;return true;}
#ifdef REMINDER_OWNER_TEST
#include "reminder_actual.inc"
#else
#include "../../src/services/reminder.c"
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int32_t day(int y,int m,int d){return epd_date_day((epd_date_t){y,m,d});}
static void reboot(void){while(agendas){agenda_t*a=agendas;agendas=a->next;free(a->entries);free(a);}restored=false;restore();}
static void check_load_failure(epd_reminder_result_t expected)
{
 void *data=(void*)1;size_t size=123;
 CHECK(epd_reminder_load((void*)1,&data,&size)==expected);
 CHECK(data==NULL&&size==0&&host_storage_depth()==0);
}
static void missing_record(void)
{
 const char *path=ROOT "/calendr.bin";
 CHECK(!host_exists(path));
 check_load_failure(EPD_REMINDER_EMPTY);
 host_stat_error(ENOENT);check_load_failure(EPD_REMINDER_EMPTY);
 const int errors[]={EIO,-EIO,EACCES,-EACCES};
 for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);++i){
  host_stat_error(errors[i]);check_load_failure(EPD_REMINDER_IO);
 }
 host_stat_error(0);CHECK(!host_exists(path));
}
static void damaged_record(void)
{
 const char *path=ROOT "/calendr.bin";size_t size=0;
 const unsigned char *stored=host_contents(path,&size);CHECK(stored&&size>16);
 unsigned char *copy=malloc(size);CHECK(copy);memcpy(copy,stored,size);
 host_stat_error(-EIO);check_load_failure(EPD_REMINDER_IO);host_stat_error(0);
 size_t current_size=0;stored=host_contents(path,&current_size);
 CHECK(stored&&current_size==size&&!memcmp(stored,copy,size));
 copy[size-1]^=1;CHECK(storage_file_replace(path,copy,size));
 check_load_failure(EPD_REMINDER_IO);
 copy[size-1]^=1;CHECK(storage_file_replace(path,copy,12));
 check_load_failure(EPD_REMINDER_IO);
 CHECK(storage_file_replace(path,copy,size));free(copy);
 void *data=NULL;size_t bytes=0;
 CHECK(epd_reminder_load((void*)1,&data,&bytes)==EPD_REMINDER_OK);
 CHECK(bytes==4&&!strcmp(data,"all"));free(data);CHECK(host_storage_depth()==0);
}
int main(void){
 host_reset();reminder_start();restore();CHECK(epd_reminder_available());
 missing_record();
 epd_reminder_t r={0};r.id=1;strcpy(r.title,"项目评审");r.rule.start_day=day(2026,10,30);r.rule.until_day=day(2026,11,3);r.rule.minute=540;r.rule.timezone_minutes=480;r.rule.repeat=EPD_REPEAT_DAILY;r.lead_minutes=10;
 fake_now=(int64_t)r.rule.start_day*86400+3000; // 08:50 local
 CHECK(epd_reminder_commit((void*)2,"old",4,&r,1)==EPD_REMINDER_INVALID);
 CHECK(epd_reminder_commit((void*)1,"old",4,&r,1)==EPD_REMINDER_OK);process();CHECK(publications==1);CHECK(armed==fake_now+86400);
 CHECK(reminder_dismiss("calendr",published.id));process();CHECK(!agendas->header.notice.id[0]);reboot();process();CHECK(publications==1);
 fake_now+=86400*3+120;process();CHECK(publications==2);int64_t cursor=agendas->entries[0].after;fake_now-=86400*2;process();CHECK(publications==2&&agendas->entries[0].after==cursor);
 host_fail_after(0);CHECK(epd_reminder_commit((void*)1,"new",4,&r,1)==EPD_REMINDER_IO);host_fail_after(-1);
 void *data=NULL;size_t n;CHECK(epd_reminder_load((void*)1,&data,&n)==EPD_REMINDER_OK&&n==4&&!strcmp(data,"old"));free(data);
 CHECK(reminder_remove_app("calendr"));CHECK(!agendas->header.enabled);reboot();fake_now+=86400*4;process();CHECK(publications==2&&armed==0);
 CHECK(epd_reminder_load((void*)1,&data,&n)==EPD_REMINDER_OK&&!strcmp(data,"old"));free(data);CHECK(agendas->header.enabled);
 CHECK(epd_reminder_commit((void*)1,"old",4,NULL,0)==EPD_REMINDER_OK);process();CHECK(!agendas->header.notice.id[0]);
 r.date_only=true;r.lead_minutes=0;r.rule.minute=0;r.rule.start_day=day(2026,11,10);r.rule.until_day=day(2026,11,12);fake_now=(int64_t)r.rule.start_day*86400-9*3600;
 CHECK(epd_reminder_commit((void*)1,"all",4,&r,1)==EPD_REMINDER_OK);process();CHECK(armed==fake_now+3600);
 fake_now+=3600;process();unsigned count=publications;CHECK(count==3);timezone_minutes=0;process();CHECK(publications==count);fake_now+=8*3600;process();CHECK(publications==count);CHECK(armed==(int64_t)(r.rule.start_day+1)*86400);
 reset_blocked=true;process();CHECK(!armed);reset_blocked=false;
 alarm_fail=true;process();CHECK(!epd_reminder_available());alarm_fail=false;process();CHECK(epd_reminder_available());
 damaged_record();
 puts("reminder: missing files, signed errno, read errors, corrupt records, durable commit, reboot, dismiss, clock jumps, timezone, uninstall, reset and RTC failures passed");return 0;
}
