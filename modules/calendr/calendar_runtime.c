#include "calendar_runtime.h"
#include "epd_reminder.h"
#include "epd_memory.h"
#include "storage.h"
#include "storage_file.h"
#include "system_time.h"
#include "calendar_exchange.h"
#include "calendar_store.h"
#include <mod_installer.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    rt_mutex_t mutex;
    epd_service_t *owner;
    cal_book_t book,imported;
    cal_status_t status;
    cal_event_t edit,draft;
    int32_t draft_day;bool draft_instance,has_draft;
    cal_command_t command;
    uint32_t id,query_revision;
    unsigned query_fields;
    int32_t search_day;
    int32_t day,first,last;
    int timezone,kind;
    size_t offset;
    char query[241],path[512];
    bool instance,pending,refresh,query_running,query_notified;
    cal_book_t preferences;
    struct {int32_t first;int timezone;uint32_t revision,used;uint64_t mask;} cache[3];
    unsigned cache_slot;uint32_t book_revision,result_book_revision;
} runtime_t;
static runtime_t *state;
static void lock(void){rt_mutex_take(state->mutex,RT_WAITING_FOREVER);}
static void unlock(void){rt_mutex_release(state->mutex);}
static const char *load_result_name(epd_reminder_result_t result)
{
    switch(result){
    case EPD_REMINDER_OK:return "ok";
    case EPD_REMINDER_EMPTY:return "empty";
    case EPD_REMINDER_INVALID:return "invalid";
    case EPD_REMINDER_IO:return "io";
    case EPD_REMINDER_MEMORY:return "memory";
    case EPD_REMINDER_UNAVAILABLE:return "unavailable";
    default:return "unknown";
    }
}
static bool commit(cal_book_t *book)
{
    void *bytes=NULL;size_t size=0;
    if(!cal_document_encode(book,state->has_draft?&state->draft:NULL,state->draft_day,state->draft_instance,&bytes,&size))return false;
    epd_reminder_t *rules=cal_alloc(book->count*sizeof(*rules)+1);size_t count=0;
    if(!rules){cal_free(bytes);return false;}
    for(size_t i=0;i<book->count;++i) {
        cal_event_t *e=book->events[i];if(e->reminder_minutes<0)continue;
        rules[count].id=e->id;rules[count].rule=e->rule;rules[count].lead_minutes=e->reminder_minutes;
        rules[count].date_only=e->all_day;
        snprintf(rules[count].title,sizeof(rules[count].title),"%.95s",e->title);
        /* Preserve a complete UTF-8 suffix when the notification title is shorter. */
        size_t n=strlen(rules[count].title);while(n&&!cal_utf8(rules[count].title,80))rules[count].title[--n]=0;
        ++count;
    }
    bool ok=epd_reminder_commit(state->owner,bytes,size,rules,count)==EPD_REMINDER_OK;
    cal_free(rules);cal_free(bytes);return ok;
}
static uint64_t month_mask(int32_t first,int zone)
{
    for(int i=0;i<3;++i)if(state->cache[i].revision==state->book_revision&&state->cache[i].first==first&&state->cache[i].timezone==zone){state->cache[i].used=++state->cache_slot;return state->cache[i].mask;}
    unsigned slot=0;for(unsigned i=1;i<3;++i)if(state->cache[i].used<state->cache[slot].used)slot=i;state->cache[slot].used=++state->cache_slot;state->cache[slot].first=first;state->cache[slot].timezone=zone;state->cache[slot].revision=state->book_revision;
    return state->cache[slot].mask=cal_busy_days(&state->book,first,42,zone);
}
static void query_rows(void)
{
    lock();int32_t first=state->first,last=state->last;int tz=state->timezone,kind=state->kind;size_t offset=state->offset;
    uint32_t revision=state->query_revision;unsigned fields=state->query_fields;int32_t search_day=state->search_day;
    char query[241];strcpy(query,state->query);state->refresh=false;state->query_notified=false;state->query_running=revision!=0;unlock();
    if(!revision)return;
    cal_occurrence_t *rows=cal_alloc(CAL_ROWS*sizeof(*rows));cal_row_t *copy=cal_alloc(CAL_ROWS*sizeof(*copy));
    if(!rows||!copy){cal_free(rows);cal_free(copy);lock();state->query_running=false;if(revision==state->query_revision){strcpy(state->status.error,"内存不足，日程查询未完成");++state->status.revision;}unlock();return;}
    size_t count=(fields&CAL_QUERY_SEARCH)?cal_search(&state->book,search_day,tz,query,kind,rows,CAL_ROWS,offset):cal_query(&state->book,first,last,tz,query,kind,rows,CAL_ROWS,offset);
    epd_date_t date=epd_date_from_day(first);date.day=1;int32_t grid=epd_date_day(date);grid-=(epd_date_weekday(grid)+7-state->book.first_weekday)%7;
    uint64_t mask=(fields&CAL_QUERY_GRID)?month_mask(grid,tz):0;cal_row_t upcoming[3]={0};size_t upcoming_count=0;
    cal_occurrence_t *future=(fields&CAL_QUERY_UPCOMING)?cal_alloc((state->book.count+1)*sizeof(*future)):NULL;
    if(future){size_t total=cal_search(&state->book,first,tz,NULL,-1,future,state->book.count,0);
        for(size_t i=0;i<total&&upcoming_count<3;++i){cal_event_t *e=cal_find(&state->book,future[i].id);if(e->kind==CAL_EVENT||future[i].day<=first)continue;
            cal_row_t *r=&upcoming[upcoming_count++];r->occurrence=future[i];r->kind=e->kind;r->duration=e->duration_minutes;strcpy(r->title,e->title);}
        cal_free(future);}
    for(size_t i=0;i<count;++i){cal_event_t *e=cal_find(&state->book,rows[i].id);copy[i].occurrence=rows[i];copy[i].duration=e->duration_minutes;copy[i].kind=e->kind;strcpy(copy[i].title,e->title);strcpy(copy[i].place,e->place);}
    lock();state->query_running=false;if(revision!=state->query_revision){unlock();cal_free(copy);cal_free(rows);return;}memcpy(state->status.rows,copy,count*sizeof(*copy));state->status.count=count;state->status.grid_first=grid;state->status.busy_days=mask;state->status.upcoming_count=upcoming_count;memcpy(state->status.upcoming,upcoming,sizeof(upcoming));
    state->status.preferences=state->book;state->status.preferences.events=NULL;state->status.preferences.count=0;
    state->status.query_revision=revision;state->result_book_revision=state->book_revision;++state->status.data_revision;
    ++state->status.revision;unlock();cal_free(copy);cal_free(rows);
}
static void work(epd_service_t *owner)
{
    lock();state->owner=owner;unlock();
    if(!epd_service_sleep_acquire(owner)){
        rt_kprintf("calendr: load blocked stage=sleep-guard cancelled=%u\n",(unsigned)epd_service_cancelled(owner));return;
    }
    rt_kprintf("calendr: load begin flash=%u reminder=%u\n",(unsigned)storage_available(STORAGE_FLASH),(unsigned)epd_reminder_available());
    void *data=NULL;size_t size=0;uint32_t started=rt_tick_get();
    epd_reminder_result_t loaded=epd_reminder_load(owner,&data,&size);
    rt_kprintf("calendr: load result=%s(%d) bytes=%lu ticks=%lu\n",load_result_name(loaded),(int)loaded,(unsigned long)size,(unsigned long)(rt_tick_get()-started));
    started=rt_tick_get();
    bool ok=loaded==EPD_REMINDER_EMPTY||(loaded==EPD_REMINDER_OK&&cal_document_decode(&state->book,&state->draft,&state->draft_day,&state->draft_instance,data,size));
    rt_kprintf("calendr: decode=%s ticks=%lu ready=%u events=%u draft=%u\n",
               loaded!=EPD_REMINDER_OK?"skipped":ok?"ok":"failed",(unsigned long)(rt_tick_get()-started),
               (unsigned)ok,(unsigned)state->book.count,(unsigned)(state->draft.duration_minutes!=0));
    state->has_draft=state->draft.duration_minutes!=0;cal_free(data);lock();state->status.has_draft=state->has_draft;state->status.ready=ok;state->status.busy=false;
    state->status.preferences=state->book;state->status.preferences.events=NULL;state->status.preferences.count=0;
    if(!ok)strcpy(state->status.error,"日历数据读取失败");++state->status.revision;unlock();
    for(;;) {
        lock();bool pending=state->pending,refresh=state->refresh;state->pending=false;unlock();
        if(pending) {
            cal_book_t candidate;cal_book_init(&candidate);void *snapshot=NULL;size_t n=0;
            ok=cal_encode(&state->book,&snapshot,&n)&&cal_decode(&candidate,snapshot,n);cal_free(snapshot);
            if(ok) switch(state->command) {
            case CAL_SAVE:
                if(!state->edit.uid[0]||state->instance)snprintf(state->edit.uid,sizeof(state->edit.uid),"%llx-%lx-%lu@epd",(unsigned long long)system_time_get_utc(),(unsigned long)rt_tick_get(),(unsigned long)candidate.next_id);
                ok=state->instance?cal_override(&candidate,state->id,state->day,&state->edit):cal_put(&candidate,&state->edit);break;
            case CAL_REMOVE:ok=cal_delete(&candidate,state->id,state->day,state->instance);break;
            case CAL_PREFERENCES:
                candidate.first_weekday=state->preferences.first_weekday;candidate.default_view=state->preferences.default_view;
                candidate.default_duration=state->preferences.default_duration;candidate.lunar=state->preferences.lunar;candidate.festivals=state->preferences.festivals;break;
            case CAL_PREVIEW:cal_book_clear(&state->imported);ok=cal_import_file(&state->imported,state->path);break;
            case CAL_IMPORT:ok=state->status.preview&&cal_merge(&candidate,&state->imported,!state->instance);break;
            case CAL_CANCEL_IMPORT:cal_book_clear(&state->imported);break;
            case CAL_EXPORT:ok=cal_export_file(&candidate,state->path);break;
            default:break;
            }
            bool draft_changed=false;bool old_has=state->has_draft,old_instance=state->draft_instance;int32_t old_day=state->draft_day;cal_event_t old_draft={0};
            if(ok&&(state->command==CAL_DRAFT||state->command==CAL_CLEAR_DRAFT||state->command==CAL_SAVE)){draft_changed=true;old_draft=state->draft;memset(&state->draft,0,sizeof(state->draft));state->has_draft=state->command==CAL_DRAFT;
                if(state->has_draft){ok=cal_event_copy(&state->draft,&state->edit);state->draft_day=state->day;state->draft_instance=state->instance;}}
            bool writes=state->command!=CAL_EXPORT&&state->command!=CAL_PREVIEW&&state->command!=CAL_CANCEL_IMPORT;
            if(ok&&writes)ok=commit(&candidate);
            lock();
            if(draft_changed){if(!ok){cal_event_clear(&state->draft);state->draft=old_draft;memset(&old_draft,0,sizeof(old_draft));state->has_draft=old_has;state->draft_day=old_day;state->draft_instance=old_instance;}cal_event_clear(&old_draft);}
            state->status.has_draft=state->has_draft;
            if(ok&&writes){cal_book_t old=state->book;state->book=candidate;candidate=old;++state->book_revision;
                state->status.preferences=state->book;state->status.preferences.events=NULL;state->status.preferences.count=0;}
            state->status.busy=false;state->status.error[0]=0;state->status.result[0]=0;
            if(!ok){if(state->command==CAL_PREVIEW)strcpy(state->status.error,"无法导入：文件损坏、超限或包含不支持的日历规则");
                else if(state->command==CAL_EXPORT&&!cal_ics_supported(&state->book))strcpy(state->status.error,"此日历包含农历或特殊重复规则，请选择完整备份");
                else strcpy(state->status.error,"操作失败，原日历数据已保留");}
            else if(state->command==CAL_EXPORT)snprintf(state->status.result,sizeof(state->status.result),"已导出：%.130s",state->path);
            if(state->command==CAL_PREVIEW){state->status.preview=ok;state->status.import_new=state->status.import_existing=0;
                for(size_t i=0;ok&&i<state->imported.count;++i){bool exists=false;for(size_t j=0;j<state->book.count;++j)if(!strcmp(state->imported.events[i]->uid,state->book.events[j]->uid)){exists=true;break;}
                    if(exists)++state->status.import_existing;else ++state->status.import_new;}}
            if(state->command==CAL_CANCEL_IMPORT||(ok&&state->command==CAL_IMPORT)){state->status.preview=false;cal_book_clear(&state->imported);}
            ++state->status.completed;++state->status.revision;cal_event_clear(&state->edit);unlock();cal_book_clear(&candidate);refresh=true;
        }
        if(refresh&&state->status.ready)query_rows();
        lock();if(!state->status.ready)state->refresh=false;bool again=state->pending||state->refresh;
        if(!again)epd_service_sleep_release(owner);unlock();
        if(epd_service_cancelled(owner))break;
        if(!again)epd_service_wait(owner,RT_WAITING_FOREVER);
    }
    epd_service_sleep_release(owner);
}
static bool flush(void){if(!state)return true;if(!cal_ui_flush())return false;lock();bool ok=!state->status.busy&&!state->pending;unlock();return ok;}
static bool process(epd_service_t *owner,bool stopping)
{(void)owner;if(!stopping)return false;cal_ui_retire();lock();state->owner=NULL;state->status.ready=false;unlock();return true;}
const epd_background_t epd_app_background={.run=work,.stack_size=16384,.priority=25,.flush=flush,.ui_process=process};
void cal_runtime_init(void)
{
    if(state)return;state=cal_alloc(sizeof(*state));if(!state){rt_kprintf("calendr: init failed stage=memory bytes=%lu\n",(unsigned long)sizeof(*state));return;}
    state->mutex=rt_mutex_create("calendr",RT_IPC_FLAG_PRIO);if(!state->mutex){rt_kprintf("calendr: init failed stage=mutex\n");cal_free(state);state=NULL;return;}
    cal_book_init(&state->book);cal_book_init(&state->imported);cal_book_init(&state->status.preferences);state->kind=-1;state->status.busy=true;state->book_revision=1;
}
void cal_runtime_cleanup(void){if(!state)return;cal_book_clear(&state->book);cal_book_clear(&state->imported);cal_event_clear(&state->edit);cal_event_clear(&state->draft);rt_mutex_delete(state->mutex);cal_free(state);state=NULL;}
bool cal_runtime_start(void)
{
    if(!state){rt_kprintf("calendr: start failed stage=runtime\n");return false;}
    if(app_service_running("calendr")){
        lock();bool ready=state->status.ready,busy=state->status.busy,worker=state->owner!=NULL;unlock();
        rt_kprintf("calendr: service existing ready=%u busy=%u worker=%u\n",(unsigned)ready,(unsigned)busy,(unsigned)worker);return true;
    }
    bool ok=app_service_start("calendr",&epd_app_background,dlmodule_find("calendr"))!=NULL;
    rt_kprintf("calendr: service start=%s\n",ok?"ok":"failed");return ok;
}
void cal_runtime_status(cal_status_t *out){if(!out)return;if(!state){memset(out,0,sizeof(*out));strcpy(out->error,"日历服务未启动");return;}lock();*out=state->status;unlock();}
bool cal_runtime_event(uint32_t id,cal_event_t *out){if(!state)return false;lock();cal_event_t *e=state->status.ready?cal_find(&state->book,id):NULL;bool ok=e&&cal_event_copy(out,e);unlock();return ok;}
static void wake_query(void)
{
    if(state->refresh&&!state->query_notified&&state->owner&&epd_service_sleep_acquire(state->owner)){
        epd_service_wake(state->owner);state->query_notified=true;
    }
}
uint32_t cal_runtime_query(int32_t first,int32_t last,int tz,const char *query,int kind,size_t offset,unsigned fields)
{
    if(!state||!(fields&CAL_QUERY_ROWS))return 0;
    int32_t search_day=(fields&CAL_QUERY_SEARCH)?system_time_local_day(system_time_get_utc()):0;
    lock();if(state->query_revision&&state->first==first&&state->last==last&&state->timezone==tz&&state->kind==kind&&
        state->offset==offset&&state->query_fields==fields&&state->search_day==search_day&&!strcmp(state->query,query?query:"")&&
        (state->refresh||state->query_running||(state->status.query_revision==state->query_revision&&state->result_book_revision==state->book_revision))){
        wake_query();uint32_t revision=state->query_revision;unlock();return revision;}
    if(!++state->query_revision)++state->query_revision;
    state->first=first;state->last=last;state->timezone=tz;state->kind=kind;state->offset=offset;state->query_fields=fields;state->search_day=search_day;
    snprintf(state->query,sizeof(state->query),"%s",query?query:"");state->refresh=true;
    wake_query();
    uint32_t revision=state->query_revision;unlock();return revision;
}
bool cal_runtime_submit(cal_command_t cmd,const cal_event_t *e,uint32_t id,int32_t day,bool instance,const char *path,const cal_book_t *preferences)
{
    if(!state)return false;lock();
    if(!state->status.ready||state->status.busy||!state->owner||!epd_service_sleep_acquire(state->owner)){unlock();return false;}
    if(e&&!cal_event_copy(&state->edit,e)){epd_service_sleep_release(state->owner);unlock();return false;}
    state->command=cmd;state->id=id;state->day=day;state->instance=instance;if(preferences)state->preferences=*preferences;
    snprintf(state->path,sizeof(state->path),"%s",path?path:"");state->pending=true;state->status.busy=true;state->status.error[0]=0;++state->status.revision;
    epd_service_wake(state->owner);unlock();return true;
}

bool cal_runtime_draft(cal_event_t *out,int32_t *day,bool *instance)
{if(!state)return false;lock();bool ok=state->status.ready&&!state->status.busy&&state->has_draft&&cal_event_copy(out,&state->draft);if(ok){*day=state->draft_day;*instance=state->draft_instance;}unlock();return ok;}
