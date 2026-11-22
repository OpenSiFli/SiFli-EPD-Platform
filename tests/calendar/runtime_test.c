#include "rtthread.h"
#include "host_fs.h"
#include "calendar_runtime.h"
#include "epd_reminder.h"
#include "epd_memory.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include "../runtime_memory_retry/fixture.h"
bool epd_memory_app_backend(epd_memory_region_t region,epd_memory_backend_t *out)
{CHECK(region==EPD_MEMORY_REGION_HCPU_PSRAM);*out=0;return true;}
size_t epd_app_allocation_peak(size_t bytes){return bytes+sizeof(struct rt_memheap_item);}
epd_memory_recovery_result_t epd_memory_recovery_begin(epd_memory_recovery_t *retry,
    epd_memory_backend_t backend,size_t bytes,bool failed)
{
    (void)backend;CHECK(failed);retry->used=true;
    if(!bytes||bytes>epd_app_psram_capacity())return EPD_MEMORY_RECOVERY_DENIED;
    retry->generation=app_memory_pressure_allocation_failed(bytes);return EPD_MEMORY_RECOVERY_PENDING;
}
epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *retry,bool cancelled)
{(void)retry;return cancelled?EPD_MEMORY_RECOVERY_DENIED:memory_pending?EPD_MEMORY_RECOVERY_PENDING:EPD_MEMORY_RECOVERY_READY;}
static char logs[8192];
static void capture_log(const char *format,...){size_t n=strlen(logs);va_list args;va_start(args,format);vsnprintf(logs+n,sizeof(logs)-n,format,args);va_end(args);}
#undef rt_kprintf
#define rt_kprintf capture_log
static bool stop,held,write_fail,read_fail,supersede,block_wake,fail_query_alloc;static unsigned step,scenario,waits,wakes,row_queries,grid_queries,search_queries;static void *durable;static size_t durable_size;static epd_reminder_t registered[8];static size_t registered_count;
static epd_reminder_result_t load_reply=EPD_REMINDER_OK;
static bool expect_ready;
static bool service_registered=true,ui_present;
static unsigned service_stops,service_starts,ui_requests,ui_retires;
static void close_during_commit(void);
static bool inject_close;
static bool ordinary_admit=true, verify_cleanup_io;
static void check_cleanup_io(void);
void *epd_app_alloc(size_t n,epd_app_memory_t r){return calloc(1,n?n:1);}void epd_app_free(void*p){free(p);}
epd_reminder_result_t epd_reminder_commit(epd_service_t *o,const void *p,size_t n,const epd_reminder_t*r,size_t count){if(verify_cleanup_io)check_cleanup_io();if(inject_close){inject_close=false;close_during_commit();}if(write_fail)return EPD_REMINDER_IO;void*copy=malloc(n);CHECK(copy);memcpy(copy,p,n);free(durable);durable=copy;durable_size=n;CHECK(count<8);memcpy(registered,r,count*sizeof(*r));registered_count=count;return EPD_REMINDER_OK;}
epd_reminder_result_t epd_reminder_load(epd_service_t *o,void **p,size_t*n){*p=NULL;*n=0;if(read_fail)return EPD_REMINDER_IO;if(load_reply!=EPD_REMINDER_OK)return load_reply;if(!durable)return EPD_REMINDER_EMPTY;*p=malloc(durable_size);memcpy(*p,durable,durable_size);*n=durable_size;return EPD_REMINDER_OK;}
bool epd_reminder_available(void){return true;}
bool epd_service_sleep_acquire(epd_service_t *o){if(stop||block_wake)return false;held=true;return true;}
#include "../unified_operation/cleanup_ports.h"
static void check_cleanup_io(void){CHECK(test_cleanup_holds==1&&test_cleanup_context==2);}
bool epd_service_reclaim_hold(epd_service_t *owner){(void)owner;return !stop&&ordinary_admit;}
void epd_service_reclaim_release(epd_service_t *owner){(void)owner;}
bool epd_service_mark_dirty(epd_service_t *owner){return owner!=NULL;}
void epd_service_sleep_release(epd_service_t*o){held=false;}
void epd_service_wake(epd_service_t*o){++wakes;}
bool epd_service_cancelled(epd_service_t*o){return stop;}
bool app_service_running(const char *id){return service_registered;}
epd_service_t *app_service_start(const char*i,const epd_background_t*d,void*m){CHECK(!service_registered);service_registered=true;++service_starts;return (void*)1;}
void app_service_stop(const char *id){CHECK(!strcmp(id,"calendr"));stop=true;++service_stops;}
void epd_service_ui_request(epd_service_t *o,uint32_t delay){CHECK(o==(void*)1&&delay<=100);++ui_requests;}
bool cal_ui_present(void){return ui_present;}
bool cal_ui_flush(void){return true;}void cal_ui_retire(void){++ui_retires;}
static size_t counted_query(const cal_book_t *b,int32_t first,int32_t last,int tz,const char *q,int kind,cal_occurrence_t *out,size_t cap,size_t offset)
{++row_queries;if(supersede){supersede=false;cal_runtime_query(first+1,last+1,tz,q,kind,offset,CAL_QUERY_ROWS);}return cal_query(b,first,last,tz,q,kind,out,cap,offset);}
static size_t counted_search(const cal_book_t *b,int32_t day,int tz,const char *q,int kind,cal_occurrence_t *out,size_t cap,size_t offset)
{++search_queries;return cal_search(b,day,tz,q,kind,out,cap,offset);}
static uint64_t counted_grid(const cal_book_t *b,int32_t first,unsigned days,int tz)
{++grid_queries;return cal_busy_days(b,first,days,tz);}
static bool persistent_query_oom;
static size_t fail_query_after=SIZE_MAX;
static void *query_alloc(size_t n){if(persistent_query_oom||!fail_query_after)return NULL;if(fail_query_after!=SIZE_MAX)--fail_query_after;if(fail_query_alloc){fail_query_alloc=false;return NULL;}return cal_alloc(n);}
#define cal_alloc query_alloc
#define cal_query counted_query
#define cal_search counted_search
#define cal_busy_days counted_grid
#include "../../modules/calendr/calendar_runtime.c"
#undef cal_query
#undef cal_search
#undef cal_busy_days
#undef cal_alloc
static cal_event_t fixture(void){cal_event_t e={0};strcpy(e.title,"评审");e.rule.start_day=epd_date_day((epd_date_t){2026,10,30});e.rule.until_day=e.rule.start_day;e.rule.minute=540;e.rule.timezone_minutes=480;e.duration_minutes=60;e.reminder_minutes=10;return e;}
static void close_during_commit(void)
{cal_event_t newer=fixture();strcpy(newer.title,"关闭期间的新草稿");cal_runtime_ui_closed(&newer,77,true);CHECK(!newer.duration_minutes);}
static void replace_waiting_query(void)
{cal_runtime_query(state->first+1,state->last+1,480,NULL,-1,0,CAL_QUERY_ROWS);memory_pending=false;}
uint32_t epd_service_wait(epd_service_t*o,rt_int32_t ticks){CHECK(!held);CHECK(++waits<25);cal_event_t e=fixture();
 if(scenario==7){
  switch(step++){
  case 0:
   ordinary_admit=false;verify_cleanup_io=true;strcpy(e.title,"RELEASE cleanup retry");
   cal_runtime_ui_closed(&e,23,false);CHECK(!flush()&&state->work.cleanup);
   test_cleanup_enter_fail=true;write_fail=true;break;
  case 1:{
   CHECK(state->pending&&state->saving_closed&&!state->closed_pending);
   CHECK(!durable&&!test_cleanup_holds&&!test_cleanup_references&&test_cleanup_context==1);
   unsigned before=wakes;CHECK(!flush()&&wakes==before+1);
   test_cleanup_enter_fail=false;break;
  }
  case 2:
   CHECK(state->closed_pending&&state->closed_failed&&!state->pending&&!test_cleanup_holds);
   CHECK(!durable&&!strcmp(state->closed_draft.title,"RELEASE cleanup retry"));
   CHECK(!flush()&&state->work.cleanup);write_fail=false;break;
  case 3:{
   CHECK(durable&&!test_cleanup_holds&&!test_cleanup_references&&test_cleanup_context==1);
   CHECK(!state->closed_pending&&cal_runtime_draft_saved()&&flush());
   cal_book_t restored;cal_event_t draft={0};int32_t day=0;bool instance=false;
   cal_book_init(&restored);CHECK(cal_document_decode(&restored,&draft,&day,&instance,durable,durable_size));
   CHECK(day==23&&!strcmp(draft.title,"RELEASE cleanup retry"));cal_event_clear(&draft);cal_book_clear(&restored);
   CHECK(!process(o,false)&&stop);break;
  }
  default:CHECK(false);
  }return 0;
 }
 if(scenario==1){CHECK(!state->status.ready);CHECK(!state->refresh);stop=true;return 0;}
 if(scenario==2){CHECK(state->status.has_draft&&!strcmp(state->draft.title,"未保存的修改"));CHECK(state->book.count==1);stop=true;return 0;}
 if(scenario==3){CHECK(state->status.ready==expect_ready);CHECK(!state->status.busy);stop=true;return 0;}
 if(scenario==4){
  if(step++==0){strcpy(e.title,"关闭前草稿");e.notes=cal_alloc(16);strcpy(e.notes,"owned notes");char *owned=e.notes;cal_runtime_ui_closed(&e,11,false);CHECK(!e.notes&&state->closed_draft.notes==owned);inject_close=true;}
  else{CHECK(!state->closed_pending&&cal_runtime_draft_saved());CHECK(state->has_draft&&!strcmp(state->draft.title,"关闭期间的新草稿"));CHECK(state->draft_day==77&&state->draft_instance);CHECK(!process(o,false)&&stop);}
  return 0;
 }
 if(scenario==5){
  if(step++==0){strcpy(e.title,"写失败后保留");cal_runtime_ui_closed(&e,12,false);write_fail=true;}
  else if(step==2){CHECK(state->closed_pending&&state->closed_failed);CHECK(!flush());unsigned before=service_stops;CHECK(!process(o,false)&&service_stops==before);cal_event_t restored={0};int32_t day;bool instance;CHECK(cal_runtime_draft(&restored,&day,&instance)&&!cal_runtime_draft_saved());CHECK(!strcmp(restored.title,"写失败后保留")&&day==12);write_fail=false;strcpy(restored.title,"重开修改后保存");CHECK(cal_runtime_submit(CAL_DRAFT,&restored,0,day,instance,NULL,NULL));cal_event_clear(&restored);}
  else{CHECK(!state->closed_pending&&cal_runtime_draft_saved()&&flush());CHECK(!strcmp(state->draft.title,"重开修改后保存"));CHECK(!process(o,false)&&stop);}
  return 0;
 }
 if(scenario==6){
  int32_t occurrence=e.rule.start_day+1;
  switch(step++){
  case 0:
   e.rule.repeat=EPD_REPEAT_DAILY;e.rule.until_day=e.rule.start_day+7;
   CHECK(cal_runtime_submit(CAL_SAVE,&e,0,0,false,NULL,NULL));break;
  case 1:{
   CHECK(state->book.count==1);cal_event_t edit={0};CHECK(cal_runtime_event(1,&edit));
   strcpy(edit.title,"仅本次修改");edit.rule.start_day=occurrence;write_fail=true;
   CHECK(cal_runtime_submit(CAL_SAVE,&edit,1,occurrence,true,NULL,NULL));
   cal_event_clear(&edit);cal_runtime_ui_closed(NULL,0,false);break;
  }
  case 2:{
   CHECK(state->closed_pending&&state->closed_failed&&state->book.count==1);
   unsigned before=service_stops;CHECK(!process(o,false)&&service_stops==before);
   cal_event_t restored={0};int32_t day=0;bool instance=false;
   CHECK(cal_runtime_draft(&restored,&day,&instance));
   CHECK(day==occurrence&&instance&&restored.id==1&&!strcmp(restored.title,"仅本次修改"));
   write_fail=false;CHECK(cal_runtime_submit(CAL_SAVE,&restored,restored.id,day,instance,NULL,NULL));
   cal_event_clear(&restored);break;
  }
  case 3:{
   CHECK(!state->closed_pending&&state->book.count==2);
   cal_event_t *base=cal_find(&state->book,1),*single=cal_find(&state->book,2);
   CHECK(base&&!strcmp(base->title,"评审")&&base->rule.repeat==EPD_REPEAT_DAILY);
   CHECK(base->rule.excluded_count==1&&base->rule.excluded[0]==occurrence);
   CHECK(single&&single->parent==1&&single->original_day==occurrence&&single->rule.repeat==EPD_REPEAT_NONE);
   CHECK(!strcmp(single->title,"仅本次修改"));CHECK(!process(o,false)&&stop);break;
  }
  default:CHECK(false);
  }
  return 0;
 }
 switch(step++){
 case 0:CHECK(state->status.ready);CHECK(cal_runtime_submit(CAL_SAVE,&e,0,0,false,NULL,NULL));CHECK(!cal_runtime_submit(CAL_SAVE,&e,0,0,false,NULL,NULL));break;
 case 1:CHECK(state->book.count==1&&registered_count==1);strcpy(e.title,"未保存的修改");CHECK(cal_runtime_submit(CAL_DRAFT,&e,0,0,false,NULL,NULL));break;
 case 2:CHECK(state->status.has_draft&&state->draft.id==0);write_fail=true;strcpy(e.title,"写入失败");CHECK(cal_runtime_submit(CAL_SAVE,&e,0,0,false,NULL,NULL));break;
 case 3:CHECK(*state->status.error);CHECK(state->closed_pending&&state->closed_failed&&!strcmp(state->closed_draft.title,"写入失败"));CHECK(state->book.count==1&&state->status.has_draft&&!strcmp(state->draft.title,"未保存的修改"));write_fail=false;
   CHECK(cal_export_file(&state->book,"/flash/example.calbak"));CHECK(cal_runtime_submit(CAL_PREVIEW,NULL,0,0,false,"/flash/example.calbak",NULL));break;
 case 4:CHECK(state->status.preview&&state->status.import_existing==1&&state->status.import_new==0);CHECK(cal_runtime_submit(CAL_IMPORT,NULL,0,0,true,NULL,NULL));break;
 case 5:CHECK(state->book.count==1&&!state->status.preview);cal_runtime_query(epd_date_day((epd_date_t){2026,10,30}),epd_date_day((epd_date_t){2026,10,30}),480,NULL,-1,0,CAL_QUERY_ROWS|CAL_QUERY_GRID);break;
 case 6:CHECK(state->status.count==1);CHECK(state->status.busy_days);stop=true;break;
 default:CHECK(false);
 }return 0;}
static void query_tests(void)
{
 stop=false;cal_runtime_init();state->owner=(void*)1;state->status.ready=true;
 cal_event_t e=fixture();CHECK(cal_put(&state->book,&e));int32_t day=e.rule.start_day;
 e.id=0;strcpy(e.title,"生日");e.kind=CAL_BIRTHDAY;e.rule.start_day=e.rule.until_day=day+2;CHECK(cal_put(&state->book,&e));
 row_queries=grid_queries=search_queries=wakes=0;
 unsigned fields=CAL_QUERY_ROWS|CAL_QUERY_GRID;
 uint32_t rev=cal_runtime_query(day,day,480,NULL,-1,0,fields);
 CHECK(rev&&state->refresh&&wakes==1);
 CHECK(cal_runtime_query(day,day,480,"",-1,0,fields)==rev&&wakes==1);
 query_rows();CHECK(row_queries==1&&grid_queries==1&&search_queries==0);
 CHECK(state->status.count==1&&state->status.query_revision==rev&&!state->refresh);
 uint32_t data=state->status.data_revision;
 CHECK(cal_runtime_query(day,day,480,NULL,-1,0,fields)==rev&&!state->refresh&&wakes==1);
 CHECK(state->status.data_revision==data);
 CHECK(cal_runtime_query(day,day,480,NULL,-1,0,0)==0&&!state->refresh&&wakes==1);
 cal_runtime_query(day,day,480,NULL,-1,0,CAL_QUERY_ROWS|CAL_QUERY_UPCOMING);query_rows();
 CHECK(row_queries==2&&grid_queries==1&&search_queries==1&&state->status.upcoming_count==1);
 cal_runtime_query(day-3,day+3,480,NULL,-1,0,fields);query_rows();
 CHECK(row_queries==3&&grid_queries==1&&search_queries==1&&state->status.upcoming_count==0);
 cal_runtime_query(day,day+100,480,"生日",-1,0,CAL_QUERY_ROWS|CAL_QUERY_SEARCH);query_rows();
 CHECK(row_queries==3&&grid_queries==1&&search_queries==2&&state->status.count==1);
 data=state->status.data_revision;supersede=true;
 cal_runtime_query(day,day,480,NULL,-1,0,CAL_QUERY_ROWS);query_rows();
 CHECK(state->refresh&&state->status.data_revision==data);
 query_rows();CHECK(!state->refresh&&state->status.count==0&&state->status.data_revision==data+1);
 CHECK(state->status.query_revision==state->query_revision);
 unsigned before=wakes;block_wake=true;
 rev=cal_runtime_query(day+3,day+3,480,NULL,-1,0,CAL_QUERY_ROWS);CHECK(state->refresh&&wakes==before);
 block_wake=false;CHECK(cal_runtime_query(day+3,day+3,480,NULL,-1,0,CAL_QUERY_ROWS)==rev&&wakes==before+1);
 fail_query_alloc=true;unsigned requests_before=memory_requests;query_rows();CHECK(!state->refresh&&!state->query_running&&state->status.query_revision==rev);
 CHECK(memory_requests==requests_before+1);
 rev=cal_runtime_query(day+4,day+4,480,NULL,-1,0,CAL_QUERY_ROWS);
 persistent_query_oom=true;requests_before=memory_requests;query_rows();persistent_query_oom=false;
 CHECK(memory_requests==requests_before+1&&!state->query_running&&state->status.query_revision!=rev);
 CHECK(cal_runtime_query(day+3,day+3,480,NULL,-1,0,CAL_QUERY_ROWS)!=rev&&state->refresh);query_rows();
 CHECK(!state->refresh&&state->status.query_revision==state->query_revision);
 rev=state->query_revision;++state->book_revision;
 CHECK(cal_runtime_query(day+3,day+3,480,NULL,-1,0,CAL_QUERY_ROWS)!=rev&&state->refresh);query_rows();
 cal_runtime_query(day,day,480,NULL,-1,0,CAL_QUERY_ROWS|CAL_QUERY_UPCOMING);query_rows();
 CHECK(state->status.upcoming_count==1);data=state->status.data_revision;
 rev=cal_runtime_query(day+1,day+1,480,NULL,-1,0,CAL_QUERY_ROWS|CAL_QUERY_UPCOMING);
 fail_query_after=2;requests_before=memory_requests;query_rows();fail_query_after=SIZE_MAX;
 CHECK(memory_requests==requests_before+1&&state->status.data_revision==data&&state->status.query_revision!=rev);
 CHECK(state->status.upcoming_count==1&&*state->status.error&&!state->query_running);
 fail_query_alloc=true;memory_delay_hook=replace_waiting_query;
 query_rows();memory_delay_hook=NULL;
 CHECK(state->refresh&&state->status.data_revision==data&&!state->query_running);
 query_rows();CHECK(!state->refresh&&state->status.query_revision==state->query_revision);
 epd_service_sleep_release(state->owner);cal_runtime_cleanup();
 puts("calendar queries: duplicate wakeups suppressed, view-specific work, cached month mask and stale-result rejection passed");
}
static void diagnostic_tests(void)
{
 scenario=3;read_fail=false;
 const epd_reminder_result_t results[]={EPD_REMINDER_EMPTY,EPD_REMINDER_INVALID,EPD_REMINDER_IO,EPD_REMINDER_MEMORY,EPD_REMINDER_UNAVAILABLE};
 const char *names[]={"empty(1)","invalid(2)","io(3)","memory(4)","unavailable(5)"};
 for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);++i){
  stop=false;waits=0;logs[0]=0;load_reply=results[i];expect_ready=load_reply==EPD_REMINDER_EMPTY;
  cal_runtime_init();work((void*)1);CHECK(!held);
  CHECK(strstr(logs,"load begin flash=1 reminder=1")&&strstr(logs,names[i])&&strstr(logs,"bytes=0")&&strstr(logs,"decode=skipped"));
  CHECK(!cal_runtime_start());stop=false;CHECK(cal_runtime_start());
  cal_runtime_cleanup();
 }
 load_reply=EPD_REMINDER_OK;stop=false;waits=0;logs[0]=0;expect_ready=false;
 durable=calloc(1,4);durable_size=4;cal_runtime_init();work((void*)1);
 CHECK(strstr(logs,"load result=ok(0) bytes=4")&&strstr(logs,"decode=failed")&&strstr(logs,"ready=0"));
 cal_runtime_cleanup();free(durable);durable=NULL;durable_size=0;
 stop=false;logs[0]=0;block_wake=true;cal_runtime_init();work((void*)1);
 CHECK(strstr(logs,"stage=sleep-guard cancelled=0")&&!strstr(logs,"load begin")&&!held);
 block_wake=false;cal_runtime_cleanup();logs[0]=0;
 CHECK(!cal_runtime_start()&&strstr(logs,"start failed stage=runtime"));
 puts("calendar load diagnostics: empty, read failure, unavailable service, invalid document and sleep admission distinguished");
}
static void lifecycle_tests(void)
{
 stop=false;service_registered=true;ui_present=false;cal_runtime_init();state->owner=(void*)1;
 CHECK(!process((void*)1,false)&&!stop);state->load_finished=true;state->status.busy=false;
 bool *pending[]={&state->status.busy,&state->pending,&state->refresh,&state->query_running,&state->status.preview,&state->closed_pending};
 for(unsigned i=0;i<sizeof(pending)/sizeof(pending[0]);++i){*pending[i]=true;CHECK(!process((void*)1,false)&&!stop);*pending[i]=false;}
 ui_present=true;CHECK(!process((void*)1,false)&&!stop);ui_present=false;
 state->status.ready=true;state->closed_pending=true;block_wake=true;stage_closed_draft();
 CHECK(state->closed_pending&&!state->pending);unsigned before_wake=wakes;
 CHECK(!process((void*)1,false)&&wakes==before_wake+1&&!stop);
 state->closed_pending=false;state->status.ready=false;block_wake=false;
 /* A terminal load error is reclaimable even though ready was never set. */
 CHECK(!state->status.ready&&!process((void*)1,false)&&stop&&state->idle_stopping);
 ui_present=true;CHECK(!cal_runtime_start());unsigned retired=ui_retires;
 CHECK(process((void*)1,true)&&state->retired&&ui_retires==retired);
 CHECK(!cal_runtime_start());service_registered=false;stop=false;
 CHECK(cal_runtime_start()&&service_registered&&!state->retired&&!state->idle_stopping&&!state->load_finished);
 unsigned starts=service_starts;CHECK(cal_runtime_start()&&service_starts==starts&&!state->owner);
 cal_runtime_cleanup();ui_present=false;
 for(scenario=4;scenario<=5;++scenario){stop=false;step=waits=0;write_fail=read_fail=false;cal_runtime_init();work((void*)1);CHECK(!held&&state->idle_stopping);cal_runtime_cleanup();}
 free(durable);durable=NULL;durable_size=0;scenario=6;stop=false;step=waits=0;write_fail=false;
 cal_runtime_init();work((void*)1);CHECK(!held&&state->idle_stopping);cal_runtime_cleanup();
 puts("calendar failed occurrence save: close, restore, retry preserves series and occurrence scope");
 free(durable);durable=NULL;durable_size=0;scenario=7;stop=false;step=waits=0;write_fail=false;
 cal_runtime_init();work((void*)1);CHECK(!held&&state->idle_stopping);cal_runtime_cleanup();
 ordinary_admit=true;verify_cleanup_io=false;
 puts("calendar RELEASE cleanup: denied ordinary work, context failure wake, failed write retention and persisted retry passed");
 puts("calendar lifecycle: unpublished owner, failed-load release, pending-work protection, immediate reopen, owned drafts, late close and failed-save recovery passed");
}
int main(void){host_reset();cal_runtime_init();cal_runtime_query(1,2,480,NULL,-1,0,CAL_QUERY_ROWS|CAL_QUERY_GRID);work((void*)1);CHECK(!held);cal_runtime_cleanup();
 scenario=2;stop=false;cal_runtime_init();work((void*)1);cal_runtime_cleanup();
 scenario=1;stop=false;read_fail=true;cal_runtime_init();cal_runtime_query(1,2,480,NULL,-1,0,CAL_QUERY_ROWS);work((void*)1);CHECK(!held);cal_runtime_cleanup();free(durable);durable=NULL;durable_size=0;host_reset();query_tests();diagnostic_tests();
 lifecycle_tests();free(durable);durable=NULL;durable_size=0;
 puts("calendar runtime: worker admission, atomic saves/drafts, import preview/skip, cached query, restart and failed-load idle passed");return 0;}
