/* Exercise the SDK's fd_alloc against the real core ledger.
 * The host heap models in-place realloc using the source block's owner,
 * matching both SDK realloc paths. No filesystem or UART is accessed. */
#include <sys/types.h>
#include "ff.h"
#undef near
#define DFS_FD_MAX 16
#define RT_NULL NULL
struct dfs_fdtable { unsigned maxfd; struct dfs_fd **fds; };
static struct dfs_fdtable _fdtab;
static unsigned dfs_reallocs, dfs_callocs;
static epd_memory_result_t dfs_realloc_result;
static void *dfs_realloc(void *ptr, size_t size)
{
    ++dfs_reallocs;
    if (!ptr) return epd_app_alloc(size, EPD_APP_SRAM);
    unsigned i;
    for (i=0; i<128 && blocks[i].ptr!=ptr; ++i) {} CHECK(i<128 && !blocks[i].region);
    epd_memory_change_t c={0};
    c.kind=EPD_MEMORY_CHANGE_REALLOC_INPLACE; c.backend=backends[0]; c.token=blocks[i].owner;
    c.old_size=blocks[i].bytes-32; c.old_physical=blocks[i].bytes;
    c.size=size; c.physical=size+32;
    c.available_before=CAPACITY-physical[0]; c.available_after=c.available_before+c.old_physical-c.physical;
    dfs_realloc_result=epd_memory_try_change(&c);
    if (dfs_realloc_result!=EPD_MEMORY_OK) return NULL;
    ptr=realloc(ptr,size); CHECK(ptr); epd_memory_notify(&c);
    physical[0]=physical[0]-c.old_physical+c.physical;
    blocks[i].ptr=ptr; blocks[i].bytes=c.physical; epd_memory_dispatch_changes(); return ptr;
}
static void *dfs_calloc(size_t count,size_t size)
{
    ++dfs_callocs;
    void *p=epd_app_alloc(count*size,EPD_APP_SRAM);
    if (p) memset(p,0,count*size); return p;
}
#define rt_realloc dfs_realloc
#define rt_calloc dfs_calloc
#include "dfs_alloc.inc"
#undef rt_realloc
#undef rt_calloc
static void dfs_close_slot(struct dfs_fdtable *table,unsigned slot)
{
    CHECK(slot<table->maxfd && table->fds[slot] && table->fds[slot]->ref_count==1);
    epd_app_free(table->fds[slot]); table->fds[slot]=NULL;
}
static void dfs_credit_case(const char *name)
{
    #define table _fdtab
    epd_activity_guard_t guard;
    epd_activity_work_t *work;
    if (!strcmp(name,"dfs_closed_table")) {
        work=epd_activity_work_queue(EPD_RESOURCE_OPTIONAL,0,4096,4096,NULL,NULL);
        CHECK(work); drain(); CHECK(epd_activity_work_ready(work)==1 && epd_activity_work_enter(work,&guard));
        CHECK(fd_alloc(&table,0)==0); table.fds[0]->ref_count=1;
        epd_memory_owner_t owner=block_owner(table.fds);
        dfs_close_slot(&table,0); epd_activity_work_leave(&guard); epd_activity_work_close(work); drain();
        CHECK(owner==EPD_MEMORY_OWNER_SYSTEM);
        work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,4096,NULL,NULL);
        CHECK(work); drain(); CHECK(epd_activity_work_ready(work)==1 && epd_activity_work_enter(work,&guard));
        for (int i=0;i<4;++i) { CHECK(fd_alloc(&table,0)==i); table.fds[i]->ref_count=1; }
        CHECK(fd_alloc(&table,0)==4 && dfs_realloc_result==EPD_MEMORY_OK);
        table.fds[4]->ref_count=1;
        CHECK(block_owner(table.fds)==owner && table.maxfd==8);
        for (unsigned i=0;i<5;++i) dfs_close_slot(&table,i);
        epd_app_free(table.fds); epd_activity_work_leave(&guard); epd_activity_work_close(work);
        return;
    }
    epd_memory_owner_t previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
    CHECK(fd_alloc(&table,0)==0); table.fds[0]->ref_count=1;
    CHECK(block_owner(table.fds)==EPD_MEMORY_OWNER_SYSTEM);
    epd_memory_owner_leave(previous);
    if (!strcmp(name,"dfs_full")) {
        previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
        for (int i=1;i<DFS_FD_MAX;++i) { CHECK(fd_alloc(&table,0)==i); table.fds[i]->ref_count=1; }
        unsigned calls=dfs_reallocs+dfs_callocs;
        CHECK(fd_alloc(&table,0)==DFS_FD_MAX && calls==dfs_reallocs+dfs_callocs);
        for (unsigned i=1;i<DFS_FD_MAX;++i) dfs_close_slot(&table,i);
        epd_memory_owner_leave(previous);
    } else if (!strcmp(name,"dfs_fil_peak")) {
        work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,4096,NULL,NULL);
        CHECK(work);drain();CHECK(epd_activity_work_enter(work,&guard));
        CHECK(!epd_app_alloc(sizeof(FIL),EPD_APP_SRAM));
        size_t peak=epd_resource_file_io_peak(512);
        CHECK(peak>sizeof(FIL)+512 && epd_resource_file_io_peak(SIZE_MAX)==SIZE_MAX);
        void *files[6],*paths[6];
        for(unsigned n=0;n<6;++n) {
            CHECK(epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,peak)==EPD_RESOURCE_OK);
            CHECK(fd_alloc(&table,0)==(int)n+1);table.fds[n+1]->ref_count=1;
            paths[n]=epd_app_alloc(256,EPD_APP_SRAM);files[n]=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);
            void *driver=epd_app_alloc(256,EPD_APP_SRAM),*lfn=epd_app_alloc((FF_MAX_LFN+1u)*2u,EPD_APP_SRAM);
            CHECK(paths[n]&&files[n]&&driver&&lfn);
            epd_app_free(driver);epd_app_free(lfn);
        }
        CHECK(physical[0]>6*sizeof(FIL));
        for(unsigned n=0;n<6;++n) { epd_app_free(files[n]);epd_app_free(paths[n]);dfs_close_slot(&table,n+1); }
        epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();
    } else {
        unsigned count=!strcmp(name,"dfs_zero") ? 1 : 8;
        for (unsigned n=0;n<count;++n) {
            work=epd_activity_work_queue(n==7 ? EPD_RESOURCE_CONTINUOUS : EPD_RESOURCE_OPTIONAL,
                0,4096,count==1 ? 0 : n==7 ? 16384 : 4096,NULL,NULL);
            CHECK(work); drain(); CHECK(epd_activity_work_ready(work)==1 && epd_activity_work_enter(work,&guard));
            unsigned reallocs=dfs_reallocs, callocs=dfs_callocs;
            int fd=fd_alloc(&table,0);
            CHECK(dfs_reallocs==reallocs && dfs_callocs==callocs+1);
            if (count==1) CHECK(fd==(int)table.maxfd && CAPACITY-physical[0]>52764);
            else {
                CHECK(fd==1); table.fds[fd]->ref_count=1;
                void *path=epd_app_alloc(256,EPD_APP_SRAM), *file=epd_app_alloc(1024,EPD_APP_SRAM);
                CHECK(path && file);
                epd_app_free(file); epd_app_free(path); dfs_close_slot(&table,(unsigned)fd);
            }
            epd_activity_work_leave(&guard); epd_activity_work_close(work); drain();
            CHECK(!task_count && !group_count);
            for (unsigned i=1;i<table.maxfd;++i) CHECK(!table.fds[i]);
        }
    }
    dfs_close_slot(&table,0); epd_app_free(table.fds);
    #undef table
}
