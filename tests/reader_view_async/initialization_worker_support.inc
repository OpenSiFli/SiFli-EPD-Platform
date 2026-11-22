/* Controlled allocator and admission boundary around the production reader
 * and SDK recovery state machines. Activity entries use distinct owner tokens. */
static uint32_t clock_ticks;
static unsigned allocation_calls,fail_allocation,live_allocations,live_compute;
static size_t layout_bytes,index_bytes;
static unsigned pressure_calls,context_rejections;
static app_memory_pressure_status_t pressure_status;
static unsigned cancel_calls,foreground_changes;
static bool foreground[3];
static rt_tick_t rt_tick_get(void) { return clock_ticks; }
static epd_memory_owner_t rt_thread_mem_context_get(void *thread) { (void)thread;return fixture_owner; }
uint32_t epd_memory_capacity_epoch(void) { return 1; }
static size_t epd_app_psram_available(void) { return 1024u*1024u; }
static bool frame_reclaim_layout(size_t bytes,unsigned priority) { assert(bytes && !priority);return true; }
static void *epd_app_alloc(size_t bytes,int region) {
    assert(region==EPD_APP_PSRAM);++allocation_calls;
    if(allocation_calls==fail_allocation)return NULL;
    void *p=malloc(bytes);assert(p);memset(p,0xa5,bytes);++live_allocations;return p;
}
static void epd_app_free(void *p) { if(p){assert(live_allocations);--live_allocations;free(p);} }
static void *compute_acquire(int *result) { *result=0;void *p=malloc(8);assert(p);++live_compute;return p; }
static void reader_compute_release(void *p) { if(p){assert(live_compute);--live_compute;free(p);} }
static void *cache_allocate(size_t bytes) { (void)bytes;assert(false);return NULL; }
static void cache_free(void *p) { (void)p;assert(false); }
static void fail(unsigned error) { view.failure=error; }
static void navigation_clear(void) { view.navigation_link=view.navigation_toc=0; }
static void link_clear(void) { navigation_clear();view.link_target[0]=0; }
static void document_set_foreground(void *document,bool enabled) {
    uintptr_t id=(uintptr_t)document;assert(id>0 && id<3);foreground[id]=enabled;++foreground_changes;
}
static bool frame_turn_matches(const view_frame_t *f,const view_command_t *c) { (void)f;(void)c;return false; }
static bool frame_turn_cached(const view_frame_t *f,const view_command_t *c) { (void)f;(void)c;return false; }
static void frame_handoff_note(view_frame_t *f,unsigned intent) { (void)f;(void)intent;assert(false); }
static void core_cancel_pending(void) { assert(view.initialized && view.display);++cancel_calls; }
static void core_seek_cursor(document_cursor_t cursor) { (void)cursor;assert(view.initialized); }
static void core_turn(int turns) { assert(view.initialized && turns);view.deferred_turns=turns; }
static void pagination_seek(unsigned page,view_state_t **current) { (void)page;(void)current;assert(false); }
static void progress_seek(unsigned percent,view_state_t **current) { (void)percent;(void)current;assert(false); }
static view_state_t *chapter_select(view_state_t *current,int direction,void *document) {
    (void)current;(void)direction;(void)document;assert(false);return NULL;
}
static void follow_toc(unsigned entry,view_state_t **current) { (void)entry;(void)current;assert(false); }
static void follow_link(uint32_t entry,view_state_t **current) { (void)entry;(void)current;assert(false); }
static view_frame_t *frame_find(document_cursor_t cursor) { (void)cursor;return NULL; }
static void frame_publish(view_frame_t *frame) { (void)frame;assert(false); }

struct rt_memheap_item { struct rt_memheap_item *next,*next_free; };
static struct rt_memheap_item free_sentinel;
static struct { struct rt_memheap_item *free_list;int lock;size_t available_size; } app_image_psram_memheap;
static int fixture_heap_take(int *lock,int timeout) { assert(lock==&app_image_psram_memheap.lock && !timeout);return RT_EOK; }
static void fixture_heap_release(int *lock) { assert(lock==&app_image_psram_memheap.lock); }
bool epd_memory_app_backend(epd_memory_region_t region,epd_memory_backend_t *out) {
    assert(region==EPD_MEMORY_REGION_HCPU_PSRAM);*out=1;return true;
}
epd_memory_recovery_result_t epd_memory_recovery_admit(epd_memory_recovery_t *r,
    bool failed,bool known,size_t available,bool *reclaim) {
    assert(known && available==app_image_psram_memheap.available_size);*reclaim=false;
    if(r->owner!=fixture_owner) {
        ++context_rejections;r->reason=EPD_MEMORY_REJECT_CONTEXT;r->result=EPD_MEMORY_INVALID;
        return EPD_MEMORY_RECOVERY_DENIED;
    }
    r->min_available=r->physical;
    r->result=failed?EPD_MEMORY_NO_CAPACITY:EPD_MEMORY_OK;
    *reclaim=failed;return failed?EPD_MEMORY_RECOVERY_PENDING:EPD_MEMORY_RECOVERY_READY;
}
static uint32_t pressure_request(size_t bytes) {
    assert(pressure_calls==0 && bytes);++pressure_calls;
    pressure_status.generation=1;pressure_status.pending=true;
    pressure_status.result=APP_MEMORY_PRESSURE_PENDING;return 1;
}
uint32_t app_memory_pressure_request(size_t bytes) { return pressure_request(bytes); }
uint32_t app_memory_pressure_allocation_failed(size_t bytes) { return pressure_request(bytes); }
uint32_t app_memory_pressure_allocation_failed_block(size_t bytes, size_t physical)
{ assert(physical); return app_memory_pressure_allocation_failed(bytes); }
app_memory_pressure_status_t app_memory_pressure_query(void) { return pressure_status; }
