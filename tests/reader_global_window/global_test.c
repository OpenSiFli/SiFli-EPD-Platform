#define FIXTURE_EXTERNAL_CAPACITY_EPOCH
#include "activity_fixture.h"
static epd_activity_work_t *allocation_activity;
#include "host.inc"
#include "cache_bridge.h"
typedef int epd_app_memory_t;
typedef unsigned reader_error_t;
static epd_resource_cache_t view_resource;
static int view_reserve_result;
/* Admission adapter for this document/layout integration fixture. The shared
 * one-MiB headroom remains unavailable to speculative page allocations. */
int epd_resource_context_reserve(epd_memory_region_t region,size_t bytes)
{
    assert(region==EPD_MEMORY_REGION_HCPU_PSRAM || region==EPD_MEMORY_REGION_SRAM);
    size_t available=epd_app_psram_available(),headroom=1024u*1024u;
    return bytes<=available && headroom<=available-bytes ? EPD_RESOURCE_OK : EPD_RESOURCE_DENIED;
}
uint32_t epd_memory_capacity_epoch(void)
{
    static size_t previous,capacity;static uint32_t epoch=1;
    if(previous!=(size_t)allocated_bytes || capacity!=psram_capacity){
        previous=(size_t)allocated_bytes;capacity=psram_capacity;++epoch;
    }
    return epoch;
}
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *r)
{r->state=EPD_MEMORY_RECOVERY_DENIED;return r->state;}
static epd_memory_recovery_result_t view_memory_recover(epd_memory_recovery_t *r,size_t peak,bool failed)
{(void)failed;r->used=true;r->physical=peak;r->state=EPD_MEMORY_RECOVERY_DENIED;return r->state;}
#include "memory_room.inc"
/* The document host persists its cache to files and owns no RAM cache tier. */
size_t document_cache_memory_bytes(void) { return 0; }
size_t document_cache_memory_reclaim(size_t requested)
{ (void)requested;assert(!irq_depth && !storage_depth);return 0; }
#define UI_SETTING_FONT_SIZE 0
static unsigned ui_settings_index(unsigned setting) { assert(setting==UI_SETTING_FONT_SIZE);return 0; }
#include "flow.inc"
#include <stdarg.h>
static unsigned reclaim_notes;
static void focused_log(const char *format,...) {
    if(strncmp(format,"reader frame_reclaim ",21))return;
    char line[512];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
    assert(strstr(line,"victim_d=") && strstr(line,"request_d=") && strstr(line,"refs="));
    ++reclaim_notes;fputs(line,stdout);
}
#undef rt_kprintf
#define rt_kprintf focused_log
#include "near_trace.inc"
#include "reader_page_cache.h"

#define VIEW_RECLAIM_TRACE 1
#define VIEW_CACHE_BUDGET (2u*1024u*1024u)
#define VIEW_CACHE_RESERVE (1024u*1024u)
#define VIEW_FRAME_BUDGET (6u*1024u*1024u)
#define VIEW_NEIGHBOR_PRIORITY 4096u
#define VIEW_INDEX_LIMIT (512u*1024u)
#define VIEW_INDEX_GLOBAL (3u*VIEW_INDEX_LIMIT)
#define READER_PAGE_MAX ((unsigned)INT_MAX-1u)
typedef union {size_t bytes;uint64_t alignment;} cache_allocation_t;
typedef struct {uint32_t magic,version,layout,end,count,toc_count,toc_scanned,complete,checksum;} view_index_header_t;
#define LV_COLOR_FORMAT_L8 6
typedef struct view_frame view_frame_t;
struct view_frame {
    struct {void *first;} cold;
    bool memory_reclaimed,allocation_attempted;
    const char *admission_stage;int admission_result;size_t admission_need;
    view_frame_t *next,*backing; view_state_t *owner;
    unsigned references,number,intent,layout,generation,memory_ticket,handoff_intent,pin_run,pin_byte;
    bool complete,required,cancelled,drop_reported,resources_ready;void *renderer,*pins,*render_image;
    epd_compute_buffer_t pixels;epd_reader_page_t page;
    struct {struct {unsigned magic,cf,w,h,stride;}header;size_t data_size;} image;
    size_t window_charge;rt_tick_t preparation_started,completed_at;
};
static size_t layout_bytes,frame_bytes,index_bytes;
static view_state_t *parked_state;
static struct {view_state_t *counter;bool failed;} pagination;
static view_frame_t *render_frame;
static struct {view_frame_t *cleanup;view_state_t *retired;} mailbox;
static bool data_retiring;
static void state_dispose(view_state_t *owner);
static bool core_initialize(void);
static void state_retire(view_state_t *owner);
static view_state_t *state_neighbor(document_t *document,bool backward);
static bool neighbor_step(void);
static bool frame_reclaim_layout(size_t bytes,unsigned priority);
static bool window_reclaim(unsigned priority);
static bool window_admit(size_t bytes,unsigned priority);
static unsigned frame_priority(const view_state_t *owner,unsigned number);
static void frame_release(view_frame_t *frame);
static void *view_allocate(size_t,epd_app_memory_t);
static bool frame_admission_error(view_frame_t *,const char *,int,size_t);
static view_frame_t **frame_pixel_victim(const view_frame_t *incoming, bool matching);
static unsigned frame_retention_distance(unsigned priority);
/* Pixel-spare reuse has a dedicated fixture. This window fixture releases
 * every unreferenced frame so final per-context accounting stays observable. */
static bool frame_spare_keep(view_frame_t *frame) { (void)frame;return false; }
static bool frame_spare_take(view_frame_t *frame) { (void)frame;return false; }
static bool frame_spare_available(const view_frame_t *frame) { (void)frame;return false; }
static void frame_spare_trim(const view_frame_t *frame) { (void)frame; }
static void frame_cold_clear(view_frame_t *f) {assert(!f->cold.first);}
static view_frame_t *frame_cold_candidate(view_frame_t *f) {(void)f;return NULL;}
static bool frame_nearby_growth(const view_frame_t *f) {(void)f;return false;}
static void frame_cold_maintain(void) {}
static bool frame_pixels_ready(const view_frame_t *f) {return f&&f->complete&&f->pixels.data;}
static int frame_materialize(view_frame_t *f,bool required) {(void)required;assert(!f->cold.first);return frame_pixels_ready(f)?1:-1;}
static int frame_prewarm(view_frame_t *f) {assert(!f->cold.first);return frame_pixels_ready(f)?1:-1;}
static void frame_admission_report(const view_frame_t *f,int result)
{fprintf(stderr,"FRAME_ADMISSION page=%u stage=%s raw=%d result=%d need=%zu available=%zu\n",
    f->number,f->admission_stage?f->admission_stage:"none",f->admission_result,result,f->admission_need,epd_app_psram_available());}
static void image_release(void *image) { assert(!image); }
static void frame_accept(view_frame_t *frame);
static bool optional_pixels_ready_for(unsigned number);
static bool window_pixels_pending(view_state_t *owner);
static view_frame_t *frame_find(document_cursor_t cursor);
static void epd_app_font_page_release(void *pins) { assert(!pins); }
static void fail(unsigned code) { view.failure=code; }
static void frame_publish(view_frame_t *frame) { assert(frame && frame->pixels.data); }
#include "window_helpers.inc"
#include "compute_acquire.inc"
#include "window_frames.inc"
static bool cursor_at_segment_start(document_cursor_t cursor) { return window_start(state,cursor); }

static unsigned renders,created,disposed;
static int frame_render(view_frame_t *frame,bool required)
{
    (void)required;
    if(!frame_reserve(frame))return -3;
    if(!epd_compute_buffer_alloc(&frame->pixels,frame->image.data_size))return -3;
    frame_bytes+=frame->pixels.capacity;
    memset(frame->pixels.data,(int)(frame->owner->progress.chapter+1),frame->pixels.capacity);
    frame->complete=true;frame->completed_at=rt_tick_get();++renders;return 1;
}
static void layout_accept(void)
{
    view_page_t *old=view.display;view.display=view.building;view.building=old;
    view.status.ready=true;view.display_pending=false;view.display_layout=view.layout;
    assert(reader_page_cache_put(&view.cache,window_number(state),view.layout,(epd_reader_page_t*)view.display));
    page_reset(view.building,(document_cursor_t){UINT32_MAX,0});
}
static void frame_accept(view_frame_t *frame) { assert(frame);if(view.display_pending)layout_accept(); }
#include "window_prepare_frames.inc"
#include "window_ownership.inc"

static void dispose(view_state_t *owner)
{
    state_dispose(owner);++disposed;
}
static void collect(void)
{
    view_state_t **at=&mailbox.retired;
    while(*at) {
        view_state_t *owner=*at;
        if(owner->references){at=&owner->next;continue;}
        *at=owner->next;dispose(owner);
    }
    document_service_process();
}
static void images_free(void *images) { assert(!images); }
static void images_cancel_pending(void) {}
static void reader_compute_idle(void) {}
static void document_font_free(lv_font_t *font) { assert(!font); }
static void epd_app_font_destroy(lv_font_t *font) { assert(!font); }
#define compute_cancel flow_compute_cancel
#include "lifecycle.inc"
#undef compute_cancel
#include "reclaim.inc"
#include "initialize.inc"
static view_state_t *state_neighbor(document_t *document,bool backward)
{
    if(!document)return NULL;
    view_state_t *owner=epd_app_alloc(sizeof(*owner),EPD_APP_PSRAM);assert(owner);memset(owner,0,sizeof(*owner));
    owner->document=document;owner->backward_target=backward;
    owner->session=view.session;owner->intent=view.intent;owner->layout=view.layout;
    owner->status.content_width=684;owner->status.bottom=1010;
    for(unsigned i=0;i<3;++i)owner->metrics[i].line_height=owner->italic_metrics[i].line_height=16;
    ++created;return owner;
}
#define VIEW_PREVIEW_ORIGIN 1000000u
#define VIEW_BACK_WINDOW_MIN 4096u
#define VIEW_BACK_WINDOW_MAX 16384u
#define VIEW_BACKGROUND_TRACE 0
static void target_finish(void) { view.display_pending=true;view.notified=false; }
static void core_seek(unsigned page) { (void)page;assert(!"unexpected legacy history seek in window fixture"); }
#define compute_cancel flow_compute_cancel
#include "backward.inc"
#undef compute_cancel
static void font_priority(bool foreground) { assert(!foreground); }
/* Drive the production reverse scan and page step one bounded operation at a time. */
#define READER_WORK_IDLE 0
#define READER_WORK_BACKGROUND 1
#define READER_WORK_TARGET 2
static int core_step(unsigned milliseconds,bool foreground,bool commit)
{
    assert(milliseconds==2 && !commit);view.compute_foreground=foreground;
    document_progress(view.document,&view.progress);
    if(view.turn_pending<0 || view.preview_back_scanning){
        if(!view.preview_back_scanning && !preview_back_begin())return READER_WORK_IDLE;
        if(view.preview_back_seeking){preview_back_seek_step();return READER_WORK_TARGET;}
        if(page_runnable(view.prefetch)||view.compute_job){
            assert(page_step(view.prefetch,false,NULL,&view.progress)>=0);
            if(view.prefetch->done)preview_back_finish();
        }
        return READER_WORK_TARGET;
    }
    if(!view.status.ready && !view.display_pending && (page_runnable(view.building)||view.compute_job)){
        assert(page_step(view.building,false,NULL,&view.progress)>=0);
        if(view.building->done)target_finish();
        return READER_WORK_TARGET;
    }
    /* core_step now returns remaining runnable work, not just whether this
     * slice changed the target. The fixture leaves index work to later slices. */
    return !view.complete && (view.index->done || page_runnable(view.index)) ?
        READER_WORK_BACKGROUND : READER_WORK_IDLE;
}
static bool preview_ahead_pending(void) { return false; }
#include "idle.inc"
#include "neighbor_step.inc"
static bool exact_neighbor_entry;
static bool neighbor_step(void)
{
    /* Reverse entry exercises a fully supplied previous chapter. */
    if(!exact_neighbor_entry && view.backward_target && !view.status.ready){
        document_progress(view.document,&view.progress);
        if(!view.progress.complete){document_prefetch_enable(view.document,true);document_target_require(view.document,true);return false;}
    }
    return production_neighbor_step();
}
static unsigned optional_counter_steps;
static void pagination_prepare(view_state_t *current) { assert(current==primary_state); }
static void pagination_step(view_state_t *current) { assert(current==primary_state);++optional_counter_steps; }
#include "window_service.inc"
static void retention_cases(void)
{
    view_state_t owner={0}; owner.layout=1; owner.wanted=100; owner.session=71; owner.intent=1;
    view_state_t *saved=state,*primary=primary_state;
    state=primary_state=&owner;
    for(int direction=-1;direction<=1;direction+=2) for(unsigned preview=0;preview<2;++preview) {
        owner.direction=direction;owner.preview_cache_active=preview!=0;owner.preview_page=0x40000008u;
        unsigned current=window_number(&owner);
        epd_reader_page_t page={0};page.start.record=62724;page.start.byte=53;page.cursor.record=65556;
        view_frame_t *previous=frame_create(&page,current-direction,1);assert(previous);
        previous->complete=true;previous->image.data_size=690680;
        assert(epd_compute_buffer_alloc(&previous->pixels,690680));frame_bytes+=previous->pixels.capacity;
        owner.frames=previous;
        view_frame_t *incoming=frame_create(&page,current+3*direction,1);assert(incoming);
        incoming->image=previous->image;
        size_t capacity=psram_capacity;psram_capacity=(size_t)allocated_bytes+VIEW_CACHE_RESERVE;
        assert(frame_priority(&owner,current-direction)==4);
        assert(!frame_reserve(incoming));assert(owner.frames==previous && previous->complete);
        assert(!frame_reclaim_layout(1,3));assert(owner.frames==previous);
        assert(!frame_pixel_victim(incoming,false) && !frame_pixel_victim(incoming,true));
        assert(!frame_reuse_pixels(incoming) && !incoming->pixels.data);
        /* A cache-only current frame is protected even without a UI reference. */
        previous->number=current;
        assert(!frame_reserve(incoming) && !frame_reclaim_layout(1,3));
        assert(!frame_pixel_victim(incoming,false));
        /* Both sides of the three-page window retain their complete pixels. */
        void *protected_pixels=previous->pixels.data;
        for(int distance=-3;distance<=3;++distance) {
            previous->number=current+distance*direction;
            assert(!frame_pixel_victim(incoming,false) && !frame_pixel_victim(incoming,true));
            assert(!frame_reuse_pixels(incoming) && !incoming->pixels.data);
            assert(owner.frames==previous && previous->pixels.data==protected_pixels && previous->references==1);
            assert(previous->complete && !previous->cancelled && owner.references==2);
        }
        previous->number=current-3*direction;
        incoming->number=previous->number;
        assert(!frame_reserve(incoming) && !frame_reclaim_layout(1,frame_distance(incoming)));
        assert(!frame_pixel_victim(incoming,false));
        incoming->number=current+3*direction;
        /* Truly farther frames remain usable for both release and donation. */
        previous->number=current-4*direction;
        previous->references=2;assert(!frame_reserve(incoming));assert(!frame_pixel_victim(incoming,false));
        previous->references=1;
        previous->renderer=(void *)1;
        assert(!frame_pixel_victim(incoming,false) && !frame_pixel_victim(incoming,true));
        previous->renderer=NULL;previous->backing=incoming;
        assert(!frame_pixel_victim(incoming,false) && !frame_pixel_victim(incoming,true));
        previous->backing=NULL;
        assert(frame_pixel_victim(incoming,true)==&owner.frames);
        unsigned notes=reclaim_notes;
        assert(frame_reuse_pixels(incoming));assert(!owner.frames && incoming->pixels.data && reclaim_notes==notes+1);
        for(unsigned i=0;i<1000;++i)frame_reclaim_note(incoming,&owner,current,0,"suppressed");
        assert(reclaim_notes==notes+1);
        assert(incoming->pixels.data==protected_pixels && owner.references==1 && !incoming->complete);
        incoming->cancelled=true;
        psram_capacity=capacity;frame_release(incoming);
        assert(!frame_bytes && !owner.references);
        ++owner.intent;
    }
    assert(frame_retention_distance(1)==1 && frame_retention_distance(4)==1);
    assert(frame_retention_distance(3)==3 && frame_retention_distance(6)==3);
    assert(frame_retention_distance(8)==4 && frame_retention_distance(9)==4);
    assert(frame_retention_distance(4096+4)==4097 && frame_retention_distance(UINT_MAX)==UINT_MAX);
    state=saved;primary_state=primary;
    puts("PASS retention: both directions and preview ordinals, three reclaim selectors, current/near/equal/held protection, farther donation and complete cleanup");
}

static void wire_fixture(void)
{
    picture_event(BOOK_DOCUMENT_CHAPTER,0,10,1,0,NULL,0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,0,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_BEGIN,NULL,0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,0,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_TARGET,NULL,0);
    picture_event(BOOK_DOCUMENT_TEXT,0,0,0,0,"one short page",14);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,14,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_END,NULL,0);
}
static void pump_owner(view_state_t *owner)
{
    for(unsigned i=0;i<256 && (!owner->status.ready||!owner->frames);++i){
        state=owner;neighbor_step();worker_input_commit(primary_state);document_worker_step(&service);document_service_process();collect();
    }
    if(!owner->frames && flow_jobs==READER_JOBS)fprintf(stderr,"POOL_STALL jobs=%u\n",flow_jobs);
    if(!owner->frames || !owner->frames->complete || owner->display->bytes!=15)
        fprintf(stderr,"WINDOW_PAGE ready=%u frame=%p complete=%u bytes=%u failure=%u text=%s end=%u cursor=%u\n",
            owner->status.ready,(void*)owner->frames,owner->frames?owner->frames->complete:0,
            owner->display->bytes,owner->failure,owner->display->text,owner->progress.end,owner->display->cursor.record);
    assert(owner->frames && owner->frames->complete && owner->display->bytes==15 && (owner->display->eof || (owner->preview_clipped && owner->progress.complete && owner->display->cursor.record==owner->progress.end)));
    document_progress(owner->document,&owner->progress);
    char expected[15];snprintf(expected,sizeof(expected),"chapter-%06u",owner->progress.chapter);
    assert(!strcmp(owner->display->text,expected));
}
static void txt_entry_cases(void)
{
    view_state_t *next=primary_state->following;
    document_t *doc=next->document;
    document_location_t *tail=doc->locations_tail;
    document_progress_t progress=next->progress;
    unsigned unit=doc->requested.unit,chapters=doc->chapter_count;
    bool target_ready=doc->target_ready;
    uint32_t target_record=doc->target_record,target_byte=doc->target_byte;
    /* Publish a real file-byte location in this document's own location table. */
    document_anchor_t original=doc->source_current;
    doc->source_current=(document_anchor_t){1,UINT32_MAX,131599,0};
    assert(location_append(doc,next->display->start.record,0));
    doc->requested.unit=1;doc->chapter_count=0;next->progress.chapters=0;
    next->progress.source_begin=119000;
    doc->target_ready=true;doc->target_record=next->display->start.record;doc->target_byte=next->display->start.byte;
    next->display_layout=next->layout;
    unsigned n=UINT_MAX;
    assert(!window_start(next,next->display->start));
    assert(window_entry(next,1,&n) && n==window_number(next));
    window_route_t route=window_route(1);
    assert(route.count && route.pages[0].owner==next && route.pages[0].number==n);
    assert(frame_priority(next,n)==1);
    state=next;
    while(next->frames){view_frame_t *f=next->frames;next->frames=f->next;frame_release(f);}
    unsigned before=renders;state=primary_state;
    bool prepared=window_prepare(primary_state);
    if(!prepared || !next->frames)fprintf(stderr,"DIRECT_ENTRY_MISSING prepared=%u runnable=%u target=%u pending=%u blocked=%u rank=%u available=%zu frames=%zu\n",
        prepared,window_runnable(next,primary_state),next->target_pending,next->display_pending,next->pixels_memory_blocked,
        window_priority(next,n),epd_app_psram_available(),frame_bytes);
    if(!prepared || !next->frames)fprintf(stderr,"ENTRY_RESULT frame=%p complete=%u number=%u renders_before=%u renders_after=%u\n",
        (void*)next->frames,next->frames?next->frames->complete:0,next->frames?next->frames->number:0,before,renders);
    if(!prepared || !next->frames){state=next;document_cursor_t probe;bool eof;
        fprintf(stderr,"ENTRY_DETAIL failure=%u request=%u font=%u init=%u pixels=%u number=%u known=%u allowed=%u optional=%u rendering=%p\n",
            view.failure,view.request_failure,view.font_memory_blocked,view.init_blocked,view.pixels_blocked,n,
            window_page(next,n,&probe,&eof),window_work_allowed(),optional_pixels_ready_for(n),(void*)view.rendering);
        for(int side=-1;side<=1;side+=2){window_route_t check=window_route(side);
            for(unsigned k=0;k<check.count;++k)fprintf(stderr,"ENTRY_ROUTE side=%d owner=%p next=%p n=%u expected=%u pending=%u\n",side,
                (void*)check.pages[k].owner,(void*)next,check.pages[k].number,n,window_pixels_pending_for(next,side));}
        state=primary_state;}
    assert(prepared && next->frames && next->frames->number==n && next->frames->complete && renders==before+1);
    /* The cached target must remain discoverable when display points elsewhere. */
    document_cursor_t start=next->display->start,end=next->display->cursor;
    assert(reader_page_cache_put(&next->cache,n,next->layout,(epd_reader_page_t*)next->display));
    next->display->start.record+=1;
    assert(window_entry(next,1,&n));
    reader_cached_page_t *cached=reader_page_cache_find(&next->cache,n,next->layout);assert(cached);
    unsigned layout=cached->layout;cached->layout++;
    assert(!window_entry(next,1,&n));
    cached->layout=layout;next->display->start=start;
    /* Cursor-byte and owner mismatches cannot select an arbitrary page. */
    doc->target_byte=start.byte+1;assert(!window_entry(next,1,&n));doc->target_byte=start.byte;
    document_t *other=primary_state->document;next->document=other;
    document_location_t *other_tail=other->locations_tail;
    bool ready=other->target_ready;uint32_t rec=other->target_record,byte=other->target_byte;
    document_anchor_t other_source=other->source_current;
    other->source_current=(document_anchor_t){1,UINT32_MAX,99000,0};
    assert(location_append(other,other->status.end-1,0));
    other->target_ready=true;other->target_record=other->status.end;other->target_byte=0;
    assert(!window_entry(next,1,&n));
    other->target_ready=ready;other->target_record=rec;other->target_byte=byte;other->source_current=other_source;
    while(other_tail->next){document_location_t *n=other_tail->next;other_tail->next=n->next;other->navigation_bytes-=sizeof(*n);epd_app_free(n);}other->locations_tail=other_tail;
    next->document=doc;
    /* Reverse TXT entry uses the exact end cursor, not the slice EOF. */
    doc->target_record=end.record;doc->target_byte=end.byte;
    doc->source_current=(document_anchor_t){1,UINT32_MAX,132407,0};assert(location_append(doc,end.record-1,0));
    assert(window_entry(next,-1,&n));
    next->progress=progress;doc->requested.unit=unit;doc->chapter_count=chapters;
    doc->target_ready=target_ready;doc->target_record=target_record;doc->target_byte=target_byte;doc->source_current=original;
    while(tail->next){document_location_t *n=tail->next;tail->next=n->next;doc->navigation_bytes-=sizeof(*n);epd_app_free(n);}doc->locations_tail=tail;
    puts("PASS TXT overlapping entry: actual target cursor, owner and layout, cached entry, byte mismatch, reverse end and global rank");
}

static void pool_limit(void)
{
    reader_compute_job_t *held[READER_JOBS];int result;
    for(unsigned i=0;i<READER_JOBS;++i){held[i]=reader_compute_create(&result);assert(held[i] && result==EPD_COMPUTE_OK);}
    assert(!reader_compute_create(&result) && result==EPD_COMPUTE_BUSY);
    assert(flow_jobs==READER_JOBS);
    for(unsigned i=0;i<READER_JOBS;++i)reader_compute_release(held[i]);
    assert(!flow_jobs && !allocated_bytes);
    puts("PASS production compute client enforces four live job objects and releases every slot");
}
static void pool_transfer(void)
{
    view_state_t *current=primary_state,parked={0},counter={0};int result;
    parked_state=&parked;pagination.counter=&counter;
    view_state_t *contexts[7];unsigned count=window_contexts(current,contexts);
    for(unsigned i=0;i<count;++i){reader_compute_release(contexts[i]->compute_spare);contexts[i]->compute_spare=NULL;}
    parked.compute_spare=reader_compute_create(&result);
    counter.compute_spare=reader_compute_create(&result);
    reader_compute_job_t *a=parked.compute_spare,*b=counter.compute_spare;
    unsigned live=flow_jobs;size_t memory=(size_t)allocated_bytes;
    state=current;
    reader_compute_job_t *first=compute_acquire(&result),*second=compute_acquire(&result);
    assert(first==a && second==b && !parked.compute_spare && !counter.compute_spare);
    assert(flow_jobs==live && (size_t)allocated_bytes==memory);
    reader_compute_release(first);reader_compute_release(second);
    parked_state=NULL;pagination.counter=NULL;
    assert(!flow_jobs);
    puts("PASS idle jobs transfer from parked and counter contexts without allocation or changing the live-job count");
}
static void window_jobs(void)
{
    assert(!flow_jobs);
    for(unsigned displaced=0;displaced<2;++displaced){
        view_frame_t *frames[4];document_cursor_t starts[4];
        for(unsigned i=0;i<4;++i){
            view_state_t *owner=window_slots[i].owner;assert(owner && owner->frames);
            frames[i]=owner->frames;state=owner;
            page_reset(view.index,(document_cursor_t){0,0});view.index->collect=false;
            for(unsigned step=0;step<128 && !view.compute_job;++step){
                assert(page_step(view.index,true,NULL,&view.progress)>=0);
                document_worker_step(&service);document_service_process();
            }
            assert(view.compute_job && view.compute_job->submitted && view.compute_page==view.index);
            starts[i]=view.index->start;
            view.building->read_request=document_record_request(view.document,view.progress.end,true);
            view.prefetch->read_request=document_record_request(view.document,view.progress.end,true);
            view.back_request=document_record_request(view.document,view.progress.end,true);
            assert(view.building->read_request && view.prefetch->read_request && view.back_request);
        }
        assert(flow_jobs==READER_JOBS && record_requests);
        int result;assert(!reader_compute_create(&result) && result==EPD_COMPUTE_BUSY);
        state=primary_state;
        /* Suppress both routes while retaining every slot and its completed pixels. */
        if(displaced)primary_state->status.ready=false;
        assert(!window_runnable(window_slots[0].owner,primary_state));
        window_quiesce(primary_state);worker_input_commit(primary_state);
        if(flow_jobs)fprintf(stderr,"IDLE_JOBS_RETAINED displaced=%u jobs=%u\n",displaced,flow_jobs);
        assert(flow_jobs==0);
        assert(state==primary_state);
        for(unsigned i=0;i<4;++i){
            view_state_t *owner=window_slots[i].owner;
            assert(!owner->compute_job && !owner->compute_page && !owner->compute_spare);
            assert(!owner->building->read_request && !owner->prefetch->read_request && !owner->back_request);
            assert(owner->frames==frames[i] && frames[i]->complete && frames[i]->pixels.data);
            assert(cursor_equal(owner->index->start,starts[i]) && cursor_equal(owner->index->cursor,starts[i]) && !owner->index->collect);
        }
        primary_state->status.ready=true;collect();assert(!record_requests);
        /* The current chapter can submit and finish a real page after all four slots return. */
        page_reset(view.index,(document_cursor_t){0,0});view.index->collect=false;
        for(unsigned step=0;step<256 && !view.index->done;++step){
            assert(page_step(view.index,true,NULL,&view.progress)>=0);
            document_worker_step(&service);document_service_process();
        }
        assert(view.index->done && view.index->eof && !view.compute_job && view.compute_spare);
        reader_compute_release(view.compute_spare);view.compute_spare=NULL;
        page_reset(view.index,(document_cursor_t){0,0});view.index->collect=false;
        assert(!flow_jobs);
    }
    puts("PASS completed and out-of-route windows cancel four submitted jobs and pending reads, preserve pixels/work-page state, and unblock real current-page work");
}
static void index_growth(void)
{
    state=window_owner(2);assert(view.window_state_charge);
    size_t before_layout=layout_bytes,before_workspace=view.window_workspace_charge,before_index=index_bytes;
    unsigned before_capacity=view.capacity;
    assert(reserve_pages(32768));
    size_t delta=(view.capacity-before_capacity)*sizeof(*view.pages);
    assert(layout_bytes==before_layout+delta && view.window_workspace_charge==before_workspace+delta && index_bytes==before_index+delta);
    before_layout=layout_bytes;before_workspace=view.window_workspace_charge;before_index=index_bytes;before_capacity=view.capacity;
    allocation_failures=1;assert(!reserve_pages(40000));assert(!allocation_failures);
    assert(layout_bytes==before_layout && view.window_workspace_charge==before_workspace && index_bytes==before_index && view.capacity==before_capacity);
    size_t capacity=psram_capacity;psram_capacity=16u*1024u*1024u;
    void *fill=cache_allocate(VIEW_CACHE_BUDGET-layout_bytes);assert(fill);
    assert(layout_bytes==VIEW_CACHE_BUDGET && !reserve_pages(40000));
    assert(view.window_workspace_charge==before_workspace && index_bytes==before_index && view.capacity==before_capacity);
    cache_free(fill);psram_capacity=capacity;assert(layout_bytes==before_layout);
    state=primary_state;
    puts("PASS extra-context index growth charges the exact delta; allocation and layout-budget failures roll back cleanly");
}
static void idle_window(void)
{
    state=primary_state;
    primary_state->complete=primary_state->previous->complete=primary_state->following->complete=true;
    assert(!window_owner(2)->complete && !window_owner(-3)->complete);
    assert(worker_idle_delay(primary_state)==rt_tick_from_millisecond(100));
    view_state_t *optional=window_owner(-3);view_frame_t *frames=optional->frames;
    optional->frames=NULL;
    assert(worker_idle_delay(primary_state)==rt_tick_from_millisecond(4));
    optional->init_blocked=true;optional->init_blocked_intent=primary_state->intent;
    optional->init_blocked_available=epd_app_psram_available();
    optional->init_blocked_epoch=epd_memory_capacity_epoch();
    assert(worker_idle_delay(primary_state)==rt_tick_from_millisecond(100));
    optional->init_blocked=false;optional->frames=frames;
    primary_state->complete=primary_state->previous->complete=primary_state->following->complete=false;
    puts("PASS satisfied optional windows and blocked admission use the long idle delay; runnable missing pixels use the short delay");
}
static void admit_priority(void)
{
    state=primary_state;
    view_frame_t *visible=primary_state->frames,*near=window_owner(2)->frames;
    void *pixels=visible->pixels.data;
    size_t capacity=psram_capacity;psram_capacity=16u*1024u*1024u;
    size_t requested=VIEW_CACHE_BUDGET-layout_bytes+128u*1024u;
    assert(epd_app_psram_available()>requested+VIEW_CACHE_RESERVE);
    assert(window_owner(-2));
    assert(window_admit(requested,2));collect();
    assert(layout_bytes<=VIEW_CACHE_BUDGET-requested && !window_owner(-3) && window_owner(-2));
    assert(primary_state->frames==visible && visible->pixels.data==pixels && window_owner(2)->frames==near);
    psram_capacity=capacity;
    puts("PASS layout-budget admission reclaims a farther slot while physical memory is sufficient and nearer pixels remain bound");
    ++primary_state->intent;
    for(unsigned i=0;i<1000;++i){
        window_prepare(primary_state);worker_input_commit(primary_state);document_worker_step(&service);document_service_process();collect();
        if(window_owner(-3) && window_owner(-3)->frames && window_owner(3) && window_owner(3)->frames)break;
    }
    assert(window_owner(-3) && window_owner(-3)->frames && window_owner(3) && window_owner(3)->frames);
}
static void close_all(void)
{
    view_state_t *root=primary_state;view_frame_t *shown=root->frames;
    frame_ref(shown);void *pixels=shown->pixels.data;root->displayed=true;
    view_state_t *pending=window_owner(3);
    if(!pending)pending=window_owner(-3);
    assert(pending);
    pending->building->read_request=document_record_request(pending->document,pending->progress.end,true);assert(pending->building->read_request);
    state=pending;
    view_frame_t *deferred=frame_create((epd_reader_page_t*)pending->display,window_number(pending),1);
    assert(deferred && deferred->window_charge);
    deferred->renderer=(void*)1;pending->rendering=deferred;state=root;
    cluster_retire(root);collect();
    assert(root->parked && parked_state==root && shown->references==2 && shown->pixels.data==pixels);
    for(unsigned i=0;i<4;++i)assert(!window_slots[i].owner);
    assert(!record_requests);
    assert(mailbox.cleanup==deferred && pending->references==1 && pending->window_state_charge);
    assert(deferred->window_charge && layout_bytes>=deferred->window_charge+pending->window_state_charge);
    /* The transport completion returns ownership before disposal may finish. */
    mailbox.cleanup=NULL;deferred->renderer=NULL;deferred->references=1;frame_release(deferred);collect();
    frame_release(shown);root->parked=root->displayed=root->selected=false;parked_state=NULL;
    state_retire(root);
    state=primary_state=NULL;
    for(unsigned i=0;i<1000 && (mailbox.retired||documents);++i){collect();document_worker_step(&service);document_service_process();}
    assert(!documents && !mailbox.retired && !frame_bytes && !layout_bytes && !index_bytes && !flow_jobs && !allocated_bytes && !fd_live);
    puts("PASS production cluster retirement cancels pending extra input, preserves displayed backing, then releases every slot/document/job after the final display owner exits");
    puts("PASS a retired extra owner and its charged frame stay alive until deferred renderer cleanup returns ownership");
}
static void direct_entry(void)
{
    view_state_t *next=primary_state->following,*opposite=window_owner(-2);
    state=next;frame_cache_clear();assert(next->status.ready && !next->display_pending);
    state=opposite;frame_cache_clear();state=primary_state;
    assert(window_prepare(primary_state));
    if(!next->frames)fprintf(stderr,"DIRECT_ENTRY_MISSING\n");
    assert(next->frames && !opposite->frames);
    assert(next->frames->complete && cursor_equal(next->display->start,
        (document_cursor_t){next->frames->page.start.record,next->frames->page.start.byte}));
    assert(window_prepare(primary_state) && opposite->frames);
    puts("PASS direct entry pixels recover after cancellation before a farther opposite extra");
}
static void near_timing(void)
{
    state=primary_state;view.trace_pending=false;view.trace_layout_ready=true;
    view.compute_foreground=true;hold_glyphs=true;
    page_reset(view.prefetch,view.display->start);view.prefetch_number=window_number(state)+1;
    for(unsigned i=0;i<128 && view.prefetch->wait_kind!=2;++i){
        assert(near_page_step()>=0);assert(!view.trace_pending && view.trace_layout_ready);
        host_ticks+=7;document_worker_step(&service);document_service_process();
    }
    assert(view.near_tracing && view.prefetch->wait_kind==2);
    host_ticks+=19;assert(near_page_step()==0);hold_glyphs=false;host_ticks+=23;
    for(unsigned i=0;i<256 && !view.prefetch->done;++i){
        assert(near_page_step()>=0);assert(!view.trace_pending && view.trace_layout_ready);
        host_ticks+=31;document_worker_step(&service);document_service_process();
    }
    assert(view.prefetch->done && !view.near_tracing);
    assert(view.trace_record_wait && view.trace_glyph_wait>=42 && view.trace_layout_ticks>=31 && view.near_steps);
    page_reset(view.prefetch,view.display->start);
    for(unsigned i=0;i<128 && !view.compute_job;++i){
        assert(near_page_step()>=0);host_ticks+=7;document_worker_step(&service);document_service_process();
    }
    assert(view.compute_job && view.near_tracing);host_ticks+=37;
    near_trace_finish("turn");rt_tick_t waited=view.trace_layout_ticks;
    assert(waited>=37 && !view.near_tracing && !view.compute_traced);
    near_trace_finish("turn");assert(view.trace_layout_ticks==waited);
    flow_compute_cancel();page_reset(view.prefetch,(document_cursor_t){UINT32_MAX,0});view.prefetch_number=UINT_MAX;
    puts("PASS near timing reuses real page_step counters across delayed record/glyph/layout supply, restores foreground flags, flushes a pending batch once on cancellation");
}
static void conversion_admission(bool two_pending)
{
    view_state_t seed={0};seed.session=seed.intent=seed.layout=7;state=&seed;
    bookshelf_item_t file={0};strcpy(file.path,"/flash/serial-window.epub");
    file.size=20000;file.modified=2;file.format=DOCUMENT_EPUB;
    document_anchor_t anchor={BOOK_DOCUMENT_SOURCE_BODY_UTF8,3,0,0};char error[96];
    committed=false;_unlink(persistent_path);
    primary_state=state_neighbor(document_open_at(&file,&anchor,error,sizeof(error)),false);
    primary_state->selected=true;pump_owner(primary_state);state=primary_state;view.selected=true;view.direction=1;
    view.previous=state_neighbor(document_adjacent(view.document,-1),true);
    view_state_t *previous=view.previous;
    if(!two_pending)pump_owner(previous);
    state=primary_state;
    committed=false;_unlink(persistent_path);
    view_state_t *pending=state_neighbor(document_adjacent(view.document,1),false);
    primary_state->following=pending;
    document_prefetch_enable(pending->document,false);
    uint32_t record=0,byte=0;
    document_progress_t progress={0};
    for(unsigned i=0;!two_pending && i<256;++i){
        document_worker_step(&service);document_service_process();
        document_progress(pending->document,&progress);
        if(document_target_cursor(pending->document,&record,&byte)==1 && progress.end>record)break;
    }
    if(!two_pending)assert(progress.end && !progress.complete && conversion_document==pending->document);
    pending->init_blocked=true;pending->init_blocked_intent=view.intent;
    pending->init_blocked_available=epd_app_psram_available();pending->init_blocked_epoch=epd_memory_capacity_epoch();
    if(two_pending){
        previous->init_blocked=true;previous->init_blocked_intent=view.intent;
        previous->init_blocked_available=epd_app_psram_available();previous->init_blocked_epoch=epd_memory_capacity_epoch();
        assert(!pending->document->status.end && !previous->document->status.end);
    }
    unsigned steps=pipe_steps;
    if(!two_pending){for(unsigned i=0;i<8;++i)document_worker_step(&service);assert(pipe_steps==steps);}
    unsigned turn=0;optional_counter_steps=0;
    document_progress_t previous_progress={0};document_progress(previous->document,&previous_progress);
    for(unsigned i=0;i<512 && (!progress.complete || !previous_progress.complete);++i){
        /* Keep the injected pixel-init denial active while BODY conversion
         * allocates and retires its own buffers. */
        pending->init_blocked_epoch=epd_memory_capacity_epoch();
        if(two_pending)previous->init_blocked_epoch=pending->init_blocked_epoch;
        window_quiesce(primary_state);
        window_service(primary_state,&turn);worker_input_commit(primary_state);
        if(pending->document->cancelled)fprintf(stderr,"CONVERSION_PREEMPTED\n");
        assert(!pending->document->cancelled);
        assert(!previous->document->cancelled);
        assert(!turn && !optional_counter_steps);
        document_worker_step(&service);document_service_process();collect();
        document_progress(pending->document,&progress);
        document_progress(previous->document,&previous_progress);
    }
    if(!progress.complete)fprintf(stderr,"CONVERSION_STRANDED steps=%u\n",pipe_steps-steps);
    assert(progress.complete && !progress.result && !pending->document->cancelled);
    assert(previous_progress.complete && !previous_progress.result);
    assert(!window_owner(-2));
    if(!two_pending){
        /* This case keeps forward pixel admission denied while testing reverse
         * admission after EOF. Final converter cleanup changes capacity epoch. */
        pending->init_blocked_epoch=epd_memory_capacity_epoch();
        assert(window_prepare(primary_state) && window_owner(-2));
        puts("PASS optional reverse admission preserves the real unfinished forward converter, resumes its paused input to EOF, then admits the reverse chapter");
    }else puts("PASS two unopened direct contexts drain through the real document worker without cancellation, gate starvation, neighbor fallback or new pagination");
    primary_state->selected=false;cluster_retire(primary_state);state=primary_state=NULL;
    for(unsigned i=0;i<1000 && documents;++i){document_worker_step(&service);collect();}
    assert(!documents && !mailbox.retired && !frame_bytes && !layout_bytes && !index_bytes && !flow_jobs && !allocated_bytes && !fd_live);
}
static void successful_window_wait(bool readable)
{
    view_state_t seed={0};seed.session=seed.intent=seed.layout=11;state=&seed;
    bookshelf_item_t file={0};strcpy(file.path,"/flash/success-window.epub");
    file.size=30000;file.modified=readable?3:4;file.format=DOCUMENT_EPUB;
    document_anchor_t anchor={BOOK_DOCUMENT_SOURCE_BODY_UTF8,2,0,0};char error[96];
    committed=false;_unlink(persistent_path);
    view_state_t *old=state_neighbor(document_open_at(&file,&anchor,error,sizeof(error)),false);
    state=primary_state=old;old->selected=true;pump_owner(old);state=old;old->selected=true;
    view_state_t *next=state_neighbor(document_adjacent(old->document,1),false);
    old->following=next;pump_owner(next);state=next;
    view_state_t *following=state_neighbor(document_adjacent(next->document,1),false);
    next->following=following;assert(window_store(following,2));pump_owner(following);state=old;
    committed=false;_unlink(persistent_path);
    view_state_t *a=state_neighbor(document_adjacent(old->document,-1),true);
    old->previous=a;
    assert(!a->init_attempted && !a->init_blocked && !a->document->status.end);
    ++old->intent;
    assert(chapter_select(old,1,NULL)==next);state=primary_state=next;
    assert(window_owner(-2)==a && window_extra(a) && next->following==following);
    target_resolve();assert(view.display_pending);frame_accept(next->frames);
    next->direction=-1;

    /* Keep the already-completed spines' wire alive through deferred CLOSE.
     * The new source has enough input to outlive its first small page. */
    uint8_t *saved_wire=wire_data;size_t saved_bytes=wire_bytes;
    wire_data=NULL;wire_bytes=0;
    picture_event(BOOK_DOCUMENT_CHAPTER,0,10,1,0,NULL,0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,0,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_BEGIN,NULL,0);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,0,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_TARGET,NULL,0);
    for(unsigned i=0;i<16;++i)picture_event(BOOK_DOCUMENT_TEXT,0,0,0,0,"one short page",14);
    picture_event(BOOK_DOCUMENT_SOURCE_ANCHOR,0,16*14,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_END,NULL,0);
    committed=false;_unlink(persistent_path);state=following;
    view_state_t *b=state_neighbor(document_adjacent(following->document,1),false);
    b->status.content_width=80;b->status.bottom=32;
    state=primary_state;assert(window_store(b,2));
    assert(window_slots[0].owner==a && window_slots[1].owner==b);
    assert((int32_t)(b->document->demand-a->document->demand)>0);
    document_prefetch_enable(b->document,false);
    unsigned opened=pipe_opens;
    for(unsigned i=0;i<256 && (conversion_document!=b->document || pipe_opens==opened);++i){
        document_worker_step(&service);document_service_process();
    }
    assert(conversion_document==b->document && b->document->converting && pipe_opens>opened);
    assert(!b->document->status.end && !a->document->status.end);
    exact_neighbor_entry=true;
    if(readable){
        for(unsigned i=0;i<512 && !b->frames;++i){
            state=b;production_neighbor_step();document_worker_step(&service);document_service_process();collect();
        }
        assert(b->frames && b->frames->complete && b->status.ready && !b->display->eof);
        assert(b->document->status.end && !b->document->status.complete && conversion_document==b->document);
    }
    state=primary_state;document_prefetch_enable(b->document,false);
    assert(window_runnable(a,primary_state) && !a->init_blocked && !a->document->status.end);
    /* Observe the real successful window branch before its caller restores flow. */
    assert(window_prepare(primary_state));
    assert(a->initialized && a->target_pending && !a->init_blocked);
    assert(flow_owner()==a->document && !a->document->prefetch_enabled && !flow_allows(b->document));
    unsigned stalled_steps=pipe_steps;
    document_worker_step(&service);document_service_process();
    assert(pipe_steps==stalled_steps && conversion_document==b->document && !b->document->cancelled);
    assert(!a->document->status.end);

    unsigned turn=0,zero_shared_grants=0;optional_counter_steps=0;
    document_progress_t a_progress={0},b_progress={0};
    for(unsigned i=0;i<512;++i){
        state=primary_state;window_quiesce(primary_state);window_service(primary_state,&turn);worker_input_commit(primary_state);
        assert(!a->init_blocked && !b->init_blocked && !a->document->cancelled && !b->document->cancelled);
        assert(!turn && !optional_counter_steps);
        if(conversion_document==b->document && !b->document->status.end &&
            flow_owner()==a->document && a->document->prefetch_enabled){
            assert(flow_allows(b->document) && conversion_runnable(b->document));++zero_shared_grants;
        }
        document_worker_step(&service);document_service_process();collect();
        document_progress(a->document,&a_progress);document_progress(b->document,&b_progress);
        if(a_progress.complete && b_progress.complete && a->frames)break;
    }
    if(!b_progress.complete)fprintf(stderr,"WINDOW_SUCCESS_STALL readable=%u steps=%u a_end=%u b_end=%u a_gate=%u b_allowed=%u active_b=%u\n",
        readable,pipe_steps-stalled_steps,(unsigned)a_progress.end,(unsigned)b_progress.end,
        (unsigned)a->document->prefetch_enabled,(unsigned)flow_allows(b->document),conversion_document==b->document);
    assert(b_progress.complete && !b_progress.result);
    assert(a_progress.complete && !a_progress.result && a->frames && a->frames->complete);
    if(!readable)assert(zero_shared_grants);
    printf("PASS successful window restores conversion: readable=%u A retained/runnable/end0, B active/newer, both EOF, A pixels complete, zero_shared_grants=%u\n",readable,zero_shared_grants);
    exact_neighbor_entry=false;primary_state->selected=false;cluster_retire(primary_state);state=primary_state=NULL;
    for(unsigned i=0;i<1000 && (documents||mailbox.retired);++i){document_worker_step(&service);collect();}
    assert(!documents && !mailbox.retired && !frame_bytes && !layout_bytes && !index_bytes && !flow_jobs && !allocated_bytes && !fd_live);
    free(wire_data);wire_data=saved_wire;wire_bytes=saved_bytes;
}
static void near_neighbor_case(bool check_priority)
{
    view_state_t seed = {0};
    seed.session = seed.intent = seed.layout = 23;
    state = &seed;
    bookshelf_item_t file = {0};
    strcpy(file.path, "/flash/ready-neighbor.epub");
    file.size = 30000; file.modified = 7; file.format = DOCUMENT_EPUB;
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 3, 0, 0};
    char error[96];
    primary_state = state_neighbor(document_open_at(&file, &anchor, error, sizeof(error)), false);
    state = primary_state;
    view_state_t *current = state;
    current->selected = true;
    pump_owner(current);
    state = current;

    if (check_priority) {
        current->direction = 1;
        current->display->cursor = current->display->start;
        current->display->eof = false;
        assert(window_route(1).count == 1 && window_route(1).missing_hop == 0);
        assert(window_route(-1).missing_hop == -1 && window_conversion_ready(current));
        neighbors_prepare(current);
        assert(current->previous && !current->following && "NEAR_PAGE_CONVERSION_FIRST");
        puts("PASS the pending reverse boundary takes priority over a chapter outside the near-page window");
    } else {
        current->direction = -1;
        committed = false;
        _unlink(persistent_path);
        document_t *previous = document_adjacent(current->document, -1);
        assert(previous);
        document_prefetch_enable(previous, true);
        document_target_require(previous, true);
        for (unsigned i = 0; !previous->status.complete; ++i) {
            assert(i < 256);
            document_worker_step(&service); document_service_process();
        }
        assert(previous->status.result == BOOK_OK && current->previous == NULL);
        document_target_require(previous, false);
        document_set_foreground(previous, false);
        document_close(previous);

        committed = false;
        _unlink(persistent_path);
        anchor.chapter = 4;
        document_t *following = document_open_at_internal(&file, &anchor, error, sizeof(error), false, false);
        assert(following && !following->status.complete);
        current->following = state_neighbor(following, false);
        assert(window_route(-1).missing_hop == -1 && !window_conversion_ready(current));
        document_progress_t progress;
        document_progress(current->document, &progress);
        assert(progress.complete && !progress.result && !current->rendering);
        unsigned opens = pipe_opens, turn = 0;
        window_service(current, &turn);
        assert(current->previous && current->previous->document == previous && "READY_NEIGHBOR_REUSED_DURING_CONVERSION");
        assert(pipe_opens == opens && !following->cancelled && !turn);

        following->work_deferred = true;
        for (unsigned i = 0; !current->previous->frames; ++i) {
            assert(i < 256);
            window_service(current, &turn);
            worker_input_commit(current);
            document_worker_step(&service); document_service_process(); collect();
        }
        assert(current->previous->frames->complete && !following->status.complete && !following->cancelled);
        puts("PASS ready previous chapter is reused while the opposite conversion waits");
    }

    current->selected = false;
    cluster_retire(current); state = primary_state = NULL;
    for (unsigned i = 0; documents || mailbox.retired; ++i) {
        assert(i < 1000);
        document_worker_step(&service); collect();
    }
    assert(!frame_bytes && !layout_bytes && !index_bytes && !allocated_bytes && !fd_live);
}

int main(int argc,char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    InitializeCriticalSection(&irq_lock);InitializeCriticalSection(&io_lock);
    manager_setup();
    assert(argc==3);
    if(!strcmp(argv[2],"all")||!strcmp(argv[2],"retention"))retention_cases();
    if(!strcmp(argv[2],"retention"))goto finished;
    pool_limit();
    /* Match the board's shared application PSRAM pool for seven-context cases. */
    psram_capacity=10u*1024u*1024u;
    assert(argc==3);snprintf(test_root,sizeof(test_root),"%s",argv[1]);
    snprintf(persistent_path,sizeof(persistent_path),"%s/persisted.bin",test_root);
    snprintf(index_file,sizeof(index_file),"%s/index.bin",test_root);
    wire_fixture();pipe_events_per_step=1;pipe_step_ticks=60;committed=false;
    if (!strcmp(argv[2], "ready-neighbor") || !strcmp(argv[2], "near-neighbor-priority")) {
        near_neighbor_case(!strcmp(argv[2], "near-neighbor-priority"));
        goto finished;
    }
    if(strcmp(argv[2],"all")&&strcmp(argv[2],"window")){
        assert(!strcmp(argv[2],"waiting-readable") || !strcmp(argv[2],"waiting-zero"));
        successful_window_wait(!strcmp(argv[2],"waiting-readable"));
        goto finished;
    }
    bookshelf_item_t file={0};strcpy(file.path,"/flash/window.epub");file.size=10000;file.modified=1;file.format=DOCUMENT_EPUB;
    document_anchor_t anchor={BOOK_DOCUMENT_SOURCE_BODY_UTF8,3,0,0};char error[96];
    view_state_t seed={0};state=&seed;seed.session=seed.intent=seed.layout=1;
    primary_state=state_neighbor(document_open_at(&file,&anchor,error,sizeof(error)),false);
    state=primary_state;primary_state->selected=true;pump_owner(primary_state);primary_state->selected=primary_state->displayed=true;primary_state->direction=1;
    state=primary_state;primary_state->following=state_neighbor(document_adjacent(primary_state->document,1),false);
    pump_owner(primary_state->following);state=primary_state;
    size_t capacity_before=psram_capacity;unsigned opens_before=pipe_opens;
    psram_capacity=(size_t)allocated_bytes+VIEW_CACHE_RESERVE;
    for(unsigned i=0;i<64;++i)assert(!window_prepare(primary_state));
    assert(!window_owner(2) && pipe_opens==opens_before && created==2);
    assert(primary_state->frames && primary_state->following->frames);
    psram_capacity=capacity_before;
    for(unsigned i=0;i<1000;++i){
        window_prepare(primary_state);worker_input_commit(primary_state);document_worker_step(&service);document_service_process();collect();
        view_state_t *last=window_owner(3);
        if(last && last->frames)break;
    }
    window_route_t route=window_route(1);assert(route.count==3);
    for(unsigned i=0;i<3;++i){
        view_state_t *owner=route.pages[i].owner;assert(owner->frames && owner->frames->complete);
        assert(frame_priority(owner,route.pages[i].number)==i+1);
        char expected[15];snprintf(expected,sizeof(expected),"chapter-%06u",4+i);
        assert(owner->progress.chapter==4+i && !strcmp(owner->display->text,expected));
    }
    assert(window_owner(2)->window_workspace_charge && window_owner(3)->window_workspace_charge);
    assert(layout_bytes<=VIEW_CACHE_BUDGET && frame_bytes<=VIEW_FRAME_BUDGET);
    puts("PASS real page_step/document supply produces three complete one-page-spine frames across EOF with bounded slots and charged workspace");
    puts("PASS admission under reserve pressure does not repeat opens or evict nearer pixels; restored capacity resumes real supply");
    state=primary_state;primary_state->previous=state_neighbor(document_adjacent(primary_state->document,-1),true);
    pump_owner(primary_state->previous);state=primary_state;
    for(unsigned i=0;i<1000;++i){
        window_prepare(primary_state);worker_input_commit(primary_state);document_worker_step(&service);document_service_process();collect();
        view_state_t *last=window_owner(-3);if(last && last->frames)break;
    }
    route=window_route(-1);assert(route.count==3 && created==7);
    for(unsigned i=0;i<3;++i)assert(route.pages[i].owner->frames && frame_priority(route.pages[i].owner,route.pages[i].number)==i+4);
    for(unsigned i=0;i<3;++i){
        view_state_t *owner=route.pages[i].owner;
        assert(owner->preview_clipped && !owner->display->eof && owner->display->cursor.record==owner->progress.end);
        document_cursor_t start;bool end=false;
        assert(window_page(owner,window_number(owner),&start,&end) && end);
        document_cursor_t cursor=owner->display->cursor;--owner->display->cursor.record;
        unsigned number;assert(window_entry(owner,-1,&number) && number==window_number(owner));
        owner->display->cursor=cursor;
    }
    puts("PASS reverse EOF/beginning distance traverses three real single-page spines; total contexts bounded at seven");
    puts("PASS production reverse target/scan/page_step produces eof=false tails accepted by display and cached-page window routes");
    assert(pool_peak==READER_JOBS && flow_jobs<=READER_JOBS);txt_entry_cases();pool_transfer();window_jobs();
    index_growth();idle_window();
    printf("HOST seven-context ledger layout=%zu/%u frame=%zu/%u index=%zu/%u allocated=%zu\n",
        layout_bytes,VIEW_CACHE_BUDGET,frame_bytes,VIEW_FRAME_BUDGET,index_bytes,VIEW_INDEX_GLOBAL,(size_t)allocated_bytes);
    view_frame_t *saved=window_owner(2)->frames;void *pixels=saved->pixels.data;rt_tick_t completed=saved->completed_at;
    view_state_t *old=primary_state,*next=old->following;
    old->displayed=false;assert(chapter_select(old,1,NULL)==next);state=primary_state=next;
    assert(next->following->frames==saved && saved->pixels.data==pixels && saved->completed_at==completed);
    assert(!next->following->window_state_charge && !next->following->window_workspace_charge && !saved->window_charge);
    old=primary_state;next=old->previous;assert(chapter_select(old,-1,NULL)==next);state=primary_state=next;
    assert(window_owner(2)->frames==saved && saved->pixels.data==pixels && saved->completed_at==completed);
    puts("PASS bidirectional slot promotion moves the same frame/owner; pixels and completion tick survive");
    near_timing();
    direct_entry();
    admit_priority();
    view_state_t *pending=window_owner(3);
    pending->building->read_request=document_record_request(pending->document,pending->progress.end,true);
    assert(pending->building->read_request && record_requests);
    worker_quiesce(primary_state,true,true,false);
    assert(pending->background_paused && !pending->building->read_request && pending->frames);
    collect();assert(!record_requests);
    puts("PASS production worker_quiesce cancels extra-slot record requests on menu/suspend and retains complete frames");
    view_state_t *active[7];unsigned active_count=window_contexts(primary_state,active);
    for(unsigned i=0;i<active_count;++i)active[i]->background_paused=false;
    view_frame_t *visible=primary_state->frames;frame_ref(visible);void *shown=visible->pixels.data;
    size_t normal_capacity=psram_capacity;
    psram_capacity=(size_t)allocated_bytes+VIEW_CACHE_RESERVE;
    state=primary_state;
    assert(window_reclaim(0));collect();
    assert(visible->references==2 && visible->pixels.data==shown && *(uint8_t*)shown==4);
    assert(layout_bytes<VIEW_CACHE_BUDGET && frame_bytes<VIEW_FRAME_BUDGET);
    psram_capacity=normal_capacity;frame_release(visible);
    puts("PASS pressure reclaims an optional slot and its charged workspace while preserving bound current pixels");
    close_all();conversion_admission(false);conversion_admission(true);
    successful_window_wait(true);successful_window_wait(false);
finished:
    free(wire_data);wire_data=NULL;wire_bytes=0;
    document_service_shutdown();document_worker_shutdown();document_service_process();
    assert_compute_released();DeleteCriticalSection(&io_lock);DeleteCriticalSection(&irq_lock);
    DeleteCriticalSection(&resource_mutex);
    printf("PASS cleanup created=%u disposed=%u render=%u requests=0 bytes=0 buffers=0 fd=0\n",created,disposed,renders);
    return 0;
}
