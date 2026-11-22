#include "core_support.inc"
void *epd_app_realloc(void *old,size_t size,epd_app_memory_t region)
{
    if(!size){epd_app_free(old);return NULL;}
    void *next=epd_app_alloc(size,region);if(!next)return NULL;
    if(old){epd_memory_block_snapshot_t block;CHECK(epd_memory_block_get(old,&block)==EPD_MEMORY_OK);
        memcpy(next,old,size<block.size?size:block.size);epd_app_free(old);}
    return next;
}
void probe_block(epd_activity_work_t *,epd_activity_work_t *);
bool probe_retry(epd_activity_work_t *);
bool probe_global(epd_memory_account_snapshot_t *);
void probe_cleanup(bool);

static epd_activity_work_t *page_anchor(void)
{
    actor=&actors[0];
    CHECK(rt_thread_mem_context_set(NULL,app)==0);
    epd_memory_owner_t page=epd_memory_scope_create_trusted(app,EPD_MEMORY_SCOPE_PAGE);
    CHECK(page!=EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_resource_runtime_page_state(page,17,true)==EPD_RESOURCE_OK);
    CHECK(rt_thread_mem_context_set(NULL,page)==0);
    epd_activity_work_t *parent=epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE,0,65536,4096,NULL,NULL);
    CHECK(parent);drain();CHECK(epd_activity_work_ready(parent)==1);
    CHECK(epd_activity_work_settle(parent));drain();
    actor=&actors[2];CHECK(rt_thread_mem_context_set(NULL,app)==0);
    return parent;
}
static void *external(size_t bytes)
{
    epd_memory_owner_t before=epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    void *p=epd_app_alloc(bytes,EPD_APP_PSRAM);CHECK(p);
    epd_memory_owner_leave_trusted(before);return p;
}
static epd_activity_work_t *denied(epd_activity_work_t *parent)
{
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_NEARBY,
        epd_activity_work_activity(parent),65536,4096,NULL,NULL);
    CHECK(work);drain();CHECK(epd_activity_work_prepare(work,65536,4096)==0);
    ticks+=rt_tick_from_millisecond(501);drain();
    CHECK(epd_activity_work_prepare(work,65536,4096)==-1);return work;
}
static bool fresh_ready(epd_activity_work_t *parent)
{
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_NEARBY,
        epd_activity_work_activity(parent),65536,4096,NULL,NULL);
    CHECK(work);drain();bool ready=epd_activity_work_prepare(work,65536,4096)==1;
    epd_activity_work_close(work);drain();return ready;
}
int main(int argc,char **argv)
{
    CHECK(argc==2);setup();
    size_t baseline[2]={physical[0],physical[1]};
    if (!strcmp(argv[1],"snapshot")) {
        epd_memory_account_snapshot_t initial;
        CHECK(probe_global(&initial));size_t previous=0;
        for(unsigned c=0;c<EPD_MEMORY_CLASS_COUNT;++c)previous+=initial.backend[backends[1]].physical[c];
        epd_memory_owner_t other=epd_memory_owner_create("other");CHECK(other!=EPD_MEMORY_OWNER_INVALID);
        epd_memory_owner_t before=epd_memory_owner_enter_trusted(other);
        void *p=epd_app_alloc(8192,EPD_APP_PSRAM);CHECK(p);epd_memory_owner_leave_trusted(before);
        epd_memory_account_snapshot_t global,local;
        CHECK(epd_memory_owner_current()==app && app!=EPD_MEMORY_OWNER_SYSTEM);
        CHECK(probe_global(&global) && epd_memory_account_snapshot(app,&local));
        size_t all=0,own=0;
        for(unsigned c=0;c<EPD_MEMORY_CLASS_COUNT;++c){all+=global.backend[backends[1]].physical[c];own+=local.backend[backends[1]].physical[c];}
        CHECK(all==previous+8224 && own<all);
        printf("PASS module nonSYSTEM snapshot includes other account: global=%zu local=%zu\n",all,own);
        epd_app_free(p);drain();CHECK(!semaphores && physical[1]==previous);return 0;
    }
    epd_activity_work_t *parent=page_anchor();
    if(!strcmp(argv[1],"large_demand")) {
        void *pressure=external(CAPACITY-128u*1024u-32);
        epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_NEARBY,
            epd_activity_work_activity(parent),65536,4096,NULL,NULL);
        CHECK(work);drain();CHECK(epd_activity_work_ready(work)==1);
        int prepared=epd_activity_work_prepare(work,262144,8192);printf("large prepare=%d\n",prepared);CHECK(prepared==0);
        ticks+=rt_tick_from_millisecond(501);drain();CHECK(epd_activity_work_prepare(work,65536,4096)==-1);
        epd_activity_demand_t demand;CHECK(epd_activity_work_demand(work,&demand));
        CHECK(demand.psram==262144 && demand.sram==8192);
        probe_block(parent,work);drain();CHECK(!probe_retry(parent));
        for(unsigned i=0;i<32;++i){
            void *small=epd_app_alloc(64,EPD_APP_SRAM);CHECK(small);drain();CHECK(!probe_retry(parent));
            epd_app_free(small);drain();CHECK(!probe_retry(parent));
        }
        epd_app_free(pressure);drain();CHECK(probe_retry(parent));
        work=epd_activity_work_queue(EPD_RESOURCE_NEARBY,epd_activity_work_activity(parent),demand.psram,demand.sram,NULL,NULL);
        CHECK(work);drain();CHECK(epd_activity_work_prepare(work,demand.psram,demand.sram)==1);
        epd_activity_work_close(work);drain();
    } else if(!strcmp(argv[1],"own_cleanup")) {
        void *pressure=NULL;
        for(unsigned attempt=0;attempt<2;++attempt){
            epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_NEARBY,
                epd_activity_work_activity(parent),65536,4096,NULL,NULL);
            CHECK(work);drain();CHECK(epd_activity_work_ready(work)==1);
            epd_activity_guard_t guard;CHECK(epd_activity_work_enter(work,&guard));
            void *pending=epd_app_alloc(4096,EPD_APP_PSRAM);CHECK(pending);
            epd_activity_work_leave(&guard);
            if(!pressure){
                epd_memory_backend_snapshot_t heap;CHECK(epd_memory_backend_get(backends[1],&heap));
                pressure=external(CAPACITY-physical[1]-heap.usage.unused-512-32);
            }
            int prepared=epd_activity_work_prepare(work,65536,4096);printf("own prepare=%d attempt=%u\n",prepared,attempt);CHECK(prepared==0);
            ticks+=rt_tick_from_millisecond(501);drain();CHECK(epd_activity_work_prepare(work,65536,4096)==-1);
            probe_cleanup(true);probe_block(parent,work);drain();CHECK(!probe_retry(parent));
            epd_app_free(pending);drain();probe_cleanup(false);
            CHECK(probe_retry(parent)==(attempt==0));
        }
        for(unsigned i=0;i<100;++i){ticks+=rt_tick_from_millisecond(100);drain();CHECK(!probe_retry(parent));}
        epd_app_free(pressure);drain();
    } else if(!strcmp(argv[1],"unrelated")) {
        static unsigned unrelated_key;
        epd_memory_backend_config_t config={EPD_MEMORY_REGION_ACPU, CAPACITY,0,EPD_MEMORY_BACKEND_NONE};
        epd_memory_backend_t unrelated;
        CHECK(epd_memory_backend_register(&unrelated_key,&config,&unrelated)==EPD_MEMORY_OK);
        epd_memory_credit_spec_t spec={0};spec.bytes[unrelated]=8192;spec.priority=EPD_MEMORY_PRIORITY_NORMAL;
        epd_memory_credit_t grant;
        CHECK(epd_memory_credit_request(app,&spec,&grant)==EPD_MEMORY_OK);
        void *pressure=external(CAPACITY-32768-32);
        probe_block(parent,denied(parent));drain();
        CHECK(!probe_retry(parent));
        CHECK(epd_memory_credit_end(grant));epd_memory_owner_release(grant);drain();
        bool rearmed=probe_retry(parent),fits=fresh_ready(parent);
        printf("unrelated backend release: rearmed=%u actual_NEARBY_ready=%u\n",rearmed,fits);
        CHECK(!rearmed && !fits);
        epd_app_free(pressure);
    } else {
        void *pressure=external(CAPACITY-32768-32);
        probe_block(parent,denied(parent));drain();
        CHECK(!probe_retry(parent));
        epd_app_free(pressure);drain();CHECK(probe_retry(parent));CHECK(fresh_ready(parent));
        void *retained=external(CAPACITY/2-32);
        pressure=external(CAPACITY/2-32768-32);
        probe_block(parent,denied(parent));drain();
        CHECK(!probe_retry(parent));
        epd_app_free(pressure);drain();
        bool rearmed=probe_retry(parent),fits=fresh_ready(parent);
        printf("later legitimate occupancy: rearmed=%u actual_NEARBY_ready=%u\n",rearmed,fits);
        CHECK(rearmed && fits);
        epd_app_free(retained);
    }
    epd_activity_work_close(parent);drain();
    CHECK(!semaphores && physical[0]==baseline[0] && physical[1]==baseline[1]);
    printf("PASS %s (%u ticks/s)\n",argv[1],RT_TICK_PER_SECOND);return 0;
}
