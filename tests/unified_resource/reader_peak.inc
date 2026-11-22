/* Production layout types/functions with the real activity/core ledger. */
#include "../../src/ui/platform/epd_reader_model.h"
#include "../../modules/common/reader/compute/include/epd_reader_layout.h"
#define RT_ALIGN_SIZE 4u
#define RT_ALIGN(n,a) (((n)+(a)-1u)&~((a)-1u))
#include "memory_peak.inc"
#include "reader_peak_types.inc"
#define VIEW_NEIGHBOR_PRIORITY 4096u
#define VIEW_CACHE_BUDGET (2u*1024u*1024u)
#define VIEW_CACHE_RESERVE 0u
#define EPD_COMPUTE_BUSY 1
#define READER_ERROR_LAYOUT_MEMORY 18
typedef struct { size_t budget,reserve;void *(*allocate)(size_t);void (*release)(void *);size_t (*available)(void); } reader_page_cache_t;
static struct {
    bool init_blocked,selected,counting_only,init_attempted,initialized,restoring,target_pending,trace_pending;
    bool init_alloc_retried,backward_target;
    unsigned init_blocked_intent,intent,cache_blocked,prefetch_number,prefetch_record,direction,failure,capacity;
    size_t init_blocked_epoch, window_state_charge, window_workspace_charge;
    epd_memory_recovery_t init_recovery;
    struct { const char *title,*text;bool indexing; } status;
    const char *title;void *document,*compute_spare;
    reader_page_cache_t cache;
    view_page_t *index,*building,*display,*prefetch;
    document_cursor_t *pages,counting_origin[1];
    rt_tick_t turn_started;
} view;
static size_t index_bytes;
static void *view_allocate(size_t bytes,epd_app_memory_t region);
size_t epd_app_psram_available(void) { return CAPACITY-physical[1]; }
static bool frame_reclaim_layout(size_t bytes,unsigned priority)
{ (void)priority; return bytes < epd_app_psram_available(); }
static void document_prefetch_enable(void *document,bool enabled) { (void)document; CHECK(!enabled); }
static void *cache_allocate(size_t bytes) { return view_allocate(bytes,EPD_APP_PSRAM); }
static void cache_free(void *p) { epd_app_free(p); }
static void fail(unsigned error) { view.failure=error; }
static void *compute_acquire(int *status) { *status=0; return view_allocate(sizeof(epd_reader_batch_t),EPD_APP_PSRAM); }
static bool reserve_pages(unsigned n) {
    CHECK(n==2); view.pages=view_allocate(256*sizeof(*view.pages),EPD_APP_PSRAM);
    if (!view.pages) return false;
    view.capacity=256; index_bytes+=view.capacity*sizeof(*view.pages); return true;
}
static void page_reset(view_page_t *p,document_cursor_t cursor) { CHECK(!p->read_request); p->cursor=cursor;p->collect=true; }
static bool window_charge(size_t bytes) { (void)bytes; return true; }
static void window_uncharge_bytes(size_t bytes) { CHECK(bytes==view.window_workspace_charge); }
static void reader_compute_release(void *memory) { epd_app_free(memory); }
/* These cases exercise admitted working-set growth and transfer. Rejection
 * classification and recovery use the SDK allocator in unified_memory. */
static epd_memory_recovery_result_t view_memory_recover(epd_memory_recovery_t *r,size_t bytes,bool failed)
{ (void)r;(void)bytes;(void)failed; CHECK(false);return EPD_MEMORY_RECOVERY_DENIED; }
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *r)
{ (void)r; CHECK(false); return EPD_MEMORY_RECOVERY_DENIED; }
#include "reader_peak_functions.inc"
static void reader_peak_case(const char *name)
{
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,65536,4096,NULL,NULL);
    epd_activity_guard_t guard; CHECK(work);drain();CHECK(epd_activity_work_enter(work,&guard));
    if (!strcmp(name,"reader_start")) {
        _Static_assert(sizeof(view_page_t)>=48368 && sizeof(view_page_t)<48400, "production page layout");
        void *first=epd_app_alloc(sizeof(view_page_t),EPD_APP_PSRAM); CHECK(first);
        CHECK(!epd_app_alloc(sizeof(view_page_t),EPD_APP_PSRAM)); epd_app_free(first);
        view.selected=true; CHECK(core_initialize() && view.initialized && !view.failure);
        epd_memory_owner_t token=block_owner(view.index);
        CHECK(block_owner(view.building)==token && block_owner(view.display)==token && block_owner(view.prefetch)==token);
        epd_memory_owner_snapshot_t owner; CHECK(epd_memory_owner_get(token,&owner)&&owner.account==app);
        CHECK(physical[1]>4*sizeof(view_page_t));
        core_initialize_release(); CHECK(!index_bytes && !view.capacity && !view.pages && !view.compute_spare);
    } else {
        unsigned char *old=view_allocate(64,EPD_APP_PSRAM); CHECK(old);memset(old,0x5a,64);
        epd_memory_owner_t old_token=block_owner(old);
        epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();
        work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,65536,4096,NULL,NULL);
        CHECK(work);drain();CHECK(epd_activity_work_enter(work,&guard));
        CHECK(!view_resize(old,64,CAPACITY*2)); CHECK(old[63]==0x5a&&block_owner(old)==old_token);
        unsigned char *fresh=view_resize(old,64,131072); CHECK(fresh&&fresh[63]==0x5a&&block_owner(fresh)!=old_token);
        epd_app_free(fresh);
    }
    epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();CHECK(!physical[1]);
}
