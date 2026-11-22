#include "epd_cold_budget.h"
#ifndef EPD_APP_PSRAM
#define EPD_APP_PSRAM 1
#endif
typedef struct { bool active; } epd_shared_service_guard_t;
static bool budget_control_entered;
static void *budget_control_memory;
static bool epd_shared_service_enter(epd_shared_service_guard_t *guard)
{ CHECK(!budget_control_entered); guard->active=budget_control_entered=true; return true; }
static bool epd_shared_service_leave(epd_shared_service_guard_t *guard)
{ CHECK(guard->active && budget_control_entered); guard->active=budget_control_entered=false; return true; }
static void *epd_app_alloc(size_t,int);
static void epd_app_free(void *);
struct rt_mutex { unsigned held; };
#define RT_IPC_FLAG_PRIO 1
#define RT_WAITING_FOREVER (-1)
static int rt_mutex_init(struct rt_mutex *m,const char *name,int flags)
{ (void)name;(void)flags;m->held=0;return 0; }
static int rt_mutex_take(struct rt_mutex *m,int wait) { (void)wait;CHECK(!m->held);++m->held;return 0; }
static int rt_mutex_release(struct rt_mutex *m) { CHECK(m->held);--m->held;return 0; }
static int rt_mutex_detach(struct rt_mutex *m) { CHECK(!m->held);return 0; }
static uint32_t storage_file_writers(const char *path)
{
    for(unsigned i=0;i<STORAGE_CLOSE_SLOTS;++i)
        if(owned_files[i].used && !strcmp(owned_files[i].path,path)) return 1;
    return 0;
}
static unsigned budget_index_reads,budget_index_writes;
static uint32_t storage_crc32(const void *bytes,size_t n)
{
    const unsigned char *p=bytes;uint32_t crc=UINT32_MAX;
    while(n--) {crc^=*p++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
    return ~crc;
}
static bool storage_record_load(const char *path,uint32_t magic,uint32_t version,void *data,size_t size)
{
    ++budget_index_reads;FILE *f=fopen(path,"rb");if(!f)return false;
    uint32_t h[4];bool ok=fread(h,1,sizeof(h),f)==sizeof(h) && h[0]==magic && h[1]==version && h[2]==size &&
        fread(data,1,size,f)==size && fgetc(f)==EOF && h[3]==storage_crc32(data,size);
    return !fclose(f) && ok;
}
static bool storage_file_replace_session(const char *path,const void *data,size_t size,uint32_t session)
{
    CHECK(!session && strstr(path,"cold-budget.bin"));++budget_index_writes;
    char temporary[512];snprintf(temporary,sizeof(temporary),"%s.tmp",path);
    FILE *f=fopen(temporary,"wb");if(!f)return false;
    bool ok=fwrite(data,1,size,f)==size;if(fclose(f))ok=false;
    if(ok){_unlink(path);ok=rename(temporary,path)==0;}return ok;
}
#define stat _stat64
#define S_ISREG(mode) (((mode)&_S_IFMT)==_S_IFREG)
/* BUDGET PRODUCTION */
#undef stat
