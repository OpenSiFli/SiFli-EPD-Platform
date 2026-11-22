#define main legacy_memory_main
#include "memory_host.inc"
#undef main
#include "epd_resource.h"
typedef enum { APP_SERVICE_RECLAIM_NONE,APP_SERVICE_RECLAIM_PROGRESS,APP_SERVICE_RECLAIM_WAITING } app_service_reclaim_result_t;
static bool app_service_stopping(void){return false;}
static void epd_power_notify(void){}
static bool epd_power_sleeping(void){return false;}
static void app_catalog_background_resume(void){}
static app_service_reclaim_result_t app_service_reclaim_step(const char *f,uint32_t g){(void)f;(void)g;return APP_SERVICE_RECLAIM_NONE;}
void epd_resource_reclaim_status(uint32_t b,epd_resource_reclaim_snapshot_t *s){(void)b;memset(s,0,sizeof(*s));}
size_t epd_resource_reclaim(uint32_t b,size_t n,size_t limit){(void)b;(void)n;(void)limit;return 0;}
#include "pressure.inc"

static epd_memory_recovery_result_t fail_allocation(epd_memory_recovery_t *s,size_t payload)
{
#if CANDIDATE
    return epd_memory_recovery_failed_allocation(s,payload);
#else
    (void)payload;return epd_memory_recovery_failed(s);
#endif
}
int main(int argc,char **argv)
{
    assert(argc==2);setup();assert(epd_memory_init()==EPD_MEMORY_OK);
    const size_t payload=690719,peak=epd_app_allocation_peak(payload);
    if(!strcmp(argv[1],"query")) {
#if CANDIDATE
        assert(!app_memory_pressure_allocation_span(0));
        assert(!app_memory_pressure_allocation_span(SIZE_MAX));
        size_t sizes[]={1,11,12,13,31,32,33,690688,690719,690720};
        for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
            void *p=rt_memheap_alloc(heaps[0],sizes[i]);assert(p);
            assert(app_memory_pressure_allocation_span(sizes[i])==physical(p));
            assert(epd_app_allocation_peak(sizes[i])>=physical(p));rt_memheap_free(p);
        }
#if defined(SOLUTION)
        const char *names[]={"psram_memheap","heap","app_imag"};
        for(unsigned n=0;n<3;++n){
            strcpy(app_image_psram_memheap.parent.name,names[n]);
            for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
                void *p=rt_memheap_alloc(heaps[0],sizes[i]);assert(p);
                assert(app_memory_pressure_allocation_span(sizes[i])==physical(p));rt_memheap_free(p);
            }
        }
#endif
#else
        puts("SKIP query is outside the old-API control");return 77;
#endif
        audit();puts("PASS allocation span query");return 0;
    }
    /* All gaps are created by the real allocator/free paths. Allocated guards
     * prevent coalescing, and the two 600KB gaps provide ample total capacity. */
    void *target=rt_memheap_alloc(heaps[0],!strcmp(argv[1],"short")?690688:payload);
    assert(target);size_t target_span=physical(target);
    void *guards[3],*holes[2];guards[0]=rt_memheap_alloc(heaps[0],64);
    for(unsigned i=0;i<2;++i){holes[i]=rt_memheap_alloc(heaps[0],600000);guards[i+1]=rt_memheap_alloc(heaps[0],64);assert(holes[i]&&guards[i+1]);}
    size_t tail_bytes=app_image_psram_memheap.available_size;
    void *tail=rt_memheap_alloc(heaps[0],tail_bytes);assert(tail);
    for(unsigned i=0;i<2;++i)rt_memheap_free(holes[i]);
    epd_memory_owner_t account=epd_memory_owner_create("span-client");
    epd_memory_credit_t credit=request(account,peak,0,EPD_MEMORY_OK);
    epd_memory_owner_t previous=epd_memory_owner_enter_trusted(credit);
    epd_memory_recovery_t recovery={0};
    assert(epd_memory_recovery_begin(&recovery,backends[0],peak,false)==EPD_MEMORY_RECOVERY_READY);
    uint32_t started=recovery.started;
    assert(!rt_memheap_alloc(heaps[0],payload));
    if(!strcmp(argv[1],"invalid")) {
#if CANDIDATE
        epd_memory_recovery_t unchanged=recovery;
        assert(epd_memory_recovery_failed_allocation(&recovery,0)==EPD_MEMORY_RECOVERY_DENIED);
        assert(epd_memory_recovery_failed_allocation(&recovery,SIZE_MAX)==EPD_MEMORY_RECOVERY_DENIED);
        assert(epd_memory_recovery_failed_allocation(&recovery,peak+1024)==EPD_MEMORY_RECOVERY_DENIED);
        assert(!memcmp(&recovery,&unchanged,sizeof(recovery)));
        epd_memory_recovery_t fresh={0};
        assert(epd_memory_recovery_begin_allocation(&fresh,backends[1],peak,payload,true)==EPD_MEMORY_RECOVERY_DENIED);
        assert(!fresh.used && !pressure.pending);
#else
        puts("SKIP explicit-payload validation is outside the old-API control");return 77;
#endif
    }
    if(!strcmp(argv[1],"initial")) {
        memset(&recovery,0,sizeof(recovery));
#if CANDIDATE
        assert(epd_memory_recovery_begin_allocation(&recovery,backends[0],peak,payload,true)==EPD_MEMORY_RECOVERY_PENDING);
#else
        assert(epd_memory_recovery_begin(&recovery,backends[0],peak,true)==EPD_MEMORY_RECOVERY_PENDING);
#endif
    } else if(!strcmp(argv[1],"legacy")) {
        assert(epd_memory_recovery_failed(&recovery)==EPD_MEMORY_RECOVERY_PENDING);
    } else assert(fail_allocation(&recovery,payload)==EPD_MEMORY_RECOVERY_PENDING);
    assert(recovery.physical==peak && recovery.started==started);
    if(!strcmp(argv[1],"continue")) {
        uint32_t generation=recovery.generation;
        epd_memory_owner_leave_trusted(previous);epd_memory_credit_end(credit);epd_memory_owner_release_trusted(credit);
        credit=request(account,peak,0,EPD_MEMORY_OK);previous=epd_memory_owner_enter_trusted(credit);
        epd_memory_recovery_t next={0};
        assert(epd_memory_recovery_continue(&next,&recovery)==EPD_MEMORY_RECOVERY_PENDING);
        assert(next.started==started && next.generation==generation && next.physical==peak);
        assert(recovery.state==EPD_MEMORY_RECOVERY_DENIED);recovery=next;
    }
    size_t credit_unused;epd_memory_credit_snapshot_t cs;assert(epd_memory_credit_get(credit,&cs));credit_unused=cs.unused[backends[0]];
    rt_memheap_free(target);
    if(!strcmp(argv[1],"cancel")) {
        assert(epd_memory_recovery_poll(&recovery,true)==EPD_MEMORY_RECOVERY_DENIED);assert(recovery.result==EPD_MEMORY_CANCELLED);
    }else if(!strcmp(argv[1],"deadline")) {
        ticks+=(APP_MEMORY_PRESSURE_TIMEOUT_MS*RT_TICK_PER_SECOND+999)/1000;
        assert(epd_memory_recovery_poll(&recovery,false)==EPD_MEMORY_RECOVERY_DENIED);assert(recovery.result==EPD_MEMORY_TIMEOUT);
    }else{
        if(!strcmp(argv[1],"busy")){
            assert(rt_sem_take(&app_image_psram_memheap.lock,0)==RT_EOK);
            app_memory_pressure_process("span-client",false);assert(pressure.pending);
            rt_sem_release(&app_image_psram_memheap.lock);
        }
        if(!strcmp(argv[1],"merge")){
            uint32_t g=app_memory_pressure_allocation_failed_block(pressure.min_available,target_span+256);
            assert(g==recovery.generation);assert(pressure_contiguous==target_span+256);
        }
        app_memory_pressure_process("span-client",false);
        bool must_fail=!strcmp(argv[1],"short")||!strcmp(argv[1],"merge")||!strcmp(argv[1],"legacy");
        app_memory_pressure_status_t status=app_memory_pressure_query();
        assert(epd_memory_credit_get(credit,&cs));assert(cs.unused[backends[0]]==credit_unused);
        void *probe=rt_memheap_alloc(heaps[0],payload);
        printf("real span=%zu credit_peak=%zu pressure_span=%zu result=%u allocator=%s credit_unused=%zu\n",target_span,peak,pressure_contiguous,status.result,probe?"success":"NULL",credit_unused);fflush(stdout);
        if(!must_fail && status.result!=APP_MEMORY_PRESSURE_SATISFIED){assert(probe);return 23;}
        if(must_fail)assert(status.result==APP_MEMORY_PRESSURE_EXHAUSTED);
        if(!strcmp(argv[1],"short"))assert(!probe);else assert(probe);
        if(probe)rt_memheap_free(probe);
        if(!must_fail)assert(epd_memory_recovery_poll(&recovery,false)==EPD_MEMORY_RECOVERY_READY);
    }
    epd_memory_owner_leave_trusted(previous);epd_memory_credit_end(credit);epd_memory_owner_release_trusted(credit);epd_memory_owner_release_trusted(account);
    for(unsigned i=0;i<3;++i)rt_memheap_free(guards[i]);rt_memheap_free(tail);audit();
    printf("PASS span boundary %s hz=%u\n",argv[1],RT_TICK_PER_SECOND);return 0;
}
