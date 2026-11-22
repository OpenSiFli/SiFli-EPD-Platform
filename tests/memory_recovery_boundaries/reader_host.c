/* Rendering, spare selection and cold conversion are idle in this fixture.
 * The included frame/compute functions, ledger, pressure and allocator are real. */
typedef struct { void *data, *allocation; uint32_t capacity; } epd_compute_buffer_t;
typedef struct { unsigned unused; } view_state_t;
typedef struct view_frame {
    struct view_frame *next;
    view_state_t *owner;
    struct { size_t data_size; const void *data; } image;
    epd_compute_buffer_t pixels;
    epd_memory_recovery_t admission_recovery;
    unsigned intent, memory_intent, number;
    bool memory_reclaimed, memory_waiting, cold_reusing, allocation_attempted, resources_ready;
    uint32_t memory_wait_started;
    const char *admission_stage;
} view_frame_t;
static view_state_t reader_context, *primary_state = &reader_context;
static struct { void *cleanup, *retired, *ack; } mailbox;
static epd_resource_cache_t view_resource;
static size_t frame_bytes;
static int view_reserve_result;
static bool reserve_denied_once;
static unsigned allocate_calls;
static bool frame_workspace_ready(view_frame_t *f) { (void)f; return true; }
static bool frame_spare_take(view_frame_t *f) { (void)f; return false; }
static bool frame_spare_available(view_frame_t *f) { (void)f; return false; }
static view_frame_t **frame_pixel_victim(view_frame_t *f, bool cached) { (void)f; (void)cached; return NULL; }
static int frame_distance(view_frame_t *f) { (void)f; return 1; }
static int frame_retention_distance(int distance) { return distance; }
static bool frame_reuse_pixels(view_frame_t *f) { (void)f; return false; }
static bool frame_nearby_growth(view_frame_t *f) { (void)f; return true; }
static bool frame_admission_error(view_frame_t *f,const char *stage,int result,size_t need)
{ (void)result; (void)need; f->admission_stage=stage; return false; }
static void frame_reclaim_note(view_frame_t *f,view_state_t *s,unsigned n,int distance,const char *reason)
{ (void)f; (void)s; (void)n; (void)distance; (void)reason; assert(false); }
static void frame_release(view_frame_t *f) { (void)f; assert(false); }
static void frame_spare_trim(view_frame_t *f) { (void)f; }
static int frame_cold_reuse(view_frame_t *f) { (void)f; return -1; }
static int frame_memory_failure(view_frame_t *f,const char *stage) { (void)f; (void)stage; return -3; }
static bool frame_reserve(view_frame_t *f)
{ (void)f; if (reserve_denied_once) { reserve_denied_once=false; return false; } return true; }
static unsigned elapsed_ms(uint32_t start) { return (uint32_t)(ticks-start)*1000u/RT_TICK_PER_SECOND; }
static void *reader_allocate(size_t bytes,epd_app_memory_t region)
{ ++allocate_calls; assert(bytes==690719 && region==EPD_APP_PSRAM); return epd_app_alloc(bytes,region); }
#define epd_app_alloc reader_allocate
#include "reader_compute.inc"
#undef epd_app_alloc
#include "reader_storage.inc"

int main(int argc,char **argv)
{
    assert(argc==2); setup(); assert(epd_memory_init()==EPD_MEMORY_OK);
    bool short_block=!strcmp(argv[1],"short"), success=!strcmp(argv[1],"success");
    bool cancel=!strcmp(argv[1],"cancel"), deadline=!strcmp(argv[1],"deadline");
    const size_t payload=690719, pixel_peak=epd_app_allocation_peak(payload);
    const size_t workspace_peak=epd_app_allocation_peak(payload+VIEW_RENDER_WORKSPACE);
    void *target=rt_memheap_alloc(heaps[0],short_block?690688:payload); assert(target);
    void *guards[3],*holes[2]; guards[0]=rt_memheap_alloc(heaps[0],64);
    for(unsigned i=0;i<2;++i) {
        holes[i]=rt_memheap_alloc(heaps[0],600000); guards[i+1]=rt_memheap_alloc(heaps[0],64);
        assert(holes[i]&&guards[i+1]);
    }
    void *tail=rt_memheap_alloc(heaps[0],app_image_psram_memheap.available_size);assert(tail);
    for(unsigned i=0;i<2;++i)rt_memheap_free(holes[i]);
    if(success){rt_memheap_free(target);target=NULL;}
    epd_memory_owner_t account=epd_memory_owner_create("reader-span");
    epd_memory_credit_t credit=request(account,workspace_peak,0,EPD_MEMORY_OK);
    epd_memory_owner_t previous=epd_memory_owner_enter_trusted(credit);
    view_resource=(epd_resource_cache_t){1,account};
    view_frame_t frame={0};frame.owner=primary_state;frame.intent=frame.memory_intent=1;
    frame.resources_ready=true;frame.image.data_size=690688;
    if(!strcmp(argv[1],"used"))
        assert(epd_memory_recovery_begin(&frame.admission_recovery,backends[0],workspace_peak,false)==EPD_MEMORY_RECOVERY_READY);
    reserve_denied_once=!strcmp(argv[1],"admission");
    int result=frame_storage(&frame,false);
    if(success) {
        assert(result==1 && allocate_calls==1 && !frame.admission_recovery.used && !pressure.pending);
    } else {
        assert(result==0 && frame.allocation_attempted && allocate_calls==1);
        assert(frame.admission_recovery.state==EPD_MEMORY_RECOVERY_PENDING);
        assert(frame.admission_recovery.physical==pixel_peak);
        epd_memory_credit_snapshot_t cs;assert(epd_memory_credit_get(credit,&cs));
        assert(cs.unused[backends[0]]==workspace_peak);
        uint32_t generation=frame.admission_recovery.generation,started=frame.admission_recovery.started;
        assert(frame_storage(&frame,false)==0 && allocate_calls==1);
        assert(frame.admission_recovery.generation==generation && frame.admission_recovery.started==started);
        rt_memheap_free(target);target=NULL;
        app_memory_pressure_process("reader-span",false);
        if(pressure_contiguous!=app_memory_pressure_allocation_span(payload)) {
            void *probe=rt_memheap_alloc(heaps[0],payload);assert(probe);rt_memheap_free(probe);
            puts("COUNTEREXAMPLE reader requested credit peak; real pixel allocation fits");return 23;
        }
        assert(pressure.result==(short_block?APP_MEMORY_PRESSURE_EXHAUSTED:APP_MEMORY_PRESSURE_SATISFIED));
        if(cancel)assert(epd_memory_recovery_poll(&frame.admission_recovery,true)==EPD_MEMORY_RECOVERY_DENIED);
        if(deadline)ticks+=(APP_MEMORY_PRESSURE_TIMEOUT_MS*RT_TICK_PER_SECOND+999)/1000;
        result=frame_storage(&frame,false);
        if(short_block||cancel||deadline)assert(result==-3 && !frame.pixels.data && allocate_calls==1);
        else assert(result==1 && frame.pixels.data && allocate_calls==2 && frame.admission_recovery.physical==pixel_peak);
    }
    if(frame.pixels.data) {
        assert(frame.image.data==frame.pixels.data && frame.pixels.capacity==690688);
        assert(frame_bytes==690688);epd_compute_buffer_free(&frame.pixels);frame_bytes=0;
    }
    epd_memory_owner_leave_trusted(previous);epd_memory_credit_end(credit);
    epd_memory_owner_release_trusted(credit);epd_memory_owner_release_trusted(account);
    for(unsigned i=0;i<3;++i)rt_memheap_free(guards[i]);rt_memheap_free(tail);audit();
    printf("PASS real reader payload %s hz=%u\n",argv[1],RT_TICK_PER_SECOND);return 0;
}
