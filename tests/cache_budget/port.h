/* Real cold budget; host filesystem primitives and resident control allocation. */
#include "../../src/platform/epd_cold_budget.h"
typedef struct stat budget_stat_t;
static unsigned budget_lock, budget_writes, budget_reads;
static bool budget_fail_index;
static uint8_t *budget_index;
static size_t budget_index_size;
static uint32_t budget_crc(const void *data, size_t bytes)
{
    const uint8_t *p = data; uint32_t crc = UINT32_MAX;
    while (bytes--) { crc ^= *p++; for (unsigned i=0;i<8;++i) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); }
    return ~crc;
}
static int budget_stat(const char *path, budget_stat_t *st)
{
    if (strstr(path,"cold-budget.bin")) {
        if (!budget_index_size) { errno=ENOENT; return -1; }
        memset(st,0,sizeof(*st)); st->st_size=(long)budget_index_size; st->st_mode=BUDGET_REGULAR; return 0;
    }
    return stat(path,st);
}
static bool budget_index_save(const char *path,const void *data,size_t size,uint32_t session)
{
    CHECK(strstr(path,"cold-budget.bin") && !session && !BUDGET_STORAGE_LOCKED);
    ++budget_writes; if (budget_fail_index) return false;
    budget_index=realloc(budget_index,size); CHECK(budget_index);
    memcpy(budget_index,data,size); budget_index_size=size; return true;
}
static bool budget_index_load(const char *path,uint32_t magic,uint32_t version,void *data,size_t bytes)
{
    CHECK(strstr(path,"cold-budget.bin")); ++budget_reads;
    if(budget_index_size!=16+bytes)return false;
    uint32_t h[4]; memcpy(h,budget_index,sizeof(h));
    if(h[0]!=magic || h[1]!=version || h[2]!=bytes || h[3]!=budget_crc(budget_index+16,bytes))return false;
    memcpy(data,budget_index+16,bytes); return true;
}
struct budget_mutex { bool held; };
static int budget_mutex_init(struct budget_mutex *m,const char *name,int flags)
{ (void)name;(void)flags; m->held=false;return 0; }
static int budget_mutex_detach(struct budget_mutex *m) { CHECK(!m->held);return 0; }
static int budget_mutex_take(struct budget_mutex *m,int timeout)
{ (void)timeout;CHECK(!m->held && !BUDGET_STORAGE_LOCKED);m->held=true;++budget_lock;return 0; }
static int budget_mutex_release(struct budget_mutex *m)
{ CHECK(m->held && budget_lock);m->held=false;--budget_lock;return 0; }
typedef struct { unsigned unused; } budget_guard;
static bool budget_enter(budget_guard *g) { (void)g;return true; }
static bool budget_leave(budget_guard *g) { (void)g;return true; }
static void *budget_alloc(size_t size,int region)
{ (void)region;CHECK(!budget_lock && !BUDGET_STORAGE_LOCKED);return calloc(1,size); }
#pragma push_macro("stat")
#pragma push_macro("rt_mutex_take")
#pragma push_macro("rt_mutex_release")
#pragma push_macro("INIT_COMPONENT_EXPORT")
#pragma push_macro("RTM_EXPORT")
#undef stat
#define stat(p,s) budget_stat(p,s)
#define rt_mutex budget_mutex
#define rt_mutex_init budget_mutex_init
#define rt_mutex_detach budget_mutex_detach
#define rt_mutex_take budget_mutex_take
#define rt_mutex_release budget_mutex_release
#define epd_shared_service_guard_t budget_guard
#define epd_shared_service_enter budget_enter
#define epd_shared_service_leave budget_leave
#define epd_app_alloc budget_alloc
#define epd_app_free free
#define storage_crc32 budget_crc
#define storage_record_load budget_index_load
#define storage_file_replace_session budget_index_save
#undef INIT_COMPONENT_EXPORT
#define INIT_COMPONENT_EXPORT(fn)
#undef RTM_EXPORT
#define RTM_EXPORT(fn)
#include "cold_budget_actual.inc"
#undef rt_mutex
#undef rt_mutex_init
#undef rt_mutex_detach
#undef rt_mutex_take
#undef rt_mutex_release
#undef epd_shared_service_guard_t
#undef epd_shared_service_enter
#undef epd_shared_service_leave
#undef epd_app_alloc
#undef epd_app_free
#undef storage_crc32
#undef storage_record_load
#undef storage_file_replace_session
#pragma pop_macro("stat")
#pragma pop_macro("rt_mutex_take")
#pragma pop_macro("rt_mutex_release")
#pragma pop_macro("INIT_COMPONENT_EXPORT")
#pragma pop_macro("RTM_EXPORT")
