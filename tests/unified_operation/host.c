/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_operation.h"
#include "epd_cold_state.h"
#include "epd_cold_budget.h"
#include "epd_cold_runtime.h"
#include "services/app_background.h"
#include "epd_operation_runtime.h"
#include "epd_resource_runtime.h"
#include "operation_example.h"
#include "hello_state.h"
#include "epd_memory.h"
#include "ui_settings_store.h"
#include <sleep_guard.h>
#include <rthw.h>
#include <storage_file.h>
#include <dfs_posix.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
int test_init_operation_metadata_init(void);
int test_init_cold_metadata_init(void);
int test_init_cold_budget_init(void);
int test_init_operation_runtime_metadata_init(void);
int test_init_checkpoint_metadata_init(void);
int test_init_background_metadata_init(void);
int test_init_events_init(void);
static struct rt_thread threads[4] = {{1,0,0},{2,0,0},{3,0,0},{4,0,0}}, runtime_thread;
static rt_thread_t current = &threads[0];
static unsigned irq_depth, critical_depth, refs[16], allocations, runs, receives, disposed, users;
static void *budget_control_memory;
static bool budget_control_reenter, budget_mutex_fail;
static int budget_control_reentered;
static unsigned io_calls, commits;
static unsigned index_commits;
static bool fail_index;
static const char *blocked_file;
static bool budget_sd;
static uint32_t budget_session=1;
static unsigned power_wakes, event_waits;
static bool stop_at_idle;
static int32_t observed_wait;
static jmp_buf idle_jump;
static struct rt_object_global_scope *global_scope;
static bool fail_global_scope;
static uint32_t ticks;
static epd_memory_owner_t context;
static bool owner_closed[16], fail_alloc, fail_enter, fail_write, unavailable;
static bool change_during_write;
static unsigned sleep_holds;
static epd_memory_owner_t parents[16];
static epd_memory_scope_kind_t kinds[16];
static const char *appids[16];
static bool task_running[16], task_yield_once;
static unsigned task_starts, task_ends;
static int task_hook_error;
static bool accept_result = true, sync_complete, reenter, close_in_receive;
static void *accepted_result;
static epd_operation_status_t received_status;
static uint32_t received_generation;
static epd_operation_scope_t business, cleanup;
static struct rt_event exec_event, ui_event, cleanup_event;

rt_base_t rt_hw_interrupt_disable(void) { return (rt_base_t)irq_depth++; }
void rt_hw_interrupt_enable(rt_base_t previous) { CHECK(irq_depth == (unsigned)previous + 1u); irq_depth = (unsigned)previous; }
void rt_enter_critical(void) { ++critical_depth; }
void rt_exit_critical(void) { CHECK(critical_depth); --critical_depth; }
rt_thread_t rt_thread_self(void) { return current; }
rt_uint8_t rt_interrupt_get_nest(void) { return 0; }
rt_uint16_t rt_critical_level(void) { CHECK(critical_depth <= UINT16_MAX); return (rt_uint16_t)critical_depth; }
uint32_t rt_tick_get(void) { return ticks; }
void rt_thread_yield(void) { CHECK(!irq_depth); }
int rt_thread_startup(rt_thread_t thread) { CHECK(thread); return 0; }
int rt_thread_delete(rt_thread_t thread) { CHECK(thread); return 0; }
int rt_event_init(struct rt_event *event, const char *name, int flags)
{ (void)name; (void)flags; CHECK(!irq_depth && global_scope && !context); memset(event,0,sizeof(*event)); return 0; }
int rt_event_detach(struct rt_event *event) { (void)event; return 0; }
int rt_event_send(struct rt_event *event, uint32_t bits)
{ CHECK(!irq_depth); event->bits |= bits; ++event->sends; return 0; }
int rt_event_recv(struct rt_event *event, uint32_t bits, int flags, int32_t wait, uint32_t *out)
{
    CHECK(!irq_depth && !context && flags==(RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR));
    CHECK(++event_waits<100);
    *out = event->bits & bits; event->bits &= ~bits;
    if(stop_at_idle && !*out) {observed_wait=wait;longjmp(idle_jump,1);}
    return 0;
}
int rt_mutex_init(struct rt_mutex *mutex, const char *name, int flags)
{ (void)flags;CHECK(global_scope && !context);mutex->held=0;return budget_mutex_fail && !strcmp(name,"coldbud") ? -1 : 0; }
int rt_mutex_detach(struct rt_mutex *mutex) { CHECK(!mutex->held);return 0; }
int rt_mutex_take(struct rt_mutex *mutex, int wait) { (void)wait;CHECK(!mutex->held);++mutex->held;return 0; }
int rt_mutex_release(struct rt_mutex *mutex) { CHECK(mutex->held);--mutex->held;return 0; }
int rt_sem_init(struct rt_semaphore *sem, const char *name, unsigned count, int flags)
{ (void)name;(void)flags;CHECK(global_scope && !context);sem->count=count;return 0; }
int rt_sem_detach(struct rt_semaphore *sem) { (void)sem;return 0; }
int rt_sem_release(struct rt_semaphore *sem) { ++sem->count;return 0; }
int rt_sem_take(struct rt_semaphore *sem, int wait)
{
    (void)wait;
    if(!sem->count) {rt_thread_t saved=current;current=&runtime_thread;CHECK(epd_operation_runtime_worker_pump(32,1)>0);current=saved;}
    CHECK(sem->count);--sem->count;return 0;
}
bool sleep_guard_acquire(sleep_guard_owner_t *owner)
{ if(!owner->held) {owner->held=true;++sleep_holds;}return true; }
void sleep_guard_release(sleep_guard_owner_t *owner)
{ if(owner->held) {owner->held=false;CHECK(sleep_holds);--sleep_holds;} }
bool system_reset_write_allowed(void) { return true; }
void epd_power_notify(void) {CHECK(!irq_depth);++power_wakes;}
unsigned ui_settings_default(ui_setting_id_t id) { (void)id;return 0; }
#ifdef _WIN32
int test_open(const char *path,int flags) {return _open(path,flags|_O_BINARY);}
int test_read(int fd,void *data,size_t size) {CHECK(size<=UINT_MAX);return _read(fd,data,(unsigned)size);}
#endif
rt_thread_t epd_thread_create_psram(const char *name, void (*entry)(void *), void *arg,
                                  uint32_t stack, uint8_t priority, uint32_t quantum)
{
    (void)name; (void)priority; (void)quantum;
    CHECK(!irq_depth && stack >= 8192 && global_scope && !context);
    runtime_thread.id=9; runtime_thread.entry=entry; runtime_thread.parameter=arg;
    return &runtime_thread;
}
int rt_object_global_scope_enter(struct rt_object_global_scope *scope)
{
    CHECK(!irq_depth && !scope->thread);if(fail_global_scope) return -1;
    scope->thread=current;scope->previous=global_scope;global_scope=scope;return 0;
}
int rt_object_global_scope_leave(struct rt_object_global_scope *scope)
{
    CHECK(!irq_depth && global_scope==scope && scope->thread==current);
    global_scope=scope->previous;scope->thread=NULL;return 0;
}
epd_memory_owner_t epd_memory_owner_current(void) { return context; }
bool epd_memory_owner_is_current_account(epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t target, current_owner;
    return epd_memory_owner_get(owner,&target) && epd_memory_owner_get(context,&current_owner) &&
        target.account==current_owner.account;
}
epd_memory_owner_t epd_memory_scope_create_trusted(epd_memory_owner_t parent,epd_memory_scope_kind_t kind)
{ return epd_memory_scope_create(parent,kind); }
static bool enforce_retain_account;
bool epd_memory_owner_retain_trusted(epd_memory_owner_t owner)
{ if (owner >= 16) return false; ++refs[owner]; return true; }
void epd_memory_owner_release_trusted(epd_memory_owner_t owner) { epd_memory_owner_release(owner); }
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner) { return epd_memory_owner_enter(owner); }
void epd_memory_owner_leave_trusted(epd_memory_owner_t owner) { epd_memory_owner_leave(owner); }
bool epd_memory_owner_close_trusted(epd_memory_owner_t owner) { return epd_memory_owner_close(owner); }
bool epd_memory_owner_get(epd_memory_owner_t owner, epd_memory_owner_snapshot_t *out)
{
    if(owner>=16) return false;
    memset(out,0,sizeof(*out));out->owner=owner;out->parent=parents[owner];out->closed=owner_closed[owner];out->kind=kinds[owner];
    epd_memory_owner_t root=owner;
    while(parents[root]) root=parents[root];
    out->account=root;
    if(appids[root]) snprintf(out->appid,sizeof(out->appid),"%s",appids[root]);
    return true;
}
epd_memory_owner_t epd_memory_scope_create(epd_memory_owner_t parent,epd_memory_scope_kind_t kind)
{
    if(parent>=16 || owner_closed[parent]) return EPD_MEMORY_OWNER_INVALID;
    for(unsigned i=8;i<16;++i) if(!refs[i]) {
        parents[i]=parent;kinds[i]=kind;owner_closed[i]=false;refs[i]=1;return i;
    }
    return EPD_MEMORY_OWNER_INVALID;
}
void app_service_background_changed(epd_memory_owner_t owner)
{ CHECK(!irq_depth && owner != EPD_MEMORY_OWNER_INVALID); epd_power_notify(); }
void app_service_ui_signal(uint32_t events) { (void)events; CHECK(!irq_depth); }
epd_memory_result_t app_service_quiesce(const char *id)
{ CHECK(!irq_depth && current==&threads[0] && id && *id);return EPD_MEMORY_OK; }
int epd_resource_runtime_close_owner(epd_memory_owner_t owner) { (void)owner;return EPD_RESOURCE_OK; }
int epd_resource_runtime_task_state(epd_memory_owner_t owner,uint64_t generation,bool running)
{
    CHECK(!irq_depth && !context && current==&runtime_thread && owner<16 && generation==owner);
    CHECK(kinds[owner]==EPD_MEMORY_SCOPE_TASK || kinds[owner]==EPD_MEMORY_SCOPE_SERVICE);
    if(task_hook_error) return task_hook_error;
    if(running) {CHECK(!task_running[owner]);++task_starts;} else {CHECK(task_running[owner]);++task_ends;}
    task_running[owner]=running;return EPD_RESOURCE_OK;
}
bool epd_memory_owner_close(epd_memory_owner_t owner)
{
    if(owner>=16 || !owner) return false;
    for(unsigned i=1;i<16;++i) {
        epd_memory_owner_t at=i;
        for(unsigned n=0;n<16 && at;++n) {if(at==owner) {owner_closed[i]=true;break;} at=parents[at];}
    }
    return true;
}
bool epd_memory_owner_retain(epd_memory_owner_t owner)
{
    return (!enforce_retain_account || epd_memory_owner_is_current_account(owner)) &&
           epd_memory_owner_retain_trusted(owner);
}
void epd_memory_owner_release(epd_memory_owner_t owner)
{ CHECK(owner < 16 && refs[owner]); --refs[owner]; }
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{
    CHECK(!irq_depth);
    if (fail_enter) return EPD_MEMORY_OWNER_INVALID;
    CHECK(owner < 16 && (refs[owner] || owner == 0));
    epd_memory_owner_t previous=context; context=owner; return previous;
}
void epd_memory_owner_leave(epd_memory_owner_t previous)
{ CHECK(!irq_depth && previous != EPD_MEMORY_OWNER_INVALID); context=previous; }
void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    CHECK(!irq_depth && region == EPD_APP_PSRAM); if (fail_alloc) return NULL;
    void *p=malloc(size); CHECK(p);
    if (global_scope && !context) {
        CHECK(!budget_control_memory && size > 7000 && size < 8192); budget_control_memory=p;
        if(budget_control_reenter) {
            epd_cold_lease_t lease=1;budget_control_reenter=false;
            budget_control_reentered=epd_cold_budget_begin("./racing",EPD_COLD_REQUIRED,1,0,&lease);
            CHECK(!lease);
        }
    } else ++allocations;
    return p;
}
void epd_app_free(void *data)
{
    CHECK(!irq_depth);
    if(data == budget_control_memory) { budget_control_memory=NULL; free(data); }
    else if(data) { CHECK(allocations); --allocations; free(data); }
}

void storage_lock(void) { CHECK(!irq_depth); }
void storage_unlock(void) { CHECK(!irq_depth); }
const char *storage_path_root(const char *path) { return !strncmp(path,"./sd/",5) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
bool storage_available(storage_volume_t volume) { return !unavailable && (volume==STORAGE_FLASH || budget_sd); }
bool storage_session_valid(storage_volume_t volume,uint32_t session)
{ return storage_available(volume) && (volume==STORAGE_FLASH ? !session : session==budget_session); }
uint32_t storage_card_session(void) { return budget_session; }
bool storage_info(storage_volume_t volume,storage_info_t *out)
{ *out=(storage_info_t){true,64u*1024u*1024u,0};return storage_available(volume); }
bool storage_path_available(const char *path) { (void)path; CHECK(!irq_depth); return !unavailable; }
bool storage_file_recover(const char *path) { (void)path; CHECK(!irq_depth); return !unavailable; }
uint32_t storage_crc32(const void *data, size_t size)
{
    CHECK(!irq_depth);
    const unsigned char *p=data; uint32_t crc=UINT32_MAX;
    while (size--) { crc^=*p++; for (unsigned b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); }
    return ~crc;
}
/* Filesystem port: the tested manager chooses revisions, slots, retry ownership
 * and envelope. This backend implements only the existing storage_file contract. */
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    CHECK(!irq_depth && current != &threads[0]); ++io_calls;
    if (unavailable) return false;
    FILE *file=fopen(path,"rb"); if (!file) return false;
    uint32_t header[4];
    bool ok=fread(header,1,sizeof(header),file)==sizeof(header) && header[0]==magic &&
        header[1]==version && header[2]==size && fread(data,1,size,file)==size &&
        fgetc(file)==EOF && header[3]==storage_crc32(data,size);
    if (fclose(file)) ok=false;
    return ok;
}
bool storage_file_replace_session(const char *path, const void *data, size_t size, uint32_t session)
{
    CHECK(!irq_depth && current != &threads[0] && session == 0); ++io_calls;
    bool index = strstr(path,"cold-budget.bin") != NULL;
    if (unavailable || fail_write || (index && fail_index)) return false;
    char temp[512]; snprintf(temp,sizeof(temp),"%s.tmp",path);
    FILE *file=fopen(temp,"wb"); CHECK(file);
    bool ok=fwrite(data,1,size,file)==size;
    if (fclose(file)) ok=false;
    if (ok) { (void)remove(path); ok=rename(temp,path)==0; }
    if (ok) {if(index) ++index_commits;else ++commits;}
    return ok;
}
bool storage_file_replace(const char *path,const void *data,size_t size)
{
    CHECK(sleep_holds==1 && current==&runtime_thread);
    if(change_during_write) {
        change_during_write=false;
        uint8_t values[UI_SETTING_COUNT]={0};values[UI_SETTING_TIMEOUT]=3;
        rt_thread_t saved=current;current=&threads[0];CHECK(ui_settings_store_save(values));current=saved;
    }
    return storage_file_replace_session(path,data,size,0);
}
uint32_t storage_path_revision(const char *path) {(void)path;return commits+1;}
uint32_t storage_volume_revision(storage_volume_t volume) {(void)volume;return commits+1;}
uint32_t storage_file_writers(const char *path) {return blocked_file && !strcmp(blocked_file,path) ? 1u : 0u;}

static void run(epd_operation_t id, void *user)
{
    CHECK(!irq_depth && current == &threads[1] && context == 1); CHECK(user == &runs); ++runs;
    if (sync_complete) {
        void *p=epd_app_alloc(8,EPD_APP_PSRAM);
        CHECK(epd_operation_complete(id,EPD_OPERATION_OK,0,p));
        if (reenter) {
            current=&threads[2]; (void)epd_operation_dispatch(10); current=&threads[1];
            CHECK(!disposed && !users);
        }
    }
}
static bool receive(epd_operation_t id, const epd_operation_snapshot_t *state, void *result, void *user)
{
    (void)id; CHECK(!irq_depth && current == &threads[0] && context == 1 && user == &runs);
    ++receives; received_status=state->status; received_generation=state->instance_generation;
    if (close_in_receive) CHECK(epd_operation_scope_close(business));
    if (accept_result) accepted_result=result;
    return accept_result;
}
static void dispose(void *result, void *user)
{ CHECK(!irq_depth && current == &threads[2] && context == 2 && user == &runs); ++disposed; epd_app_free(result); }
static void dispose_user(void *user)
{ CHECK(!irq_depth && current == &threads[2] && context == 2 && user == &runs); ++users; }
/* Simulate the app callback that submits these synthetic fixture scopes. */
static bool submit_as_owner(const epd_operation_spec_t *spec, epd_operation_t *out)
{
    epd_operation_scope_snapshot_t scope;
    if (!epd_operation_scope_get(spec->scope, &scope)) return false;
    epd_memory_owner_t previous = context;
    context = scope.owner;
    bool ok = epd_operation_submit(spec, out);
    context = previous;
    return ok;
}
static epd_operation_spec_t setup(void)
{
    if (!parents[1] && !parents[2]) parents[1] = parents[2] = 15;
    CHECK(epd_operation_scope_open(1,77,&business));
    CHECK(epd_operation_scope_open(2,77,&cleanup));
    epd_operation_spec_t spec={0};
    spec.scope=business; spec.cleanup_scope=cleanup;
    spec.execute=(epd_operation_executor_t){&threads[1],&exec_event,1};
    spec.deliver=(epd_operation_executor_t){&threads[0],&ui_event,1};
    spec.cleanup=(epd_operation_executor_t){&threads[2],&cleanup_event,1};
    spec.run=run; spec.receive=receive; spec.dispose_result=dispose; spec.dispose_user=dispose_user; spec.user=&runs;
    return spec;
}
static size_t pump(unsigned thread)
{ current=&threads[thread]; return epd_operation_dispatch(128); }
static void drain(void) { for(unsigned i=0;i<3;++i) { pump(1);pump(2);pump(0);pump(2); } }
static void finish_scopes(void)
{
    CHECK(epd_operation_scope_close(business)); CHECK(epd_operation_scope_close(cleanup));
    CHECK(epd_operation_scope_release(business)); CHECK(epd_operation_scope_release(cleanup));
    CHECK(!refs[1] && !refs[2]);
}
static void cancel_queued(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id; CHECK(submit_as_owner(&spec,&id));
    CHECK(epd_operation_cancel(id)); CHECK(!runs && !receives); drain();
    CHECK(!runs && receives==1 && users==1 && received_status==EPD_OPERATION_CANCELLED);
    CHECK(epd_operation_release(id)); CHECK(!epd_operation_release(id)); finish_scopes();
}
static void cancel_running(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id; CHECK(submit_as_owner(&spec,&id)); pump(1);
    CHECK(runs==1 && epd_operation_cancel(id)); drain(); CHECK(!users && !receives);
    CHECK(epd_operation_scope_close(business)); CHECK(!epd_operation_scope_release(business));
    void *p=epd_app_alloc(8,EPD_APP_PSRAM);
    CHECK(epd_operation_complete(id,EPD_OPERATION_OK,0,p));
    CHECK(!epd_operation_complete(id,EPD_OPERATION_OK,0,NULL)); drain();
    CHECK(disposed==1 && users==1 && !receives); CHECK(epd_operation_release(id)); finish_scopes();
}
static void queue_full(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id; sync_complete=true; accept_result=false;
    CHECK(submit_as_owner(&spec,&id)); drain(); CHECK(receives==1 && !users && allocations==1);
    epd_operation_snapshot_t view; CHECK(epd_operation_get(id,&view) && view.result_pending && view.delivery_blocked);
    drain(); CHECK(receives==1); accept_result=true; CHECK(epd_operation_retry_delivery(id)); drain();
    CHECK(receives==2 && users==1 && !disposed && accepted_result); epd_app_free(accepted_result);
    CHECK(epd_operation_release(id)); finish_scopes();
}
static void deadline(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id;
    ticks=UINT32_MAX-3u; spec.has_deadline=true; spec.deadline_tick=5;
    CHECK(submit_as_owner(&spec,&id)); CHECK(epd_operation_next_timeout(100)==9); pump(1);
    ticks=6; epd_operation_poll(); drain(); CHECK(!receives && !users);
    CHECK(epd_operation_complete(id,EPD_OPERATION_OK,0,NULL)); drain();
    CHECK(received_status==EPD_OPERATION_EXPIRED && receives==1); CHECK(epd_operation_release(id));
    ticks=10; spec.deadline_tick=10; CHECK(submit_as_owner(&spec,&id)); drain();
    CHECK(runs==1 && received_status==EPD_OPERATION_EXPIRED); CHECK(epd_operation_release(id)); finish_scopes();
}
static void scope_generation(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id; sync_complete=true;
    CHECK(submit_as_owner(&spec,&id)); pump(1); CHECK(epd_operation_scope_close(business)); drain();
    CHECK(!receives && disposed==1 && users==1); CHECK(epd_operation_release(id));
    epd_operation_scope_t old=business; CHECK(epd_operation_scope_release(business));
    CHECK(epd_operation_scope_open(1,78,&business)); CHECK(business!=old);
    CHECK(!submit_as_owner(&spec,&id)); spec.scope=business;
    CHECK(submit_as_owner(&spec,&id)); drain(); CHECK(receives==1 && received_generation==78);
    epd_app_free(accepted_result); CHECK(epd_operation_release(id)); finish_scopes();
}
static void reentrant(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id;
    sync_complete=true; reenter=true; close_in_receive=true; accept_result=false;
    CHECK(submit_as_owner(&spec,&id)); drain();
    CHECK(runs==1 && receives==1 && disposed==1 && users==1); CHECK(epd_operation_release(id)); finish_scopes();
}
static void capacity(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t ids[EPD_OPERATION_CAPACITY], rejected=0;
    for(unsigned i=0;i<EPD_OPERATION_CAPACITY;++i) CHECK(submit_as_owner(&spec,&ids[i]));
    CHECK(!submit_as_owner(&spec,&rejected) && !rejected);
    for(unsigned i=0;i<EPD_OPERATION_CAPACITY;++i) { CHECK(epd_operation_cancel(ids[i])); CHECK(epd_operation_release(ids[i])); }
    drain(); CHECK(users==EPD_OPERATION_CAPACITY && receives==EPD_OPERATION_CAPACITY);
    CHECK(!epd_operation_cancel(ids[0])); finish_scopes();
    epd_operation_scope_t all[EPD_OPERATION_SCOPE_CAPACITY], extra=0;
    for(unsigned i=0;i<EPD_OPERATION_SCOPE_CAPACITY;++i) CHECK(epd_operation_scope_open(3,i,&all[i]));
    CHECK(!epd_operation_scope_open(3,99,&extra));
    for(unsigned i=0;i<EPD_OPERATION_SCOPE_CAPACITY;++i) { CHECK(epd_operation_scope_close(all[i])); CHECK(epd_operation_scope_release(all[i])); }
    CHECK(!refs[3]);
}
static void context_failure(void)
{
    epd_operation_spec_t spec=setup(); epd_operation_t id; sync_complete=true;
    CHECK(submit_as_owner(&spec,&id)); fail_enter=true; CHECK(pump(1)==0 && !runs); fail_enter=false;
    drain(); CHECK(runs==1 && users==1); epd_app_free(accepted_result); CHECK(epd_operation_release(id)); finish_scopes();
}
static epd_cold_state_spec_t cold_spec(const char *key, unsigned revision, bool optional)
{
    epd_operation_executor_t executor={&threads[1],&exec_event,1}; CHECK(epd_cold_state_init(&executor));
    epd_cold_state_spec_t spec={key,4,10,1,revision,optional}; return spec;
}
static void cold_pump(void) { current=&threads[1]; (void)epd_cold_state_dispatch(64); current=&threads[0]; }
static epd_cold_state_snapshot_t cold_get(epd_cold_state_t id)
{ epd_cold_state_snapshot_t view; CHECK(epd_cold_state_get(id,&view)); return view; }
static void cold_roundtrip(void)
{
    epd_cold_state_spec_t spec=cold_spec("app/position",1,false); epd_cold_state_t id;
    char bytes[]="serialized state";
    CHECK(epd_cold_state_put(&spec,bytes,sizeof(bytes),&id)); bytes[0]='X';
    CHECK(!io_calls && epd_cold_state_dispatch(1)==0); cold_pump();
    CHECK(cold_get(id).state==EPD_COLD_STATE_SAVED && commits==1); CHECK(epd_cold_state_release(id));
    spec.instance_generation=20;
    CHECK(epd_cold_state_load(&spec,&id)); cold_pump();
    CHECK(cold_get(id).instance_generation==20 && cold_get(id).state==EPD_COLD_STATE_LOADED);
    char out[32]="unchanged"; size_t size=999;
    CHECK(!epd_cold_state_copy(id,out,1,&size) && size==999 && !strcmp(out,"unchanged"));
    CHECK(epd_cold_state_copy(id,out,sizeof(out),&size) && !strcmp(out,"serialized state"));
    CHECK(epd_cold_state_release(id) && !refs[4]);
}
static void cold_failure(void)
{
    epd_cold_state_spec_t spec=cold_spec("app/edit",1,false); epd_cold_state_t old,id;
    CHECK(epd_cold_state_put(&spec,"old",4,&old)); cold_pump(); CHECK(epd_cold_state_release(old));
    spec.revision=2; fail_write=true; CHECK(epd_cold_state_put(&spec,"new",4,&id)); cold_pump();
    CHECK(cold_get(id).retry_required && refs[4]==1 && !epd_cold_state_release(id) && !epd_cold_state_abandon(id));
    CHECK(epd_cold_state_load(&spec,&old)); cold_pump(); char out[8]; size_t n;
    CHECK(epd_cold_state_copy(old,out,sizeof(out),&n) && !strcmp(out,"old")); CHECK(epd_cold_state_release(old));
    owner_closed[4]=true; fail_write=false; CHECK(epd_cold_state_retry(id)); cold_pump();
    CHECK(cold_get(id).state==EPD_COLD_STATE_SAVED); CHECK(epd_cold_state_release(id) && !refs[4]);
}
static void cold_revision(void)
{
    epd_cold_state_spec_t spec=cold_spec("app/edit",3,false); epd_cold_state_t a,b;
    CHECK(epd_cold_state_put(&spec,"three",6,&a)); spec.revision=2;
    CHECK(epd_cold_state_put(&spec,"two",4,&b)); cold_pump();
    CHECK(cold_get(a).state==EPD_COLD_STATE_SAVED && cold_get(b).error==EPD_COLD_STATE_STALE);
    CHECK(epd_cold_state_release(a)); epd_cold_state_blob_t blob={0}; CHECK(epd_cold_state_take(b,&blob));
    CHECK(!strcmp(blob.data,"two") && refs[4]==1); epd_cold_state_blob_release(&blob); epd_cold_state_blob_release(&blob);
    spec.revision=3; CHECK(epd_cold_state_put(&spec,"three",6,&a)); cold_pump();
    CHECK(commits==1 && cold_get(a).state==EPD_COLD_STATE_SAVED); CHECK(epd_cold_state_release(a));
    spec.format_version=2; CHECK(epd_cold_state_load(&spec,&a)); cold_pump();
    CHECK(cold_get(a).error==EPD_COLD_STATE_FORMAT); CHECK(epd_cold_state_release(a));
}
static void cold_capacity(void)
{
    epd_cold_state_spec_t spec=cold_spec("key",1,false); epd_cold_state_t ids[EPD_COLD_STATE_JOBS], id;
    fail_alloc=true; CHECK(!epd_cold_state_put(&spec,"a",2,&id) && !refs[4]); fail_alloc=false;
    for(unsigned batch=0;batch<2;++batch) {
        for(unsigned i=0;i<EPD_COLD_STATE_JOBS;++i) {
            char key[32]; snprintf(key,sizeof(key),"key%u",batch*EPD_COLD_STATE_JOBS+i); spec.key=key;
            CHECK(epd_cold_state_put(&spec,"a",2,&ids[i]));
        }
        CHECK(!epd_cold_state_put(&spec,"b",2,&id)); cold_pump();
        for(unsigned i=0;i<EPD_COLD_STATE_JOBS;++i) CHECK(epd_cold_state_release(ids[i]));
    }
    spec.key="extra"; spec.optional=true; CHECK(epd_cold_state_put(&spec,"x",2,&id)); cold_pump();
    CHECK(cold_get(id).error==EPD_COLD_STATE_FULL && epd_cold_state_abandon(id));
    CHECK(!allocations && !refs[4]);
}
static void cold_corrupt(void)
{
    epd_cold_state_spec_t spec=cold_spec("app/state",1,false); epd_cold_state_t id;
    CHECK(epd_cold_state_put(&spec,"one",4,&id)); cold_pump(); CHECK(epd_cold_state_release(id));
    FILE *f=fopen("00.bin","r+b"); CHECK(f); CHECK(fseek(f,16,SEEK_SET)==0); CHECK(fputc(0xff,f)!=EOF); CHECK(fclose(f)==0);
    spec.revision=2; CHECK(epd_cold_state_put(&spec,"two",4,&id)); cold_pump(); CHECK(cold_get(id).error==EPD_COLD_STATE_FORMAT && commits==1);
    epd_cold_state_blob_t blob; CHECK(epd_cold_state_take(id,&blob)); epd_cold_state_blob_release(&blob);
}
static void runtime_case(void)
{
    CHECK(epd_operation_runtime_init()); CHECK(epd_operation_runtime_init());
    epd_operation_executor_t execute,deliver,retire;
    CHECK(epd_operation_runtime_executors(&execute,&deliver,&retire));
    CHECK(execute.thread==&runtime_thread && retire.thread==execute.thread && deliver.thread==&threads[0]);
    CHECK(execute.wake->bits);
    current=&threads[3]; CHECK(!epd_operation_runtime_init() && !epd_operation_runtime_ui_pump(8));
    CHECK(!epd_operation_runtime_worker_pump(8,1));
    epd_cold_state_spec_t spec={"runtime/test",4,1,1,1,false}; epd_cold_state_t id;
    CHECK(epd_cold_state_put(&spec,"worker",7,&id)); CHECK(!io_calls);
    current=&runtime_thread; CHECK(epd_operation_runtime_worker_pump(8,1)==1);
    CHECK(cold_get(id).state==EPD_COLD_STATE_SAVED && epd_cold_state_release(id));
}
static void runtime_global_objects(void)
{
    CHECK(epd_memory_owner_retain(1));context=1;fail_global_scope=true;
    CHECK(!epd_operation_runtime_init() && context==1 && !global_scope);
    fail_global_scope=false;CHECK(epd_operation_runtime_init() && context==1 && !global_scope);
    CHECK(ui_settings_store_start() && context==1 && !global_scope);
    context=0;epd_memory_owner_release(1);
}
static int32_t worker_until_idle(void)
{
    rt_thread_t saved=current;current=&runtime_thread;stop_at_idle=true;event_waits=0;
    if(!setjmp(idle_jump)) runtime_thread.entry(runtime_thread.parameter);
    stop_at_idle=false;current=saved;return observed_wait;
}
typedef struct {size_t pending, calls;uint32_t deadline;} client_state_t;
static size_t client_process(size_t budget,void *user)
{
    client_state_t *client=user;CHECK(current==&runtime_thread && !irq_depth && !context);
    size_t n=client->pending<budget?client->pending:budget;
    client->pending-=n;++client->calls;return n;
}
static uint32_t client_deadline(uint32_t maximum,void *user)
{
    client_state_t *client=user;CHECK(current==&runtime_thread && !irq_depth && !context);
    return client->deadline<maximum?client->deadline:maximum;
}
static void runtime_deadlines(void)
{
    static client_state_t first={0,0,UINT32_MAX},second={0,0,UINT32_MAX};
    epd_operation_runtime_client_t a={&first,&first,client_process,client_deadline};
    epd_operation_runtime_client_t b={&second,&second,client_process,client_deadline};
    CHECK(epd_operation_runtime_register(&a));CHECK(epd_operation_runtime_init());
    CHECK(worker_until_idle()==RT_WAITING_FOREVER);
    first.deadline=37;epd_operation_runtime_wake();CHECK(worker_until_idle()==37);
    CHECK(epd_operation_runtime_register(&b));second.deadline=13;
    CHECK(worker_until_idle()==13);
    CHECK(epd_operation_runtime_next_timeout_ticks(99)==99);
    current=&runtime_thread;CHECK(epd_operation_runtime_next_timeout_ticks(7)==7);current=&threads[0];
    epd_operation_spec_t spec=setup();epd_operation_t id;
    ticks=100;spec.has_deadline=true;spec.deadline_tick=105;
    CHECK(submit_as_owner(&spec,&id));CHECK(worker_until_idle()==5);
    CHECK(epd_operation_cancel(id));drain();CHECK(epd_operation_release(id));finish_scopes();
    first.deadline=second.deadline=UINT32_MAX;CHECK(worker_until_idle()==RT_WAITING_FOREVER);
}
static void runtime_clients(void)
{
    static client_state_t states[EPD_OPERATION_RUNTIME_CLIENTS+1];
    epd_operation_runtime_client_t client={0};
    client.process=client_process;client.next_timeout_ticks=client_deadline;
    for(unsigned i=0;i<EPD_OPERATION_RUNTIME_CLIENTS;++i) {
        states[i].pending=19;states[i].deadline=UINT32_MAX;client.key=client.user=&states[i];
        CHECK(epd_operation_runtime_register(&client));CHECK(epd_operation_runtime_register(&client));
    }
    client.key=client.user=&states[EPD_OPERATION_RUNTIME_CLIENTS];CHECK(!epd_operation_runtime_register(&client));
    client.key=&states[0];CHECK(!epd_operation_runtime_register(&client));
    CHECK(epd_operation_runtime_init());
    CHECK(!epd_operation_runtime_worker_pump(2,0));
    current=&runtime_thread;CHECK(epd_operation_runtime_worker_pump(2,0)==2*EPD_OPERATION_RUNTIME_CLIENTS);
    current=&threads[0];
    for(unsigned i=0;i<EPD_OPERATION_RUNTIME_CLIENTS;++i) CHECK(states[i].pending==17 && states[i].calls==1);
    CHECK(worker_until_idle()==RT_WAITING_FOREVER);
    for(unsigned i=0;i<EPD_OPERATION_RUNTIME_CLIENTS;++i) CHECK(!states[i].pending && states[i].calls>=4);
}
static void runtime_cold_burst(void)
{
    CHECK(epd_operation_runtime_init());epd_cold_state_t ids[EPD_COLD_STATE_JOBS];
    epd_cold_state_spec_t spec={NULL,4,1,1,1,false};
    for(unsigned i=0;i<EPD_COLD_STATE_JOBS;++i) {
        char key[20];snprintf(key,sizeof(key),"burst/%u",i);spec.key=key;
        CHECK(epd_cold_state_put(&spec,"bytes",6,&ids[i]));
    }
    CHECK(worker_until_idle()==RT_WAITING_FOREVER && commits==EPD_COLD_STATE_JOBS);
    CHECK(power_wakes>=EPD_COLD_STATE_JOBS);
    for(unsigned i=0;i<EPD_COLD_STATE_JOBS;++i) CHECK(cold_get(ids[i]).state==EPD_COLD_STATE_SAVED && epd_cold_state_release(ids[i]));
}
static void runtime_ui_wake(void)
{
    CHECK(epd_operation_runtime_init());epd_operation_spec_t spec=setup();epd_operation_t ids[2];
    spec.deliver.wake=NULL;spec.deliver.wake_bits=0;
    for(unsigned i=0;i<2;++i) CHECK(submit_as_owner(&spec,&ids[i]));
    pump(1);unsigned before=power_wakes;
    for(unsigned i=0;i<2;++i) CHECK(epd_operation_complete(ids[i],EPD_OPERATION_OK,0,NULL));
    CHECK(power_wakes>before);before=power_wakes;current=&threads[0];
    CHECK(epd_operation_runtime_ui_pump(1)==1 && receives==1 && power_wakes>before);
    drain();CHECK(receives==2 && users==2);
    for(unsigned i=0;i<2;++i) CHECK(epd_operation_release(ids[i]));finish_scopes();
}
static void owner_close(void)
{
    parents[1]=5; parents[2]=5;
    epd_operation_spec_t spec=setup();epd_operation_t id;
    CHECK(submit_as_owner(&spec,&id));pump(1);
    epd_operation_runtime_close_owner(5);
    CHECK(!epd_operation_runtime_owner_drained(5));
    CHECK(!epd_operation_scope_open(1,78,&business));
    CHECK(!submit_as_owner(&spec,&id));
    void *data=epd_app_alloc(8,EPD_APP_PSRAM);
    CHECK(epd_operation_complete(id,EPD_OPERATION_OK,0,data));drain();
    CHECK(!receives && disposed==1 && users==1 && !refs[1] && !refs[2]);
    CHECK(epd_operation_runtime_owner_drained(5) && epd_operation_release(id));
    CHECK(!epd_operation_scope_release(cleanup));
}
static void settings_pump(void)
{current=&runtime_thread;(void)epd_operation_runtime_worker_pump(64,1);current=&threads[0];}
static void task_run(epd_operation_t id,void *user)
{
    CHECK(user==&runs && !irq_depth && context==1 && task_running[1]);++runs;
    if(task_yield_once && runs==1) {CHECK(epd_operation_yield(id));return;}
    if(sync_complete) {
        CHECK(epd_operation_complete(id,EPD_OPERATION_OK,0,NULL));
        if(reenter) {CHECK(current==&runtime_thread);(void)epd_operation_runtime_worker_pump(8,0);CHECK(task_running[1]);}
    }
}
static void task_shared_owner(void)
{
    CHECK(epd_operation_runtime_init());kinds[1]=EPD_MEMORY_SCOPE_TASK;
    epd_operation_spec_t spec=setup();spec.run=task_run;epd_operation_t first,second;
    CHECK(submit_as_owner(&spec,&first));epd_operation_scope_t other;
    CHECK(epd_operation_scope_open(1,88,&other));spec.scope=other;CHECK(submit_as_owner(&spec,&second));
    CHECK(pump(1)==0 && !runs && !task_starts);settings_pump();
    CHECK(task_starts==1 && !task_ends);pump(1);CHECK(runs==2);
    CHECK(epd_operation_cancel(first));settings_pump();CHECK(!task_ends);
    CHECK(epd_operation_complete(first,EPD_OPERATION_OK,0,NULL));drain();settings_pump();CHECK(!task_ends);
    CHECK(epd_operation_complete(second,EPD_OPERATION_OK,0,NULL));settings_pump();drain();
    CHECK(task_ends==1 && !task_running[1] && users==2);
    CHECK(epd_operation_release(first) && epd_operation_release(second));
    CHECK(epd_operation_scope_close(other) && epd_operation_scope_release(other));finish_scopes();
}
static void task_yield_and_return(void)
{
    CHECK(epd_operation_runtime_init());kinds[1]=EPD_MEMORY_SCOPE_SERVICE;
    epd_operation_spec_t spec=setup();spec.run=task_run;epd_operation_t id;
    epd_operation_executor_t execute,deliver,retire;CHECK(epd_operation_runtime_executors(&execute,&deliver,&retire));
    spec.execute=execute;task_yield_once=sync_complete=reenter=true;
    CHECK(submit_as_owner(&spec,&id));current=&runtime_thread;
    CHECK(epd_operation_runtime_worker_pump(1,0)==1 && runs==1 && task_running[1]);
    CHECK(epd_operation_runtime_worker_pump(1,0)==1 && runs==2 && !task_running[1]);
    CHECK(task_starts==1 && task_ends==1);drain();CHECK(epd_operation_release(id));finish_scopes();
}
static void task_start_failure(void)
{
    CHECK(epd_operation_runtime_init());kinds[1]=EPD_MEMORY_SCOPE_TASK;
    epd_operation_spec_t spec=setup();spec.run=task_run;epd_operation_t id;
    CHECK(submit_as_owner(&spec,&id));CHECK(pump(1)==0);CHECK(epd_operation_cancel(id));settings_pump();drain();
    CHECK(!task_starts && !runs && epd_operation_release(id));
    task_hook_error=EPD_RESOURCE_CAPACITY;CHECK(submit_as_owner(&spec,&id));CHECK(pump(1)==0);
    settings_pump();drain();settings_pump();
    epd_operation_snapshot_t state;CHECK(epd_operation_get(id,&state));
    CHECK(!runs && state.status==EPD_OPERATION_FAILED && state.error==EPD_RESOURCE_CAPACITY);
    CHECK(!task_starts && epd_operation_release(id));finish_scopes();
}
static void public_example(void)
{
    CHECK(epd_operation_runtime_init());operation_example_t job={0};unsigned char bytes[100];
    for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(unsigned char)i;
    CHECK(!operation_example_start(&job,1,1,bytes,sizeof(bytes),100));kinds[1]=EPD_MEMORY_SCOPE_TASK;
    context=1;CHECK(operation_example_start(&job,1,1,bytes,sizeof(bytes),100));context=0;memset(bytes,0,sizeof(bytes));
    uint32_t sum=0;epd_operation_status_t status=EPD_OPERATION_FAILED;
    CHECK(!operation_example_finish(&job,&sum,&status));
    settings_pump();CHECK(epd_operation_runtime_ui_pump(8)==1);settings_pump();
    CHECK(operation_example_finish(&job,&sum,&status) && sum==4950 && status==EPD_OPERATION_OK && !refs[1]);
    context=1;CHECK(operation_example_start(&job,1,2,bytes,sizeof(bytes),100));context=0;CHECK(operation_example_cancel(&job));
    settings_pump();(void)epd_operation_runtime_ui_pump(8);settings_pump();
    CHECK(operation_example_finish(&job,&sum,&status) && status==EPD_OPERATION_CANCELLED && !refs[1]);
    epd_cold_state_spec_t spec={"example/position",4,10,1,1,false};epd_cold_state_t state;
    CHECK(operation_example_save_position(&spec,7,42,&state));settings_pump();CHECK(epd_cold_state_release(state));
    spec.instance_generation=11;CHECK(epd_cold_state_load(&spec,&state));settings_pump();sum=999;
    CHECK(!operation_example_restore_position(state,10,7,&sum) && sum==999);
    CHECK(!operation_example_restore_position(state,11,8,&sum) && sum==999);
    CHECK(operation_example_restore_position(state,11,7,&sum) && sum==42);
    CHECK(epd_cold_state_release(state));
}
static void settings_save(void)
{
    uint8_t values[UI_SETTING_COUNT]={0};
    CHECK(ui_settings_store_start());values[UI_SETTING_TIMEOUT]=1;
    CHECK(ui_settings_store_save(values));values[UI_SETTING_TIMEOUT]=2;CHECK(ui_settings_store_save(values));
    CHECK(!io_calls && ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_PENDING && sleep_holds==1);
    settings_pump();CHECK(commits==1 && !sleep_holds && ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_OK);
    uint8_t loaded[UI_SETTING_COUNT];CHECK(ui_settings_store_load(loaded) && loaded[UI_SETTING_TIMEOUT]==2);
    CHECK(ui_settings_store_save(values));settings_pump();CHECK(commits==1);
}
static void settings_failure(void)
{
    uint8_t values[UI_SETTING_COUNT]={0};CHECK(ui_settings_store_start());
    values[UI_SETTING_TIMEOUT]=1;CHECK(ui_settings_store_save(values));settings_pump();
    unsigned char original[76],after[76];FILE *file=fopen("system/settings.bin","rb");CHECK(file);
    CHECK(fread(original,1,sizeof(original),file)==sizeof(original));CHECK(fclose(file)==0);fail_write=true;
    values[UI_SETTING_TIMEOUT]=2;CHECK(ui_settings_store_save(values));settings_pump();
    CHECK(ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_FAILED && !sleep_holds);
    file=fopen("system/settings.bin","rb");CHECK(file);
    CHECK(fread(after,1,sizeof(after),file)==sizeof(after));CHECK(fclose(file)==0);
    CHECK(!memcmp(original,after,sizeof(original)));
    uint8_t loaded[UI_SETTING_COUNT];memset(loaded,99,sizeof(loaded));
    CHECK(ui_settings_store_reload(loaded)==UI_SETTINGS_LOAD_IO_ERROR && loaded[0]==99);
    settings_pump();CHECK(commits==1);fail_write=false;CHECK(ui_settings_store_save(values));settings_pump();
    CHECK(ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_OK && commits==2);
    CHECK(ui_settings_store_load(loaded) && loaded[UI_SETTING_TIMEOUT]==2);
}
static void settings_backpressure(void)
{
    uint8_t values[UI_SETTING_COUNT]={0};CHECK(ui_settings_store_start());
    values[UI_SETTING_TIMEOUT]=1;CHECK(ui_settings_store_save(values));settings_pump();
    epd_operation_spec_t spec=setup();epd_operation_t ids[EPD_OPERATION_CAPACITY];
    for(unsigned attempt=0;attempt<2;++attempt) {
        for(unsigned i=0;i<EPD_OPERATION_CAPACITY;++i) CHECK(submit_as_owner(&spec,&ids[i]));
        values[UI_SETTING_TIMEOUT]=4;CHECK(!ui_settings_store_save(values) && !sleep_holds);
        CHECK(ui_settings_store_status(NULL)==(attempt?UI_SETTINGS_SAVE_FAILED:UI_SETTINGS_SAVE_OK));
        for(unsigned i=0;i<EPD_OPERATION_CAPACITY;++i) {CHECK(epd_operation_cancel(ids[i]));CHECK(epd_operation_release(ids[i]));}
        drain();
        if(!attempt) {
            fail_write=true;values[UI_SETTING_TIMEOUT]=3;CHECK(ui_settings_store_save(values));settings_pump();
            CHECK(ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_FAILED);
        }
    }
    CHECK(commits==1);fail_write=false;values[UI_SETTING_TIMEOUT]=3;
    CHECK(ui_settings_store_save(values));settings_pump();
    uint8_t loaded[UI_SETTING_COUNT];CHECK(ui_settings_store_load(loaded) && loaded[UI_SETTING_TIMEOUT]==3);
    finish_scopes();
}
static void settings_reset(void)
{
    uint8_t values[UI_SETTING_COUNT]={0};CHECK(ui_settings_store_start());
    values[UI_SETTING_TIMEOUT]=1;CHECK(ui_settings_store_save(values));
    values[UI_SETTING_TIMEOUT]=4;CHECK(ui_settings_store_save_sync(values));settings_pump();
    CHECK(!sleep_holds && commits==1);uint8_t loaded[UI_SETTING_COUNT];
    CHECK(ui_settings_store_load(loaded) && loaded[UI_SETTING_TIMEOUT]==4);
    fail_write=true;values[UI_SETTING_TIMEOUT]=2;CHECK(!ui_settings_store_save_sync(values));settings_pump();
    CHECK(ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_FAILED);
}
static void settings_during_write(void)
{
    uint8_t values[UI_SETTING_COUNT]={0};CHECK(ui_settings_store_start());
    values[UI_SETTING_TIMEOUT]=1;CHECK(ui_settings_store_save(values));change_during_write=true;
    current=&runtime_thread;CHECK(epd_operation_runtime_worker_pump(1,0)==1);current=&threads[0];
    CHECK(ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_PENDING && sleep_holds==1 && commits==1);
    settings_pump();CHECK(commits==2 && !sleep_holds && ui_settings_store_status(NULL)==UI_SETTINGS_SAVE_OK);
    uint8_t loaded[UI_SETTING_COUNT];CHECK(ui_settings_store_load(loaded) && loaded[UI_SETTING_TIMEOUT]==3);
}
static void checkpoint_pump(void)
{
    CHECK(!context);
    for(unsigned i=0;i<4;++i) {
        settings_pump();
        (void)epd_operation_runtime_ui_pump(8);
    }
}
static void checkpoint_bind(epd_memory_owner_t owner,const char *id)
{
    CHECK(!refs[owner]);refs[owner]=1;kinds[owner]=EPD_MEMORY_SCOPE_APP;appids[owner]=id;
    CHECK(app_background_should_start(id,true));
    CHECK(app_background_bind(id,owner)==EPD_MEMORY_OK);
}
static void checkpoint_open(epd_memory_owner_t owner,hello_state_t *state)
{
    epd_memory_owner_t previous=epd_memory_owner_enter(owner);
    CHECK(hello_state_open(state));
    epd_memory_owner_leave(previous);
}
static void checkpoint_increment(epd_memory_owner_t owner,hello_state_t *state)
{
    epd_memory_owner_t previous=epd_memory_owner_enter(owner);
    CHECK(hello_state_increment(state));
    epd_memory_owner_leave(previous);
}
static epd_cold_runtime_snapshot_t checkpoint_view(epd_memory_owner_t owner)
{
    epd_cold_runtime_snapshot_t state;
    CHECK(epd_cold_runtime_get(owner,&state));return state;
}
static bool checkpoint_call(epd_memory_owner_t owner, bool (*call)(epd_checkpoint_t), epd_checkpoint_t id)
{
    epd_memory_owner_t previous=epd_memory_owner_enter(owner);
    bool result=call(id);
    epd_memory_owner_leave(previous);return result;
}
static void checkpoint_finish(epd_memory_owner_t owner,hello_state_t *model)
{
    epd_app_background_state_t terminal;
    epd_app_background_snapshot_t before;
    CHECK(app_background_snapshot(owner,&before)==EPD_MEMORY_OK);
    CHECK(app_background_prepare_close(owner,&terminal)==EPD_MEMORY_OK);
    CHECK(terminal==(before.effective==EPD_APP_BG_STATE ? EPD_APP_BG_COLD : EPD_APP_BG_RELEASED));
    CHECK(!owner_closed[owner] && refs[owner]>=3);
    CHECK(!checkpoint_call(owner,epd_checkpoint_changed,model->checkpoint));
    epd_cold_runtime_close_owner(owner);epd_cold_runtime_close_owner(owner);
    CHECK(epd_cold_runtime_owner_busy(owner) && model->checkpoint);
    checkpoint_pump();
    CHECK(!epd_cold_runtime_owner_busy(owner) && !model->checkpoint && !owner_closed[owner]);
    epd_operation_runtime_close_owner(owner);
    CHECK(owner_closed[owner] && epd_operation_runtime_owner_drained(owner));
    epd_app_background_snapshot_t background;
    CHECK(app_background_snapshot(owner,&background)==EPD_MEMORY_OK);
    CHECK(background.saved_revision==background.dirty_revision);
    CHECK(app_background_report(owner,background.revision,terminal)==EPD_MEMORY_OK);
    CHECK(app_background_unbind(owner)==EPD_MEMORY_OK && refs[owner]==1);
    CHECK(epd_memory_owner_close(owner));epd_memory_owner_release(owner);
}
static void checkpoint_save(epd_memory_owner_t owner)
{
    epd_app_background_state_t terminal;
    CHECK(app_background_foreground(owner,false)==EPD_MEMORY_OK);
    epd_memory_result_t result=app_background_prepare_close(owner,&terminal);
    CHECK(result==EPD_MEMORY_WAITING || result==EPD_MEMORY_OK);
    checkpoint_pump();
    CHECK(checkpoint_view(owner).durable);
}
static void checkpoint_cycle(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"counter-demo");
    hello_state_t old={0};checkpoint_open(1,&old);
    epd_app_background_snapshot_t policy;
    CHECK(app_background_snapshot(1,&policy)==EPD_MEMORY_OK && policy.effective==EPD_APP_BG_RELEASE);
    checkpoint_increment(1,&old);checkpoint_increment(1,&old);
    CHECK(app_background_snapshot(1,&policy)==EPD_MEMORY_OK && policy.effective==EPD_APP_BG_STATE);
    CHECK(!io_calls && old.count==2);checkpoint_pump();checkpoint_save(1);
    CHECK(commits==1 && checkpoint_view(1).saved_revision==2);
    checkpoint_finish(1,&old);
    hello_state_t fresh={0};checkpoint_bind(2,"counter-demo");checkpoint_open(2,&fresh);
    CHECK(!fresh.count && checkpoint_view(2).restoring);checkpoint_pump();CHECK(fresh.count==2);
    checkpoint_save(2);CHECK(commits==1);checkpoint_finish(2,&fresh);
}
static void checkpoint_explicit_stop(void)
{
    enforce_retain_account=true;
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"counter-demo");
    hello_state_t model={0};checkpoint_open(1,&model);checkpoint_pump();
    CHECK(!checkpoint_view(1).failed && !checkpoint_view(1).restoring && !context);
    checkpoint_increment(1,&model);
    CHECK(app_background_foreground(1,false)==EPD_MEMORY_OK);
    CHECK(app_background_request_stop(1)==EPD_MEMORY_OK && !app_background_accepting(1));
    epd_app_background_state_t terminal;
    CHECK(app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);
    checkpoint_pump();CHECK(commits==1 && checkpoint_view(1).durable);
    checkpoint_finish(1,&model);

    hello_state_t restored={0};checkpoint_bind(2,"counter-demo");checkpoint_open(2,&restored);
    checkpoint_pump();CHECK(restored.count==1);
    checkpoint_save(2);CHECK(commits==1);
    CHECK(app_background_request_stop(2)==EPD_MEMORY_OK && !app_background_accepting(2));
    checkpoint_finish(2,&restored);CHECK(commits==1);
}
static void checkpoint_save_failure(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"counter-demo");
    hello_state_t old={0};checkpoint_open(1,&old);checkpoint_increment(1,&old);
    checkpoint_pump();checkpoint_save(1);checkpoint_finish(1,&old);
    hello_state_t fresh={0};checkpoint_bind(2,"counter-demo");checkpoint_open(2,&fresh);checkpoint_pump();
    CHECK(fresh.count==1);checkpoint_increment(2,&fresh);fail_write=true;
    CHECK(app_background_foreground(2,false)==EPD_MEMORY_OK);
    epd_app_background_state_t terminal;
    CHECK(app_background_prepare_close(2,&terminal)==EPD_MEMORY_WAITING);checkpoint_pump();
    CHECK(checkpoint_view(2).failed && !checkpoint_view(2).durable && commits==1);
    CHECK(!checkpoint_call(2,epd_checkpoint_unregister,fresh.checkpoint));
    unsigned attempts=io_calls;checkpoint_pump();
    CHECK(io_calls==attempts && app_background_prepare_close(2,&terminal)!=EPD_MEMORY_OK);
    CHECK(epd_cold_runtime_owner_busy(2) && !owner_closed[2] && fresh.count==2);
    /* The prior file remains readable while the accepted replacement is failed. */
    epd_cold_state_spec_t spec={"counter-demo/counter",0,99,1,0,false};epd_cold_state_t load;
    CHECK(epd_cold_state_load(&spec,&load));settings_pump();unsigned char bytes[16];size_t size;
    CHECK(epd_cold_state_copy(load,bytes,sizeof(bytes),&size) && size==16 && bytes[12]==1);
    CHECK(epd_cold_state_release(load));
    fail_write=false;CHECK(checkpoint_call(2,epd_checkpoint_retry,fresh.checkpoint));checkpoint_pump();
    CHECK(commits==2 && checkpoint_view(2).durable);checkpoint_finish(2,&fresh);
    hello_state_t restored={0};checkpoint_bind(3,"counter-demo");checkpoint_open(3,&restored);checkpoint_pump();
    CHECK(restored.count==2);checkpoint_save(3);checkpoint_finish(3,&restored);
}
static void checkpoint_reopen(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"counter-demo");
    hello_state_t model={0};checkpoint_open(1,&model);checkpoint_pump();checkpoint_increment(1,&model);
    CHECK(app_background_foreground(1,false)==EPD_MEMORY_OK);epd_app_background_state_t terminal;
    CHECK(app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);
    (void)epd_operation_runtime_ui_pump(8);CHECK(checkpoint_view(1).busy && !commits);
    CHECK(app_background_foreground(1,true)==EPD_MEMORY_OK);checkpoint_pump();
    CHECK(commits==1 && app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);
    CHECK(!owner_closed[1] && model.checkpoint);
    checkpoint_increment(1,&model);checkpoint_save(1);CHECK(commits==2);checkpoint_finish(1,&model);
    hello_state_t fresh={0};checkpoint_bind(2,"counter-demo");checkpoint_open(2,&fresh);checkpoint_pump();
    CHECK(fresh.count==2);checkpoint_save(2);checkpoint_finish(2,&fresh);
}
static void checkpoint_edit_before_restore(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"counter-demo");
    hello_state_t old={0};checkpoint_open(1,&old);checkpoint_pump();
    for(unsigned i=0;i<5;++i) checkpoint_increment(1,&old);
    checkpoint_save(1);checkpoint_finish(1,&old);
    hello_state_t fresh={0};checkpoint_bind(2,"counter-demo");checkpoint_open(2,&fresh);
    checkpoint_increment(2,&fresh);checkpoint_pump();CHECK(fresh.count==6);
    CHECK(checkpoint_view(2).data_revision==6);checkpoint_save(2);checkpoint_finish(2,&fresh);
    hello_state_t restored={0};checkpoint_bind(3,"counter-demo");checkpoint_open(3,&restored);checkpoint_pump();
    CHECK(restored.count==6);checkpoint_save(3);checkpoint_finish(3,&restored);
}
typedef struct {
    hello_state_t data;
    epd_memory_owner_t owner;
    unsigned captures, restores, detaches;
    bool edit_in_capture, restore_failure;
} checkpoint_model_t;
static bool checkpoint_capture(void *user,void *bytes,size_t capacity,size_t *size)
{
    checkpoint_model_t *model=user;
    CHECK(!irq_depth && current==&threads[0] && context==model->owner && capacity>=4);
    unsigned char *out=bytes;
    for(unsigned i=0;i<4;++i) out[i]=(unsigned char)(model->data.count>>(i*8));
    *size=4;++model->captures;
    if(model->edit_in_capture) {
        model->edit_in_capture=false;
        CHECK(epd_checkpoint_changed(model->data.checkpoint));++model->data.count;
        CHECK(!epd_operation_runtime_ui_pump(8));
    }
    return true;
}
static bool checkpoint_restore_model(void *user,const void *bytes,size_t size)
{
    checkpoint_model_t *model=user;
    CHECK(!irq_depth && current==&threads[0] && context==model->owner && size==4);
    ++model->restores;if(model->restore_failure) return false;
    const unsigned char *in=bytes;
    model->data.count=(uint32_t)in[0]|(uint32_t)in[1]<<8|(uint32_t)in[2]<<16|(uint32_t)in[3]<<24;
    return true;
}
static void checkpoint_detach_model(void *user)
{
    checkpoint_model_t *model=user;
    CHECK(!irq_depth && current==&threads[0] && context==model->owner && !owner_closed[context]);
    CHECK(epd_cold_runtime_owner_busy(context));++model->detaches;model->data.checkpoint=0;
}
static bool checkpoint_model_open(checkpoint_model_t *model,uint32_t version)
{
    epd_checkpoint_spec_t spec={"model",1,version,4,false,model,checkpoint_capture,checkpoint_restore_model,checkpoint_detach_model};
    epd_memory_owner_t previous=epd_memory_owner_enter(model->owner);
    bool ok=epd_checkpoint_register(&spec,&model->data.checkpoint);
    if(ok) {
        epd_app_background_policy_t policy={EPD_APP_BG_STATE,0,EPD_APP_BG_RELEASE};
        CHECK(epd_app_background_set(&policy,NULL)==EPD_MEMORY_OK);
    }
    epd_memory_owner_leave(previous);return ok;
}
static void checkpoint_reentrant_capture(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"model-demo");
    checkpoint_model_t model={0};model.owner=1;model.edit_in_capture=true;
    CHECK(checkpoint_model_open(&model,7));checkpoint_pump();
    epd_app_background_state_t terminal;CHECK(app_background_foreground(1,false)==EPD_MEMORY_OK);
    CHECK(app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);checkpoint_pump();
    CHECK(commits==1 && model.data.count==1 && checkpoint_view(1).data_revision==2 && !checkpoint_view(1).durable);
    CHECK(app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);checkpoint_pump();
    CHECK(commits==2 && model.captures==2);checkpoint_finish(1,&model.data);CHECK(model.detaches==1);
}
static void checkpoint_restore_retry(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"model-demo");
    checkpoint_model_t old={0};old.owner=1;old.data.count=17;
    CHECK(checkpoint_model_open(&old,7));checkpoint_pump();checkpoint_save(1);checkpoint_finish(1,&old.data);
    checkpoint_model_t fresh={0};fresh.owner=2;fresh.restore_failure=true;
    checkpoint_bind(2,"model-demo");CHECK(checkpoint_model_open(&fresh,7));checkpoint_pump();
    CHECK(fresh.restores==1 && !fresh.data.count && checkpoint_view(2).failed && allocations==1);
    unsigned reads=io_calls;checkpoint_pump();CHECK(fresh.restores==1 && reads==io_calls);
    fresh.restore_failure=false;CHECK(checkpoint_call(2,epd_checkpoint_retry,fresh.data.checkpoint));checkpoint_pump();
    CHECK(fresh.restores==2 && fresh.data.count==17 && !allocations);
    checkpoint_save(2);CHECK(commits==1);checkpoint_finish(2,&fresh.data);CHECK(fresh.detaches==1);
}
static void checkpoint_format_and_duplicate(void)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"model-demo");
    checkpoint_model_t old={0};old.owner=1;old.data.count=23;
    CHECK(checkpoint_model_open(&old,7));checkpoint_pump();
    checkpoint_model_t duplicate={0};duplicate.owner=1;CHECK(!checkpoint_model_open(&duplicate,7));
    checkpoint_save(1);checkpoint_finish(1,&old.data);
    checkpoint_model_t wrong={0};wrong.owner=2;checkpoint_bind(2,"model-demo");
    CHECK(checkpoint_model_open(&wrong,8));checkpoint_pump();
    CHECK(!wrong.restores && !wrong.data.count && checkpoint_view(2).failed && commits==1);
    CHECK(checkpoint_call(2,epd_checkpoint_unregister,wrong.data.checkpoint));checkpoint_pump();
    CHECK(wrong.detaches==1 && !epd_cold_runtime_owner_busy(2) && !allocations);
    epd_memory_owner_t previous=epd_memory_owner_enter(2);
    epd_app_background_policy_t policy={EPD_APP_BG_RELEASE,0,EPD_APP_BG_RELEASE};
    CHECK(epd_app_background_set(&policy,NULL)==EPD_MEMORY_OK);epd_memory_owner_leave(previous);
    CHECK(app_background_foreground(2,false)==EPD_MEMORY_OK);epd_app_background_state_t terminal;
    CHECK(app_background_prepare_close(2,&terminal)==EPD_MEMORY_OK && terminal==EPD_APP_BG_RELEASED);
    epd_app_background_snapshot_t state;CHECK(app_background_snapshot(2,&state)==EPD_MEMORY_OK);
    CHECK(app_background_report(2,state.revision,terminal)==EPD_MEMORY_OK);
    CHECK(app_background_unbind(2)==EPD_MEMORY_OK);CHECK(epd_memory_owner_close(2));epd_memory_owner_release(2);
    checkpoint_model_t correct={0};correct.owner=3;checkpoint_bind(3,"model-demo");
    CHECK(checkpoint_model_open(&correct,7));checkpoint_pump();CHECK(correct.data.count==23);
    checkpoint_save(3);CHECK(commits==1);checkpoint_finish(3,&correct.data);
}
static void checkpoint_init_abort(void)
{
    CHECK(epd_operation_runtime_init());
    for(unsigned owner=1;owner<=2;++owner) {
        checkpoint_bind(owner,"model-demo");checkpoint_model_t model={0};model.owner=owner;
        CHECK(checkpoint_model_open(&model,7));
        if(owner==2) {
            current=&runtime_thread;CHECK(epd_cold_runtime_worker_pump(1)==1);current=&threads[0];
            CHECK(allocations==1 && !io_calls);
        }
        /* Initialization aborts before any edit. A queued read still owns its
         * buffer until the shared worker and GUI detach have actually finished. */
        epd_cold_runtime_close_owner(owner);epd_cold_runtime_close_owner(owner);
        CHECK(epd_cold_runtime_owner_busy(owner));checkpoint_pump();
        CHECK(!epd_cold_runtime_owner_busy(owner) && model.detaches==1 && !model.restores && !allocations);
        CHECK(!owner_closed[owner] && !commits);
        epd_memory_owner_t previous=epd_memory_owner_enter(owner);
        epd_app_background_policy_t policy={EPD_APP_BG_RELEASE,0,EPD_APP_BG_RELEASE};
        CHECK(epd_app_background_set(&policy,NULL)==EPD_MEMORY_OK);epd_memory_owner_leave(previous);
        CHECK(app_background_foreground(owner,false)==EPD_MEMORY_OK);epd_app_background_state_t terminal;
        CHECK(app_background_prepare_close(owner,&terminal)==EPD_MEMORY_OK && terminal==EPD_APP_BG_RELEASED);
        epd_operation_runtime_close_owner(owner);epd_app_background_snapshot_t state;
        CHECK(app_background_snapshot(owner,&state)==EPD_MEMORY_OK);
        CHECK(app_background_report(owner,state.revision,terminal)==EPD_MEMORY_OK);
        CHECK(app_background_unbind(owner)==EPD_MEMORY_OK);epd_memory_owner_release(owner);
    }
}
#include "checkpoint_notify.inc"
#include "cold_budget.inc"
int main(int argc, char **argv)
{
    budget_reset();
    CHECK(!test_init_operation_metadata_init() && !test_init_cold_metadata_init() &&
          !test_init_operation_runtime_metadata_init() && !test_init_checkpoint_metadata_init() &&
          !test_init_background_metadata_init() && !test_init_events_init());
    CHECK(argc==2);
    struct {const char *name; void (*run)(void);} cases[]={
        {"budget_metadata",budget_metadata},
        {"budget_slots",budget_slots},
        {"budget_order",budget_order},{"budget_required",budget_required},{"budget_leases",budget_leases},
        {"budget_reboot",budget_reboot},{"budget_index_failure",budget_index_failure},
        {"budget_cold_cross_class",budget_cold_cross_class},{"budget_deferred",budget_deferred},{"budget_media",budget_media},
        {"budget_reboot_pending",budget_reboot_pending},{"budget_corrupt_index",budget_corrupt_index},
        {"cancel_queued",cancel_queued},{"cancel_running",cancel_running},{"queue_full",queue_full},
        {"deadline",deadline},{"scope_generation",scope_generation},{"reentrant",reentrant},
        {"capacity",capacity},{"context_failure",context_failure},{"cold_roundtrip",cold_roundtrip},
        {"cold_failure",cold_failure},{"cold_revision",cold_revision},{"cold_capacity",cold_capacity},
        {"cold_corrupt",cold_corrupt},{"runtime",runtime_case},{"owner_close",owner_close},
        {"runtime_deadlines",runtime_deadlines},{"runtime_clients",runtime_clients},
        {"runtime_cold_burst",runtime_cold_burst},{"runtime_ui_wake",runtime_ui_wake},
        {"settings_save",settings_save},{"settings_failure",settings_failure},
        {"settings_backpressure",settings_backpressure},
        {"task_shared_owner",task_shared_owner},{"task_yield_and_return",task_yield_and_return},
        {"task_start_failure",task_start_failure},
        {"public_example",public_example},
        {"checkpoint_cycle",checkpoint_cycle},{"checkpoint_explicit_stop",checkpoint_explicit_stop},
        {"checkpoint_save_failure",checkpoint_save_failure},
        {"checkpoint_reopen",checkpoint_reopen},{"checkpoint_edit_before_restore",checkpoint_edit_before_restore},
        {"checkpoint_reentrant_capture",checkpoint_reentrant_capture},
        {"checkpoint_restore_retry",checkpoint_restore_retry},
        {"checkpoint_format_and_duplicate",checkpoint_format_and_duplicate},
        {"checkpoint_init_abort",checkpoint_init_abort},
        {"checkpoint_notify_saving",checkpoint_notify_saving},
        {"checkpoint_notify_retry",checkpoint_notify_retry},
        {"checkpoint_notify_progress",checkpoint_notify_progress},
        {"checkpoint_notify_revision",checkpoint_notify_revision},
        {"runtime_global_objects",runtime_global_objects},
        {"settings_reset",settings_reset},{"settings_during_write",settings_during_write}};
    bool found=false;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) if(!strcmp(argv[1],cases[i].name)) {cases[i].run();found=true;break;}
    CHECK(found && !irq_depth && !critical_depth && !context && !allocations && !global_scope);
    for(unsigned i=1;i<16;++i) CHECK(!refs[i]);
    CHECK(!sleep_holds);
    printf("PASS %s\n",argv[1]); return 0;
}
