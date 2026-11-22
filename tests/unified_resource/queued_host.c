#include "queued_support.inc"
#include "dfs_credit.inc"
static epd_resource_cache_t view_resource;
#include "reader_peak.inc"
#include "io_pressure.inc"
#include "record_activity.inc"
#include "near_activity.inc"
static void (*component_wake_hook)(void);
static void component_wake(void *context)
{
    CHECK(!context && !mutexes && !irq && actor==&actors[1]); ++component_wakes;
    if (component_wake_hook) component_wake_hook();
}
static epd_activity_work_t *queued(epd_resource_kind_t kind, epd_resource_activity_t parent, size_t psram, size_t sram)
{ return epd_activity_work_queue(kind,parent,psram,sram,component_wake,NULL); }
#include "retire_activity.inc"
#include "prepare_activity.inc"
#include "document_reject.inc"
int main(int argc,char **argv)
{
    CHECK(argc==2); setup();
    actor=&actors[0]; CHECK(rt_thread_mem_context_set(NULL,app)==0);
    if (!strncmp(argv[1],"document_",9)) {
        document_reject_case(argv[1]);
    } else if (!strncmp(argv[1],"prepare_",8)) {
        prepare_activity_case(argv[1]);
    } else if (!strncmp(argv[1],"retire_",7)) {
        retire_activity_case(argv[1]);
    } else if (!strcmp(argv[1],"ui")) {
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512);
        CHECK(w && task_count==1 && epd_activity_work_ready(w)==0 && !blocked);
        drain(); CHECK(component_wakes && epd_activity_work_ready(w)==1 && !blocked);
        epd_activity_guard_t g; CHECK(epd_activity_work_enter(w,&g));
        void *p=epd_app_alloc(100,EPD_APP_PSRAM); CHECK(p);
        epd_memory_owner_t token=block_owner(p); epd_memory_owner_snapshot_t owner;
        CHECK(epd_memory_owner_get(token,&owner) && owner.account==app);
        epd_activity_work_leave(&g); epd_activity_work_close(w); CHECK(!task_count && !group_count);
        drain(); CHECK(!semaphores && physical[1]==132);
        epd_memory_account_snapshot_t usage; CHECK(epd_memory_scope_snapshot(token,&usage));
        CHECK(usage.backend[backends[1]].physical[EPD_MEMORY_ACTIVE]==132 && !usage.backend[backends[1]].unused);
        epd_app_free(p);
    } else if (!strcmp(argv[1],"nested")) {
        epd_memory_owner_t page=epd_memory_scope_create(app,EPD_MEMORY_SCOPE_PAGE);
        CHECK(epd_resource_runtime_page_state(page,1,true)==0);
        CHECK(rt_thread_mem_context_set(NULL,page)==0);
        epd_activity_work_t *outer=queued(EPD_RESOURCE_CONTINUOUS,0,8192,1024); CHECK(outer); drain();
        epd_activity_guard_t guard; CHECK(epd_activity_work_enter(outer,&guard));
        epd_activity_work_t *inner=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(inner);
        epd_activity_work_leave(&guard); drain(); CHECK(epd_activity_work_ready(inner)==1);
        epd_activity_work_close(outer); drain();
        CHECK(epd_activity_work_enter(inner,&guard));
        void *p=epd_app_alloc(128,EPD_APP_SRAM); CHECK(p); epd_app_free(p);
        epd_activity_work_leave(&guard);
        CHECK(epd_resource_runtime_close_owner(page)==EPD_RESOURCE_ACCEPTED);
        epd_memory_owner_close(page); drain(); CHECK(epd_activity_work_ready(inner)==-1);
        epd_activity_work_close(inner); drain();
        CHECK(rt_thread_mem_context_set(NULL,app)==0); epd_memory_owner_release(page);
    } else if (!strcmp(argv[1],"capacity")) {
        epd_memory_owner_t previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
        void *p=epd_app_alloc(CAPACITY*3/4,EPD_APP_PSRAM); CHECK(p); epd_memory_owner_leave(previous);
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,CAPACITY/2,0); CHECK(w); drain();
        CHECK(epd_activity_work_ready(w)==0 && !blocked); epd_app_free(p); drain();
        CHECK(epd_activity_work_ready(w)==1 && !blocked); epd_activity_work_close(w);
    } else if (!strcmp(argv[1],"sram")) {
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,4096,CAPACITY*2); CHECK(w); drain();
        CHECK(epd_activity_work_ready(w)==-1 && epd_activity_work_ready(w)==-1); epd_activity_work_close(w);
    } else if (!strcmp(argv[1],"joint")) {
        epd_memory_owner_t previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
        void *occupied=epd_app_alloc(CAPACITY*3/4,EPD_APP_SRAM); CHECK(occupied);
        epd_memory_owner_leave(previous);
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,4096,CAPACITY/2); CHECK(w); drain();
        CHECK(epd_activity_work_ready(w)==0 && !blocked);
        epd_memory_account_snapshot_t usage; CHECK(epd_memory_scope_snapshot(app,&usage));
        CHECK(!usage.backend[backends[0]].unused && !usage.backend[backends[1]].unused);
        epd_app_free(occupied); drain(); CHECK(epd_activity_work_ready(w)==1);
        epd_activity_guard_t guard; CHECK(epd_activity_work_enter(w,&guard));
        void *a=epd_app_alloc(CAPACITY/2-32,EPD_APP_SRAM), *b=epd_app_alloc(4096-32,EPD_APP_PSRAM);
        CHECK(a && b); epd_app_free(a); epd_app_free(b);
        epd_activity_work_leave(&guard); epd_activity_work_close(w);
    } else if (!strcmp(argv[1],"page")) {
        epd_memory_owner_t page=epd_memory_scope_create(app,EPD_MEMORY_SCOPE_PAGE);
        CHECK(epd_resource_runtime_page_state(page,1,true)==0); CHECK(rt_thread_mem_context_set(NULL,page)==0);
        epd_activity_work_t *display=queued(EPD_RESOURCE_INTERACTIVE,0,4096,4096); CHECK(display); drain();
        CHECK(epd_activity_work_ready(display)==1 && epd_activity_work_settle(display)); drain();
        CHECK(!task_count && epd_activity_work_ready(display)==-1);
        epd_resource_activity_snapshot_t state; CHECK(epd_resource_activity_get(epd_activity_work_activity(display),&state)==0);
        CHECK(!state.effective.minimum_bytes && !state.effective.target_bytes);
        epd_memory_account_snapshot_t usage; CHECK(epd_memory_scope_snapshot(page,&usage));
        CHECK(!usage.backend[backends[0]].unused && !usage.backend[backends[1]].unused);
        epd_activity_work_t *near=queued(EPD_RESOURCE_NEARBY,epd_activity_work_activity(display),1024,0);
        CHECK(near); drain(); CHECK(epd_activity_work_ready(near)==1 && task_count==1);
        CHECK(epd_resource_runtime_page_state(page,1,false)==0); drain();
        CHECK(epd_resource_activity_get(epd_activity_work_activity(near),&state)==0 && state.effective.kind==EPD_RESOURCE_OPTIONAL);
        epd_activity_work_close(near); epd_activity_work_close(display); drain();
        CHECK(epd_resource_runtime_close_owner(page)==EPD_RESOURCE_ACCEPTED); drain();
        CHECK(!epd_resource_runtime_owner_busy(page)); CHECK(rt_thread_mem_context_set(NULL,app)==0);
        epd_memory_owner_close(page); epd_memory_owner_release(page);
    } else if (!strcmp(argv[1],"proof")) {
        CHECK(!queued(EPD_RESOURCE_INTERACTIVE,0,4096,0));
        CHECK(!queued(EPD_RESOURCE_NEARBY,0,4096,0)); CHECK(!task_count && !group_count);
    } else if (!strcmp(argv[1],"references")) {
        epd_activity_work_t *w=queued(EPD_RESOURCE_OPTIONAL,0,4096,0); CHECK(w && epd_activity_work_retain(w)); drain();
        epd_activity_work_close(w); CHECK(task_count==1 && epd_activity_work_ready(w)==1);
        epd_activity_work_close(w); CHECK(!task_count && task_ends==1);
    } else if (!strcmp(argv[1],"policy")) {
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,4096,0); CHECK(w); drain();
        accepting=false; CHECK(epd_activity_work_ready(w)==-1); epd_activity_work_close(w);
    } else if (!strcmp(argv[1],"allocation")) {
        fail_alloc=1;
        epd_activity_work_t *w=queued(EPD_RESOURCE_CONTINUOUS,0,4096,0);
        CHECK(w && fail_alloc==1 && task_count==1); drain();
        CHECK(epd_activity_work_ready(w)==1);
        fail_alloc=0; epd_activity_work_close(w); drain();
        CHECK(!task_count && !group_count);
    } else if (!strcmp(argv[1],"near_epoch")) {
        near_activity_case();
    } else if (!strncmp(argv[1],"record_",7)) {
        record_activity_case(argv[1]);
    } else if (!strncmp(argv[1],"io_",3)) {
        io_pressure_case(argv[1]);
    } else if (!strncmp(argv[1],"reader_",7)) {
        reader_peak_case(argv[1]);
    } else if (!strncmp(argv[1],"dfs_",4)) {
        dfs_credit_case(argv[1]);
    } else CHECK(false);
    CHECK(!blocked); finish(); CHECK(!task_count && !group_count);
    printf("PASS queued %s (%u ticks/s)\n",argv[1],RT_TICK_PER_SECOND); return 0;
}
