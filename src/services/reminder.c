#include "reminder_internal.h"
#include "epd_reminder.h"
#include "epd_memory.h"
#include "app_service_internal.h"
#include "storage_file.h"
#include "system_time.h"
#include "system_reset.h"
#include "platform/epd_system_internal.h"
#include <rtdevice.h>
#include <drivers/alarm.h>
#include <drivers/pm.h>
#include <dfs_posix.h>
#include <rtm.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <rthw.h>
#include "bf0_hal.h"

#define ROOT "/flash/system/reminders"
#define MAGIC 0x524d4431u
typedef struct { epd_reminder_t rule; int64_t after; } entry_t;
typedef struct { uint32_t count, bytes; bool enabled; epd_notification_t notice; } header_t;
typedef struct agenda {
    struct agenda *next;
    char app[8];
    header_t header;
    entry_t *entries;
    bool pending_ack;
    char ack[64];
} agenda_t;
static rt_mutex_t mutex;
static struct rt_event event;
static agenda_t *agendas;
static bool started, restored, rtc_ready, storage_ready;
static rt_alarm_t alarm;
static int64_t armed;
typedef struct ack {struct ack *next;char app[32],id[64];} ack_t;
static ack_t *acknowledgements;

static bool identity(epd_service_t *owner, char app[8])
{ return owner && epd_service_is_worker(owner) && epd_service_identity(owner,app,8); }
static agenda_t *find(const char *app)
{ for(agenda_t *a=agendas;a;a=a->next)if(!strcmp(a->app,app))return a;return NULL; }
static void path_for(const char *app,char path[96]) { snprintf(path,96,ROOT "/%s.bin",app); }
static bool valid_id(const char *app)
{
    size_t n=strlen(app); if(!n||n>7)return false;
    for(size_t i=0;i<n;++i)if(!((app[i]>='a'&&app[i]<='z')||(app[i]>='0'&&app[i]<='9')||app[i]=='_'))return false;
    return true;
}
static void *read_record(const char *app, size_t *size, bool *absent)
{
    char path[96];struct stat st;path_for(app,path);*absent=false;*size=0;
    storage_lock();
    bool recovered=storage_file_recover(path);
    int result=stat(path,&st), error=errno;
    if(result && (error==ENOENT || error==-ENOENT))*absent=true;
    size_t max=16+sizeof(header_t)+EPD_REMINDER_MAX*sizeof(entry_t)+EPD_REMINDER_DATA_MAX;
    void *p=NULL;
    if(recovered && !result && st.st_size>=(int)(16+sizeof(header_t)) && (size_t)st.st_size<=max) {
        *size=(size_t)st.st_size-16;p=epd_app_alloc(*size,EPD_APP_PSRAM);
        if(p&&!storage_record_load(path,MAGIC,1,p,*size)){epd_app_free(p);p=NULL;}
    }
    storage_unlock();return p;
}
static bool valid_blob(const header_t *h,size_t n)
{
    if(h->count>EPD_REMINDER_MAX||h->bytes>EPD_REMINDER_DATA_MAX||
       n!=sizeof(*h)+h->count*sizeof(entry_t)+h->bytes)return false;
    const entry_t *e=(const entry_t *)(h+1);
    for(uint32_t i=0;i<h->count;++i) {
        if(!e[i].rule.id||!epd_calendar_rule_valid(&e[i].rule.rule)||e[i].rule.lead_minutes<0||
           e[i].rule.lead_minutes>525600||!memchr(e[i].rule.title,0,sizeof(e[i].rule.title)))return false;
        for(uint32_t j=0;j<i;++j)if(e[j].rule.id==e[i].rule.id)return false;
    }
    return memchr(h->notice.id,0,sizeof(h->notice.id)) && memchr(h->notice.app_id,0,sizeof(h->notice.app_id)) &&
           memchr(h->notice.title,0,sizeof(h->notice.title)) && memchr(h->notice.text,0,sizeof(h->notice.text)) &&
           memchr(h->notice.page,0,sizeof(h->notice.page));
}
static bool persist(agenda_t *a,const void *payload)
{
    size_t size=sizeof(header_t)+a->header.count*sizeof(entry_t)+a->header.bytes;
    uint8_t *blob=epd_app_alloc(size,EPD_APP_PSRAM);if(!blob)return false;
    memcpy(blob,&a->header,sizeof(header_t));memcpy(blob+sizeof(header_t),a->entries,a->header.count*sizeof(entry_t));
    if(a->header.bytes)memcpy(blob+sizeof(header_t)+a->header.count*sizeof(entry_t),payload,a->header.bytes);
    char path[96];path_for(a->app,path);
    bool ok=storage_mkdirs(ROOT)&&storage_record_save(path,MAGIC,1,blob,size);epd_app_free(blob);return ok;
}
static bool persist_progress(agenda_t *a)
{
    size_t n;bool absent;header_t *old=read_record(a->app,&n,&absent);
    bool ok=old&&valid_blob(old,n)&&old->bytes==a->header.bytes;
    if(ok)ok=persist(a,(uint8_t *)(old+1)+old->count*sizeof(entry_t));
    epd_app_free(old);return ok;
}
static void restore(void)
{
    storage_lock();DIR *d=opendir(ROOT);
    if(d) {
        struct dirent *de;
        while((de=readdir(d))!=NULL) {
            char app[8]={0};size_t len=strlen(de->d_name);
            if(len<5||len>11||strcmp(de->d_name+len-4,".bin"))continue;
            memcpy(app,de->d_name,len-4);if(!valid_id(app)||find(app))continue;
            size_t n;bool absent;header_t *h=read_record(app,&n,&absent);
            if(!h||!valid_blob(h,n)){epd_app_free(h);rt_kprintf("reminder: invalid record %s\n",app);continue;}
            agenda_t *a=epd_app_alloc(sizeof(*a),EPD_APP_PSRAM);
            entry_t *entries=epd_app_alloc(h->count*sizeof(entry_t)+1,EPD_APP_PSRAM);
            if(!a||!entries){epd_app_free(a);epd_app_free(entries);epd_app_free(h);continue;}
            memset(a,0,sizeof(*a));strcpy(a->app,app);a->header=*h;a->entries=entries;
            memcpy(entries,h+1,h->count*sizeof(entry_t));a->next=agendas;agendas=a;epd_app_free(h);
        }
        closedir(d);
    }
    storage_ready=storage_available(STORAGE_FLASH);
    storage_unlock();restored=storage_ready;
}
void reminder_time_changed(void) { if(started)rt_event_send(&event,1); }
static void due_callback(rt_alarm_t value,time_t stamp)
{ (void)value;(void)stamp;reminder_time_changed(); }
static void arm(int64_t utc)
{
    if(utc==armed&&rtc_ready)return;
    if(alarm){rt_alarm_delete(alarm);alarm=NULL;}armed=0;rtc_ready=true;
    if(!utc)return;
    time_t t=(time_t)(utc+28800);struct rt_alarm_setup setup={0};setup.flag=RT_ALARM_ONESHOT;
    if((int64_t)t!=utc+28800||!gmtime_r(&t,&setup.wktime)){rtc_ready=false;return;}
    alarm=rt_alarm_create(due_callback,&setup);
    rtc_ready=alarm&&rt_alarm_start(alarm)==RT_EOK;
    if(rtc_ready)armed=utc;else rt_kprintf("reminder: RTC alarm unavailable\n");
}
static void process(void)
{
    int64_t now=system_time_get_utc(),nearest=0;
    system_time_settings_t settings;system_time_get_settings(&settings);
    if(system_reset_blocks_apps()||!system_time_utc_valid(now)){arm(0);return;}
    rt_base_t level=rt_hw_interrupt_disable();ack_t *acks=acknowledgements;acknowledgements=NULL;rt_hw_interrupt_enable(level);
    while(acks){ack_t *next=acks->next;agenda_t *a=find(acks->app);if(a&&!strcmp(a->header.notice.id,acks->id)){strcpy(a->ack,acks->id);a->pending_ack=true;}rt_free(acks);acks=next;}
    for(agenda_t *a=agendas;a;a=a->next) {
        if(!a->header.enabled)continue;
        bool dirty=false;unsigned missed=0;int64_t latest=INT64_MIN;header_t previous=a->header;
        entry_t *backup=epd_app_alloc(a->header.count*sizeof(entry_t)+1,EPD_APP_PSRAM);
        if(!backup){storage_ready=false;continue;}
        memcpy(backup,a->entries,a->header.count*sizeof(entry_t));
        if(a->pending_ack&&!strcmp(a->ack,a->header.notice.id)) {a->header.notice.id[0]=0;dirty=true;}
        for(uint32_t i=0;i<a->header.count;++i) {
            entry_t *e=&a->entries[i];int64_t t;int32_t day;
            epd_calendar_rule_t rule=e->rule.rule;
            int zone=e->rule.date_only?settings.timezone_minutes*60:0;
            if(e->rule.date_only)rule.timezone_minutes=settings.timezone_minutes;
            int64_t lead=e->rule.lead_minutes*INT64_C(60);
            /* A clock jump aggregates missed occurrences into one notice. */
            if(epd_calendar_last(&rule,e->after-zone+lead,now+lead,&t,&day)) {
                e->after=t-lead+1+zone;dirty=true;++missed;
                if(t>=latest){latest=t;
                epd_notification_t *n=&a->header.notice;memset(n,0,sizeof(*n));strcpy(n->app_id,a->app);
                snprintf(n->id,sizeof(n->id),"rem:%lu:%ld",(unsigned long)e->rule.id,(long)day);
                snprintf(n->page,sizeof(n->page),"event:%lu:%ld",(unsigned long)e->rule.id,(long)day);
                snprintf(n->title,sizeof(n->title),"%s",e->rule.title);
                epd_date_t d=epd_date_from_day(day);snprintf(n->text,sizeof(n->text),"%04d-%02u-%02u %02u:%02u",d.year,d.month,d.day,rule.minute/60,rule.minute%60);}
            }
            if(epd_calendar_next(&rule,e->after-zone+lead,&t,NULL)&&(!nearest||t-lead<nearest))nearest=t-lead;
        }
        if(missed>1)snprintf(a->header.notice.text,sizeof(a->header.notice.text),"%u 项日程已到期",missed);
        if(dirty&&!persist_progress(a)) {
            a->header=previous;memcpy(a->entries,backup,a->header.count*sizeof(entry_t));
            rt_kprintf("reminder: save failed %s\n",a->app);
            storage_ready=false;
        }
        else if(dirty)a->pending_ack=false;
        epd_app_free(backup);
        if(a->header.notice.id[0])epd_notification_publish_resident(&a->header.notice);
    }
    arm(nearest>now?nearest:0);
}
static void run(void *unused)
{
    (void)unused;
    for(;;) {
        rt_pm_request(PM_SLEEP_MODE_IDLE);
        rt_mutex_take(mutex,RT_WAITING_FOREVER);
        if(!restored)restore();process();rt_mutex_release(mutex);
        rt_pm_release(PM_SLEEP_MODE_IDLE);
        rt_uint32_t bits;rt_event_recv(&event,1,RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR,RT_WAITING_FOREVER,&bits);
    }
}
void reminder_start(void)
{
    if(started)return;mutex=rt_mutex_create("reminder",RT_IPC_FLAG_PRIO);
    if(!mutex)return;
    if(rt_event_init(&event,"reminder",RT_IPC_FLAG_PRIO)!=RT_EOK){rt_mutex_delete(mutex);mutex=NULL;return;}
    rt_thread_t t=epd_thread_create_psram("reminder",run,NULL,8192,24,10);
    if(!t){rt_event_detach(&event);rt_mutex_delete(mutex);mutex=NULL;return;}
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_RTC,AON_PIN_MODE_HIGH);
    started=true;rtc_ready=true;rt_thread_startup(t);
}
bool epd_reminder_available(void){return started&&rtc_ready&&storage_ready;}
RTM_EXPORT(epd_reminder_available);
epd_reminder_result_t epd_reminder_load(epd_service_t *owner,void **data,size_t *size)
{
    char app[8];if(!data||!size||!identity(owner,app))return EPD_REMINDER_INVALID;
    *data=NULL;*size=0;if(!started||!storage_available(STORAGE_FLASH))return EPD_REMINDER_UNAVAILABLE;
    rt_mutex_take(mutex,RT_WAITING_FOREVER);size_t n;bool absent;header_t *h=read_record(app,&n,&absent);
    epd_reminder_result_t result=absent?EPD_REMINDER_EMPTY:EPD_REMINDER_IO;
    if(h&&valid_blob(h,n)) {
        if(!h->enabled){char path[96];path_for(app,path);h->enabled=true;
            if(!storage_record_save(path,MAGIC,1,h,n)){epd_app_free(h);rt_mutex_release(mutex);return EPD_REMINDER_IO;}
            agenda_t *a=find(app);if(a)a->header.enabled=true;reminder_time_changed();}
        void *p=epd_app_alloc(h->bytes+1,EPD_APP_PSRAM);
        if(p){memcpy(p,(uint8_t *)(h+1)+h->count*sizeof(entry_t),h->bytes);*data=p;*size=h->bytes;result=EPD_REMINDER_OK;}
        else result=EPD_REMINDER_MEMORY;
    }
    epd_app_free(h);rt_mutex_release(mutex);return result;
}
RTM_EXPORT(epd_reminder_load);
epd_reminder_result_t epd_reminder_commit(epd_service_t *owner,const void *data,size_t size,const epd_reminder_t *rules,size_t count)
{
    char app[8];if(!identity(owner,app)||(!data&&size)||size>EPD_REMINDER_DATA_MAX||count>EPD_REMINDER_MAX||(!rules&&count))return EPD_REMINDER_INVALID;
    for(size_t i=0;i<count;++i) {
        if(!rules[i].id||!epd_calendar_rule_valid(&rules[i].rule)||rules[i].lead_minutes<0||rules[i].lead_minutes>525600||!memchr(rules[i].title,0,96))return EPD_REMINDER_INVALID;
        for(size_t j=0;j<i;++j)if(rules[i].id==rules[j].id)return EPD_REMINDER_INVALID;
    }
    if(!started)return EPD_REMINDER_UNAVAILABLE;
    agenda_t *a=epd_app_alloc(sizeof(*a),EPD_APP_PSRAM);entry_t *entries=epd_app_alloc(count*sizeof(entry_t)+1,EPD_APP_PSRAM);
    if(!a||!entries){epd_app_free(a);epd_app_free(entries);return EPD_REMINDER_MEMORY;}
    memset(a,0,sizeof(*a));strcpy(a->app,app);a->entries=entries;a->header.count=count;a->header.bytes=size;a->header.enabled=true;
    rt_mutex_take(mutex,RT_WAITING_FOREVER);if(!restored)restore();agenda_t *old=find(app);
    int64_t now=system_time_get_utc();system_time_settings_t settings;system_time_get_settings(&settings);
    for(size_t i=0;i<count;++i) {
        entries[i].rule=rules[i];entries[i].after=system_time_utc_valid(now)?now:INT64_C(1704067200);
        if(rules[i].date_only)entries[i].after+=settings.timezone_minutes*60;
        if(old)for(uint32_t j=0;j<old->header.count;++j)if(old->entries[j].rule.id==rules[i].id) {
            if(old->entries[j].rule.date_only==rules[i].date_only)entries[i].after=old->entries[j].after;
            break;
        }
    }
    char dismiss[64]={0};
    if(old){a->header.notice=old->header.notice;unsigned long notice_id=0;
        if(sscanf(old->header.notice.id,"rem:%lu:",&notice_id)==1){bool keep=false;
            for(size_t i=0;i<count;++i)if(rules[i].id==notice_id)keep=true;
            if(!keep){strcpy(dismiss,old->header.notice.id);a->header.notice.id[0]=0;}}}
    bool ok=persist(a,data);
    if(ok) {
        storage_ready=true;
        if(old){epd_app_free(old->entries);old->entries=entries;old->header=a->header;epd_app_free(a);}
        else {a->next=agendas;agendas=a;}
    } else {epd_app_free(entries);epd_app_free(a);}
    rt_mutex_release(mutex);if(ok){if(*dismiss)epd_notification_dismiss(app,dismiss);reminder_time_changed();}return ok?EPD_REMINDER_OK:EPD_REMINDER_IO;
}
RTM_EXPORT(epd_reminder_commit);
bool reminder_dismiss(const char *app,const char *id)
{
    if(!started||!app||!id||strncmp(id,"rem:",4))return true;
    ack_t *ack=rt_calloc(1,sizeof(*ack));if(!ack)return false;
    snprintf(ack->app,sizeof(ack->app),"%s",app);snprintf(ack->id,sizeof(ack->id),"%s",id);
    rt_base_t level=rt_hw_interrupt_disable();ack->next=acknowledgements;acknowledgements=ack;rt_hw_interrupt_enable(level);
    reminder_time_changed();return true;
}
bool reminder_remove_app(const char *app)
{
    if(!app||!valid_id(app))return false;
    if(mutex)rt_mutex_take(mutex,RT_WAITING_FOREVER);
    size_t n;bool absent;header_t *h=read_record(app,&n,&absent);bool ok=absent;
    if(h&&valid_blob(h,n)){h->enabled=false;h->notice.id[0]=0;char path[96];path_for(app,path);ok=storage_record_save(path,MAGIC,1,h,n);}
    if(ok){agenda_t *a=find(app);if(a){a->header.enabled=false;a->header.notice.id[0]=0;}}
    epd_app_free(h);if(mutex)rt_mutex_release(mutex);reminder_time_changed();return ok;
}
