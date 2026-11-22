#include <rtthread.h>
#include "app_memory_pressure.h"
struct sdk_item { struct sdk_item *next,*next_free; };
static struct { struct rt_semaphore lock; size_t available_size; struct sdk_item *free_list; } sdk_heap;
#include "core_support.inc"
#include "realloc.inc"
static app_memory_pressure_status_t sdk_pressure;
uint32_t app_memory_pressure_request(size_t bytes)
{ sdk_pressure.min_available=bytes;sdk_pressure.pending=true;return ++sdk_pressure.generation; }
uint32_t app_memory_pressure_allocation_failed_block(size_t minimum,size_t physical)
{ (void)physical;return app_memory_pressure_request(minimum); }
app_memory_pressure_status_t app_memory_pressure_query(void) {return sdk_pressure;}
#include "recovery_sdk.inc"
static epd_memory_owner_t page_owner;
static size_t baseline[2];
static void *pressure;
static unsigned pressure_region=EPD_APP_PSRAM;

void retry_host_init(bool sram)
{
    setup();baseline[0]=physical[0];baseline[1]=physical[1];
    sdk_heap.available_size=CAPACITY-physical[1];
    pressure_region=sram?EPD_APP_SRAM:EPD_APP_PSRAM;
    page_owner=epd_memory_scope_create_trusted(app,EPD_MEMORY_SCOPE_PAGE);
    CHECK(page_owner!=EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_resource_runtime_page_state(page_owner,17,true)==EPD_RESOURCE_OK);
}
epd_activity_work_t *retry_host_input(void (*wake)(void *))
{
    actor=&actors[0];CHECK(rt_thread_mem_context_set(NULL,page_owner)==0);
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE,0,65536,4096,wake,NULL);
    CHECK(work);drain();
    actor=&actors[2];CHECK(rt_thread_mem_context_set(NULL,app)==0);return work;
}
void retry_host_poll(unsigned ms)
{ ticks+=rt_tick_from_millisecond((int)ms);drain(); }
void retry_host_recovery(epd_memory_recovery_t *r)
{ CHECK(epd_memory_recovery_begin(r,backends[1],1024,false)==EPD_MEMORY_RECOVERY_READY); }
bool retry_host_waiting(epd_activity_work_t *w)
{ epd_resource_activity_snapshot_t s;CHECK(epd_resource_activity_get(epd_activity_work_activity(w),&s)==EPD_RESOURCE_OK);return s.state==EPD_RESOURCE_WAITING; }
unsigned retry_host_tasks(void) { return task_count; }
void retry_host_pressure(void)
{
    CHECK(!pressure);
    epd_memory_owner_t before=epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    size_t remaining=CAPACITY-physical[pressure_region];
    epd_memory_backend_snapshot_t heap;
    CHECK(epd_memory_backend_get(backends[pressure_region],&heap));
    size_t leave=heap.usage.unused+heap.usage.allocating+512u;
    pressure=epd_app_alloc(remaining-leave-32,(epd_app_memory_t)pressure_region);CHECK(pressure);
    epd_memory_owner_leave_trusted(before);
}
void retry_host_release(void) { epd_app_free(pressure);pressure=NULL;drain(); }
void retry_host_invalidate(unsigned kind)
{
    if(kind==1) {
        CHECK(epd_resource_runtime_page_state(page_owner,18,true)==EPD_RESOURCE_STALE);
        CHECK(epd_resource_runtime_page_state(page_owner,17,false)==EPD_RESOURCE_OK);
        epd_memory_owner_t next=epd_memory_scope_create_trusted(app,EPD_MEMORY_SCOPE_PAGE);
        CHECK(next!=EPD_MEMORY_OWNER_INVALID);
        CHECK(epd_resource_runtime_page_state(next,18,true)==EPD_RESOURCE_OK);
    }
    else if(kind==2) CHECK(epd_memory_owner_close_trusted(app));
    else CHECK(epd_resource_runtime_page_state(page_owner,17,false)==EPD_RESOURCE_OK);
    drain();
}
epd_memory_owner_t retry_host_owner(void *p) { return block_owner(p); }
bool retry_host_ended(epd_memory_owner_t owner)
{
    epd_memory_credit_snapshot_t credit;
    if(!epd_memory_credit_get(owner,&credit))return true;
    return credit.state==EPD_MEMORY_CREDIT_ENDED && !credit.unused[backends[0]] && !credit.unused[backends[1]];
}
void retry_host_finish(void)
{
    if(pressure)retry_host_release();drain();
    CHECK(!semaphores && !task_count && !group_count && !blocked && !irq && !mutexes);
    CHECK(physical[0]==baseline[0] && physical[1]==baseline[1]);
}
