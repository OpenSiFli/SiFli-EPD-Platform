/* Real owner accounting with a host physical allocator and thread context. */
#define EPD_MEMORY_HOST
#define RT_USING_THREAD_MEM_CONTEXT
#define RT_TICK_PER_SECOND 1000u
#define RT_NULL NULL
#include "epd_memory_manager.h"
#include <assert.h>
static epd_memory_owner_t current_owner;
static bool fail_system_enter;
static unsigned rt_interrupt_get_nest(void){return 0;}
static unsigned rt_critical_level(void){return 0;}
static rt_thread_t rt_thread_self(void){return (void *)1;}
static epd_memory_owner_t rt_thread_mem_context_get(rt_thread_t t){(void)t;return current_owner;}
static int rt_thread_mem_context_set(rt_thread_t t,epd_memory_owner_t owner)
{
    (void)t;
    if(owner==EPD_MEMORY_OWNER_SYSTEM&&fail_system_enter){fail_system_enter=false;return -1;}
    if(!epd_memory_owner_retain(owner))return -1;
    epd_memory_owner_t previous=current_owner;current_owner=owner;
    epd_memory_owner_release(previous);return RT_EOK;
}
#include "memory_core.inc"
typedef struct {epd_memory_owner_t owner;size_t bytes;epd_app_memory_t region;} allocation_t;
static epd_memory_backend_t backend,sram_backend;
static size_t physical_used,region_used[2];
static unsigned system_allocations,caller_allocations;
static int fail_allocation=-1;
void *epd_app_alloc(size_t n,epd_app_memory_t region)
{
    if(fail_allocation==0)return NULL;
    if(fail_allocation>0)--fail_allocation;
    size_t bytes=sizeof(allocation_t)+(n?n:1);
    allocation_t *p=calloc(1,bytes);if(!p)return NULL;
    epd_memory_change_t c={0};c.kind=EPD_MEMORY_CHANGE_ALLOC;
    c.token=current_owner;c.backend=region==EPD_APP_PSRAM?backend:sram_backend;c.size=n;c.physical=bytes;
    c.available_before=1024*1024-region_used[region];c.available_after=c.available_before-bytes;
    if(epd_memory_try_change(&c)!=EPD_MEMORY_OK){free(p);return NULL;}
    epd_memory_notify(&c);physical_used+=bytes;region_used[region]+=bytes;
    p->owner=current_owner;p->bytes=n;p->region=region;
    if(current_owner==EPD_MEMORY_OWNER_SYSTEM)++system_allocations;else ++caller_allocations;
    return p+1;
}
void epd_app_free(void *ptr)
{
    if(!ptr)return;
    allocation_t *p=(allocation_t *)ptr-1;
    epd_memory_change_t c={0};c.kind=EPD_MEMORY_CHANGE_FREE;
    c.token=p->owner;c.backend=p->region==EPD_APP_PSRAM?backend:sram_backend;c.old_size=p->bytes;
    c.old_physical=sizeof(*p)+(p->bytes?p->bytes:1);
    c.available_before=1024*1024-region_used[p->region];c.available_after=c.available_before+c.old_physical;
    epd_memory_notify(&c);physical_used-=c.old_physical;region_used[p->region]-=c.old_physical;free(p);
}
