#include "activity_fixture.h"
#define EPD_APP_PSRAM 1
#define READER_ERROR_LAYOUT_MEMORY 98
#define READER_ERROR_NONE 0
#define rt_kprintf(...) ((void)0)
static epd_activity_work_t *allocation_activity;
static unsigned font_retries;
static void epd_app_font_retry_memory(const void *font) { assert(font);++font_retries; }
static void view_activity_wake(void *unused) { (void)unused; }
static epd_activity_work_t *view_activity_new(void) { return epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE,0,65536,0,view_activity_wake,NULL); }
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int reader_work_t;
typedef void document_t;
typedef void lv_font_t;
typedef unsigned reader_error_t;
typedef unsigned rt_tick_t;
#define READER_WORK_TARGET 2
#define READER_WORK_IDLE 0
#define INTENT_NONE 0
#define INTENT_CANCEL 1
#define INTENT_REFLOW 8
#define RT_THREAD_PRIORITY_MAX 32
#define RT_WAITING_FOREVER ((unsigned)-1)
typedef struct view_state view_state_t;
typedef struct { view_state_t *owner; unsigned pin_run,pin_byte; } view_frame_t;
static view_frame_t *render_frame;
typedef struct { int kind; view_frame_t *origin; unsigned session; } view_command_t;
typedef struct { unsigned record,byte; } document_cursor_t;
typedef struct { document_cursor_t start;void *read_request;bool collect; } view_page_t;
typedef struct { bool ready,indexing;unsigned pages,page,percent; } reader_view_t;
typedef struct view_state { unsigned request_failure,request_failure_intent; void *fonts[3],*oblique[3]; uint32_t input_order;bool input_enabled; struct view_state *previous,*following; bool work_exhausted,init_attempted,init_blocked,initialized,selected,displayed,parked,target_pending,background_paused,complete; int direction; unsigned session,intent,failure,display_layout;
view_page_t *display,*index,*building,*prefetch,*compute_page;view_frame_t *rendering;void *back_request;bool frame_published,display_pending,numeric_target;char link_target[2];unsigned published_intent,navigation_link,navigation_toc; reader_view_t status; void *document,*compute_spare; } view_state_t;
static struct { view_state_t *counter;bool failed; } pagination;
static void pagination_reset(void) { pagination.counter=NULL; }
static void pagination_step(view_state_t *current) { (void)current; }
static void pagination_prepare(view_state_t *current) { (void)current; }
static void pagination_snapshot(view_state_t *context,reader_view_t *info) { (void)context;(void)info; }
static struct { epd_activity_work_t *activity; bool stop,replace,paused,suspended,indexing,rejected; unsigned intent,session,result_intent,error,pages,page,percent,layout; void *document; document_cursor_t cursor; int work,wake,stopped; view_state_t *incoming,*configuration; view_command_t command; view_frame_t *ack,*cleanup; } mailbox;
static view_state_t *state, *primary_state, *parked_state;
#define view (*state)
static void worker_nearby_inputs(view_state_t *current,uint64_t parent) { (void)current;(void)parent; }
static unsigned locks,ticks,steps,renders,retired,acknowledged,inits,configurations;
static bool init_fails,work_pending,stopped;
static bool near_wait, ahead_wait;
static bool preview_ahead_pending(void) { return ahead_wait; }
static unsigned neighbor_prepares,neighbor_steps;
static unsigned mode,record_cancels,images_cancelled,prefetch_disabled,commands_applied,idle_calls;
static view_state_t *fixture_current;
static view_state_t cold_destination;
static view_page_t resumed_display;
static view_frame_t rendering_frame;
static view_frame_t ack_frame;
static void rt_enter_critical(void) { ++locks; }
static void rt_exit_critical(void) { assert(locks); --locks; }
static unsigned rt_tick_from_millisecond(unsigned x) { return x; }
static void rt_sem_take(int sem,unsigned timeout)
{
    (void)sem;assert(!locks);++ticks;
    if(mode==12) {
        if(ticks<=2) {
            assert(timeout==20 && !steps && !renders && !commands_applied);
            assert(inits==ticks && fixture_current->selected && !fixture_current->initialized && !fixture_current->init_attempted);
            assert(!mailbox.error && !fixture_current->display);
            if(ticks==1) { mailbox.command.kind=INTENT_CANCEL;mailbox.command.origin=&ack_frame; }
            else assert(mailbox.command.kind==INTENT_CANCEL && mailbox.command.origin==&ack_frame);
        } else if(ticks==3) {
            assert(inits==3 && steps==1 && renders==1 && commands_applied==1);
            assert(mailbox.command.kind==INTENT_NONE && !mailbox.command.origin);
        } else {
            assert(ticks==4 && inits==3 && steps==2 && renders==2 && commands_applied==1);
            assert(mailbox.command.kind==INTENT_NONE && !mailbox.command.origin);mailbox.stop=true;
        }
        return;
    }
    if(mode==13 || mode==14) {
        assert(state==&cold_destination && primary_state==state);
        if(ticks<=2) {
            assert(timeout==20 && !cold_destination.initialized && !cold_destination.display && !renders);
            assert(steps==(mode==14?1u:0u) && inits==ticks-1 && !mailbox.error);
        } else {
            assert(ticks==3 && cold_destination.initialized && cold_destination.display==&resumed_display);
            assert(inits==2 && steps==(mode==14?2u:1u) && renders==1);mailbox.stop=true;
        }
        return;
    }
    if(mode==11) {
        assert(!neighbor_prepares && !neighbor_steps && !pagination.counter);
        mailbox.stop=true;return;
    }
    if(mode==9) {
        assert(mailbox.paused && !mailbox.suspended && configurations==1 && commands_applied==1);
        assert(steps==1 && renders==1 && mailbox.command.kind==INTENT_NONE);
        mailbox.stop=true;return;
    }
    if(mode==6) {
        assert(steps==1 && renders==1 && commands_applied==1);
        mailbox.stop=true;return;
    }
    if(mode==7) { if(ticks==6)mailbox.stop=true;return; }
    if(mode==8) { assert(timeout==1 && !steps && renders==1);mailbox.stop=true;return; }
    if(mode==4) {
        assert(timeout==4 && !neighbor_prepares && !neighbor_steps);
        if(ticks<=2) assert(!steps && renders==ticks && fixture_current->rendering);
        if(ticks==3) { assert(steps==1 && renders==4 && !fixture_current->rendering);mailbox.stop=true; }
        return;
    }
    if(mode==5) {
        /* A new foreground target must resolve before adopting a preparation. */
        assert(steps==1 && renders==1 && commands_applied==1 && timeout==4);
        mailbox.stop=true;return;
    }
    if(mode==10) {
        assert(timeout==4 || ticks==3);
        if(ticks<=2) assert(!neighbor_prepares && !neighbor_steps);
        if(ticks==2) ahead_wait=false;
        if(ticks==3) { assert(neighbor_prepares==1 && neighbor_steps==1); mailbox.stop=true; }
        return;
    }
    if(mode==2) {
        if(ticks<=2) assert(!neighbor_prepares && !neighbor_steps);
        if(ticks==2) near_wait=false;
        if(ticks==3) { assert(neighbor_prepares==1 && neighbor_steps==1);mailbox.stop=true; }
        return;
    }
    if(mode==3) { assert(timeout==1 && render_frame);mailbox.stop=true;return; }
    if(mode==1) {
        if(ticks==1) { mailbox.suspended=true; fixture_current->rendering=&rendering_frame;render_frame=&rendering_frame; }
        if(ticks==2) {
            assert(timeout==RT_WAITING_FOREVER && steps==1 && renders==1 && !render_frame && record_cancels==4);
            assert(images_cancelled && prefetch_disabled && idle_calls && !fixture_current->compute_page);
            mailbox.command.kind=INTENT_CANCEL;
        }
        if(ticks==3) { assert(timeout==RT_WAITING_FOREVER && steps==1 && mailbox.command.kind==INTENT_CANCEL);mailbox.suspended=false; }
        if(ticks==4) { assert(steps==2 && commands_applied==1);mailbox.stop=true; }
        return;
    }
    if(ticks==2) { assert(renders>=2 || init_fails); mailbox.ack=&ack_frame; mailbox.paused=true; }
    if(ticks==3) { assert(acknowledged==1 || init_fails);assert(timeout==RT_WAITING_FOREVER); mailbox.paused=false; }
    if(ticks==5) mailbox.stop=true;
}
static void rt_sem_release(int sem) { (void)sem;stopped=true; }
static void *rt_thread_self(void) { return NULL; }
static void rt_thread_suspend(void *t) { (void)t;assert(locks); }
static void rt_schedule(void) { assert(!locks && stopped); }
static void cluster_retire(view_state_t *s) { assert(!locks);if(s)++retired; }
static void state_retire(view_state_t *s) { assert(!locks);if(s)++retired; }
static void core_initialize(void) {
    assert(!locks && view.selected && view.intent==mailbox.intent);++inits;
    if(mode>=12 && mode<=14) {
        assert(!view.display && !view.initialized && !view.init_attempted);
        if(inits<(mode==12?3u:2u))return;
        view.display=&resumed_display;
    }
    view.init_attempted=true;view.initialized=!init_fails;view.failure=init_fails?99:0;
}
static void state_reflow(view_state_t *s) { (void)s;assert(!locks);if(mode==9)assert(commands_applied==1);++configurations; }
static void frame_accept(view_frame_t *f) { assert(!locks);if(f) { ++acknowledged;view.status.ready=true;work_pending=false; } }
static void command_apply(view_command_t c,view_state_t **p) { (void)p;assert(!locks);if(c.kind)++commands_applied; }
static void core_background_pause(bool p) { view.background_paused=p; }
static void frame_release(view_frame_t *f) { assert(!locks);if(f==render_frame)render_frame=NULL; }
static bool index_client_cleanup(bool closing) { (void)closing;assert(!locks);return false; }
static bool render_cleanup(void) { assert(!locks);return false; }
static void target_resolve(void) { assert(!locks);if(view.initialized)view.target_pending=false; }
static void navigation_resolve(view_state_t **current)
{
    assert(*current==state && !view.navigation_link && !view.navigation_toc);
    if(mode==13 && !ticks) *current=state=primary_state=&cold_destination;
}
static void turn_continue(view_state_t **current) {
    assert(!locks);
    if(mode==14 && !ticks) *current=state=primary_state=&cold_destination;
}
static void neighbor_step(void) { assert(!locks && !near_wait);++neighbor_steps; }
static reader_work_t core_step(unsigned budget,bool foreground,bool commit)
{
    assert(!locks && budget && commit && view.initialized && view.display);
    assert(foreground==(allocation_activity->kind!=EPD_RESOURCE_OPTIONAL));
    assert(allocation_activity->kind==EPD_RESOURCE_NEARBY || !foreground || view.background_paused);++steps;
    if(mode==7) {
        view.status.pages=steps==1?10:steps==2?11:12;
        view.status.indexing=steps<3;
        if(steps>=5)view.failure=8;
    }
    return READER_WORK_TARGET;
}
static void frame_prepare(void)
{
    assert(!locks && view.initialized && view.display);++renders;
    if (!work_pending && !view.rendering && !render_frame) {
        view.frame_published=true;view.published_intent=view.intent;
    }
    if(mode==4) {
        if(renders<=3) assert(!steps);
        if(renders==3) view.rendering=NULL;
    }
    if(mode==5) assert(steps==1);
    if(mode==6) assert(steps==1);
    if(mode==8) { assert(!steps);view.rendering->pin_byte+=32; }
}
static bool core_waiting(void) { return work_pending; }
static bool near_frames_pending(void) { return near_wait; }
static bool window_work_allowed(void) { return !view.failure && !view.init_blocked; }
static bool page_work_allowed(const view_page_t *page) { return page != NULL; }
static void *document_adjacent(void *doc,int direction) { (void)doc;(void)direction;assert(!locks);return NULL; }
static view_state_t *state_neighbor(void *doc,bool backward) { (void)doc;(void)backward;return NULL; }
static void neighbors_prepare(view_state_t *current) { (void)current;assert(!locks && !near_wait);++neighbor_prepares; }
static void font_priority(bool foreground) { (void)foreground;assert(!locks); }
static void images_cancel_pending(void) { assert(!locks);++images_cancelled; }
static void document_record_cancel(void *request) { assert(!locks);if(request)++record_cancels; }
static unsigned prefetch_enabled;
static bool prefetch_gate;
static void document_prefetch_enable(void *doc,bool enable)
{
    (void)doc;assert(!locks);prefetch_gate=enable;
    if(enable)++prefetch_enabled;else ++prefetch_disabled;
}
static void compute_cancel(void) { assert(!locks);view.compute_page=NULL; }
static void page_reset(view_page_t *page,document_cursor_t start) { page->start=start; }
static void reader_compute_idle(void) { assert(!locks);++idle_calls; }
static void worker_trace_wait(bool cleanup,bool paused,bool suspended)
{ (void)cleanup;(void)paused;(void)suspended;assert(!locks); }
static unsigned ui_wakes;
static void epd_reader_ui_wake(void) { assert(!locks); ++ui_wakes; }
/* This fixture admits only the current chapter and its immediate neighbors. */
static unsigned window_contexts(view_state_t *current, view_state_t **out)
{
    unsigned count = 0;
    if (current) out[count++] = current;
    if (current && current->previous) out[count++] = current->previous;
    if (current && current->following && current->following != current->previous)
        out[count++] = current->following;
    return count;
}
static void window_clear(void) {}
static void window_quiesce(view_state_t *current) { (void)current; }
static bool window_prepare(view_state_t *current) { (void)current; return false; }
static bool window_conversion_ready(view_state_t *current) { (void)current;return mode!=11; }
static document_t *window_conversion_document(view_state_t *current) { (void)current;return NULL; }
static bool window_extra(const view_state_t *owner) { (void)owner; return false; }
static bool window_runnable(view_state_t *owner, view_state_t *current) { (void)owner;(void)current;return false; }
static void reader_compute_release(void *job) { assert(!job); }
static unsigned resource_publishes,resource_steps,resource_closes;
static int view_resource;
static void view_resource_publish(void) { assert(!locks);++resource_publishes; }
static void view_resource_step(void) { assert(!locks);++resource_steps; }
static void epd_resource_cache_close(int *bank) { assert(bank==&view_resource && !locks);++resource_closes; }
/* PRODUCTION */
int main(void)
{
    view_state_t current={0};
view_page_t display={0};
current.display=(void*)&display; ack_frame.owner=&current; current.session=7;mailbox.session=7;mailbox.intent=10;mailbox.incoming=&current;mailbox.replace=true;
    work_pending=true;mailbox.activity=view_activity_new();view_worker(NULL);
    assert(steps==4 && renders==4 && inits==1 && retired==1 && stopped && !locks && !state);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=(void*)&display;
    ticks=steps=renders=retired=acknowledged=inits=0;stopped=false;init_fails=true;
    current.session=7;mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(!steps && !renders && inits==1 && retired==1 && mailbox.error==99 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    ticks=steps=renders=retired=acknowledged=inits=commands_applied=record_cancels=0;
    stopped=false;init_fails=false;work_pending=true;mode=1;fixture_current=&current;rendering_frame.owner=&current;
    view_page_t index={{50,1},(void*)1,true},building={{60,2},(void*)2,true},prefetch={{70,3},(void*)3,true};
    current.index=&index;current.building=&building;current.prefetch=&prefetch;current.back_request=(void*)4;current.compute_page=&index;
    current.session=7;mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && steps==2 && record_cancels==4 && !locks);
    assert(index.start.record==50 && index.start.byte==1 && index.collect);
    state=&current;current.complete=true;current.rendering=NULL;current.background_paused=false;work_pending=false;render_frame=NULL;
    assert(worker_idle_delay(&current)==100);
    ahead_wait=true;assert(worker_idle_delay(&current)==4);ahead_wait=false;
    work_pending=true;assert(worker_idle_delay(&current)==4);work_pending=false;
    current.work_exhausted=true;assert(worker_idle_delay(&current)==1);
    current.background_paused=true;assert(worker_idle_delay(&current)==100);
    current.background_paused=false;current.work_exhausted=false;
    view_state_t neighbor={0};current.previous=&neighbor;assert(worker_idle_delay(&current)==4);
    neighbor.init_blocked=true;assert(worker_idle_delay(&current)==100);
    neighbor.init_blocked=false;
    neighbor.complete=true;assert(worker_idle_delay(&current)==100);
    current.previous=NULL;
    view_page_t counter_page={.start={900,11},.read_request=(void*)5,.collect=false};
    view_state_t counter={.initialized=true,.index=&counter_page,.compute_page=&counter_page};
    pagination.counter=&counter;unsigned before_cancel=record_cancels;
    assert(worker_idle_delay(&current)==4);
    worker_quiesce(&current,true,false,false);
    assert(record_cancels==before_cancel+1 && !counter.compute_page && !counter_page.collect && counter_page.start.record==900);
    counter.compute_page=&counter_page;counter_page.read_request=(void*)6;work_pending=true;
    worker_quiesce(&current,false,false,false);
    assert(record_cancels==before_cancel+2 && !counter.compute_page && counter.background_paused);
    pagination.counter=NULL;work_pending=false;state=NULL;
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));memset(&neighbor,0,sizeof(neighbor));
    current.display=&display;current.status.ready=true;current.following=&neighbor;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    ticks=neighbor_prepares=neighbor_steps=0;near_wait=true;stopped=false;mode=2;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && neighbor_prepares==1 && neighbor_steps==1 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    rendering_frame.owner=&current;render_frame=&rendering_frame;mode=3;ticks=0;stopped=false;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);render_frame=NULL;
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=current.complete=true;current.rendering=&rendering_frame;
    rendering_frame.owner=&current;fixture_current=&current;
    ticks=steps=renders=neighbor_prepares=neighbor_steps=0;
    work_pending=false;near_wait=true;stopped=false;mode=4;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==3 && steps==1 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=current.complete=true;current.rendering=&rendering_frame;
    rendering_frame.owner=&current;fixture_current=&current;mailbox.command.kind=INTENT_CANCEL;
    ticks=steps=renders=commands_applied=0;work_pending=true;stopped=false;mode=5;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=true;current.rendering=render_frame=&rendering_frame;
    rendering_frame.owner=&current;fixture_current=&current;mailbox.command.kind=INTENT_CANCEL;
    ticks=steps=renders=commands_applied=0;work_pending=true;stopped=false;mode=6;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);render_frame=NULL;
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    ticks=steps=renders=ui_wakes=0;work_pending=false;near_wait=true;stopped=false;mode=7;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==6 && ui_wakes==2 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=true;current.rendering=&rendering_frame;rendering_frame.owner=&current;
    ticks=steps=renders=0;work_pending=false;stopped=false;mode=8;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    view_state_t configuration={0};
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=true;mailbox.paused=true;mailbox.configuration=&configuration;
    mailbox.command.kind=INTENT_REFLOW;ticks=steps=renders=configurations=commands_applied=0;
    work_pending=true;stopped=false;mode=9;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=current.complete=true;
    current.previous=current.following=&neighbor;
    ticks=steps=renders=neighbor_prepares=neighbor_steps=0;
    work_pending=near_wait=false;ahead_wait=true;stopped=false;mode=10;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==3 && !locks && !ahead_wait);
    memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));current.display=&display;
    current.session=mailbox.session=7;mailbox.incoming=&current;mailbox.replace=true;
    current.status.ready=current.complete=true;current.previous=current.following=&neighbor;
    ticks=steps=renders=neighbor_prepares=neighbor_steps=0;stopped=false;mode=11;
    mailbox.activity=view_activity_new();view_worker(NULL);assert(stopped && ticks==1 && !locks);
    puts("PASS a draining conversion skips neighbor preparation, neighbor steps and pagination for the entire worker round");
    for(mode=12;mode<=14;++mode) {
        memset(&mailbox,0,sizeof(mailbox));memset(&current,0,sizeof(current));
        memset(&cold_destination,0,sizeof(cold_destination));
        current.session=cold_destination.session=mailbox.session=7;mailbox.intent=23;
        mailbox.incoming=fixture_current=&current;mailbox.replace=true;
        if(mode!=12) { current.initialized=current.init_attempted=true;current.display=&display; }
        ticks=steps=renders=inits=commands_applied=ui_wakes=0;
        work_pending=true;near_wait=ahead_wait=false;stopped=false;init_fails=false;
        ack_frame.owner=&current;mailbox.activity=view_activity_new();view_worker(NULL);
        assert(stopped && !locks && !mailbox.error && !activity_tasks);
    }
    puts("PASS pending initialization waits 20ms, preserves commands, initializes selected intent and resumes once");
    puts("PASS navigation and turn handoffs to uninitialized contexts skip layout/render until initialization resumes");
    memset(&current,0,sizeof(current));state=primary_state=&current;
    current.status.ready=current.complete=true;mode=0;render_frame=NULL;
    work_pending=near_wait=false;unsigned turn=0,grants=prefetch_enabled;
    window_service(&current,&turn);worker_input_commit(&current);assert(prefetch_gate && prefetch_enabled==grants+1);
    near_wait=true;document_flow();worker_input_commit(&current);assert(!prefetch_gate);
    near_wait=false;work_pending=true;document_flow();worker_input_commit(&current);assert(!prefetch_gate);
    work_pending=false;current.background_paused=true;document_flow();worker_input_commit(&current);assert(!prefetch_gate);
    current.background_paused=false;current.link_target[0]='x';document_flow();worker_input_commit(&current);assert(!prefetch_gate);
    current.link_target[0]=0;window_service(&current,&turn);worker_input_commit(&current);assert(prefetch_gate);
    puts("PASS production optional NAV gate reopens after settled work and stays closed for near pages, foreground work, pause and link targets");
    puts("PASS fourth-layout preparation yields every 4ms and releases neighbor admission when complete or blocked");
    puts("PASS paused reflow restores the origin before configuration, consumes its command once, and advances target rendering");
    puts("PASS adopted renderer resolves the new target before drawing; exhausted slices wait 1ms, pending providers retain fallback");
    puts("PASS final pagination and error changes wake UI once; intermediate and unchanged snapshots stay quiet");
    puts("PASS optional resource preparation finishes before further layout; new foreground target keeps layout priority");
    puts("PASS near frames gate neighbor preparation and layout; ready gate resumes both; active renderer yields 1ms");
    puts("PASS counting page pauses and yields to a newer target without discarding completed chapter totals");
    puts("PASS finished three-context work backs off to 100ms; a new target or unfinished neighbor restores active cadence");
    puts("PASS paused idle uses event wait; full suspension cancels pending records/render, freezes cold target, preserves intent, resumes and closes");
    puts("PASS independent worker advances without UI polling, consumes bind ACK, retires after stop, reports initialization error once");
    assert(resource_publishes && resource_steps && resource_closes);
    return 0;
}
