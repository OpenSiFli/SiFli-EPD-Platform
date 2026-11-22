/* Production I/O state machines with the real core, runtime and activity helper.
 * Storage doubles retain actual SDK FIL/dfs_fd-sized allocations until close. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include "epd_cold_budget.h"
/* Resident manager metadata is separate from the synthetic payload heaps. */
static void *queued_control_memory;
void *epd_app_realloc(void *old,size_t bytes,epd_app_memory_t region)
{
    CHECK(!old&&!queued_control_memory&&region==EPD_APP_PSRAM&&
          epd_memory_owner_current()==EPD_MEMORY_OWNER_SYSTEM);
    queued_control_memory=calloc(1,bytes);CHECK(queued_control_memory);
    return queued_control_memory;
}
#define STORAGE_PATH_MAX 512
#define STORAGE_SD 1
#define STORAGE_FLASH 0
#define COVER_DISK_RESERVE (2u*1024u*1024u)
#define ENTRY_DISK_RESERVE COVER_DISK_RESERVE
#define COVER_CHUNK 4096u
#define ENTRY_IO_BYTES 4096u
#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RGB565 2
typedef int epd_service_t;
typedef int storage_volume_t;
typedef struct { uint64_t used, total; } storage_info_t;
typedef struct { unsigned value; } book_file_version_t;
typedef struct { uint32_t checksum, bytes, width, height, stride; } io_header_t;
typedef struct { uint8_t *data; } entry_pixels_t;
typedef struct { entry_pixels_t *pixels; } reader_entry_t;
typedef struct {
    epd_activity_work_t *work;
    epd_cold_lease_t disk_lease;
    io_header_t header;
    unsigned key;
    book_file_version_t source, cached;
    entry_pixels_t *pixels;
    struct { struct { const uint8_t *data; struct { unsigned magic, cf, w, h, stride; } header; } image; } bitmap;
    char path[STORAGE_PATH_MAX], temporary[STORAGE_PATH_MAX];
    size_t offset;
    uint32_t epoch, session, checksum;
    storage_volume_t volume;
    int phase, fd;
    bool writing, temporary_owned, hit;
} cover_job_t;
typedef cover_job_t entry_store_job_t;
enum { COVER_OPEN, COVER_HEADER, COVER_PIXELS, COVER_SEAL, COVER_SYNC, COVER_CLOSE, COVER_COMMIT, COVER_ABORT };
enum { ENTRY_OPEN, ENTRY_HEADER, ENTRY_PIXELS, ENTRY_SEAL, ENTRY_SYNC, ENTRY_CLOSE, ENTRY_COMMIT, ENTRY_ABORT };
static struct { cover_job_t *active, *ready; unsigned epoch, errors, saved; } cache;
static struct { entry_store_job_t *job; reader_entry_t *ready; bool active, cancel_requested; } entry_store;
static unsigned io_lock, io_opens, io_closes, io_commits, io_aborts, io_reads, io_writes;
static uint8_t io_disk[32768];
static size_t io_size, io_offset;
static void *io_fil, *io_path, *io_fd;
static bool io_done;
static epd_cold_lease_t io_lease;
static unsigned io_leases_started,io_leases_finished,io_leases_succeeded;
/* Disk capacity is available in these SRAM admission cases. Track the lease
 * through close/publication; the cold-budget suite covers disk eviction. */
int epd_cold_budget_begin(const char *path,epd_cold_class_t kind,size_t bytes,
                          uint32_t session,epd_cold_lease_t *lease)
{
    CHECK(!io_lock&&!io_fil&&!io_lease&&path&&*path&&lease);
    CHECK(kind==EPD_COLD_DERIVED&&session==1);
    CHECK(!bytes||bytes==sizeof(io_header_t)+16384u);
    io_lease=++io_leases_started;*lease=io_lease;
    return EPD_COLD_BUDGET_OK;
}
int epd_cold_budget_finish(epd_cold_lease_t lease,epd_cold_class_t kind,bool success)
{
    CHECK(!io_lock&&!io_fil&&io_lease&&lease==io_lease&&kind==EPD_COLD_DERIVED);
    io_lease=0;++io_leases_finished;if(success)++io_leases_succeeded;
    return EPD_COLD_BUDGET_OK;
}
bool epd_cold_budget_temporary(const char *path,char *out,size_t capacity)
{
    CHECK(io_lease&&path&&out);
    int length=snprintf(out,capacity,"%s.tmp",path);
    return length>0&&(size_t)length<capacity;
}
static void storage_lock(void) { ++io_lock; }
static void storage_unlock(void) { CHECK(io_lock); --io_lock; }
static bool storage_available(int volume) { (void)volume; return true; }
static uint32_t storage_card_session(void) { return 1; }
static bool storage_session_valid(int volume, uint32_t session) { (void)volume; return session == 1; }
static bool storage_info(int volume, storage_info_t *out) { (void)volume; *out=(storage_info_t){0,10000000}; return true; }
static bool directory_pressure;
static unsigned directory_calls;
static int directory_error;
static bool storage_mkdirs(const char *path)
{
    (void)path;
    if(!directory_pressure)return true;
    CHECK(io_lock);++directory_calls;
    if(directory_error){errno=directory_error;return false;}
    void *fil=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);
    void *fd=fil?epd_app_alloc(sizeof(struct dfs_fd),EPD_APP_SRAM):NULL;
    void *name=fd?epd_app_alloc(STORAGE_PATH_MAX,EPD_APP_SRAM):NULL;
    bool ok=fil&&fd&&name;
    epd_app_free(name);epd_app_free(fd);epd_app_free(fil);
    if(!ok)errno=ENOMEM;
    return ok;
}
static bool storage_file_recover(const char *path) { (void)path; return true; }
static bool book_file_version_valid(const char *path,const book_file_version_t *v) { (void)path;(void)v;return true; }
static book_file_version_t book_file_version(const char *path) { (void)path;return (book_file_version_t){1}; }
static int storage_file_owned_open(const char *path, int flags, bool temporary)
{
    (void)path; (void)temporary; CHECK(io_lock && !io_fil && io_lease);
    io_fil=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);
    io_fd=epd_app_alloc(sizeof(struct dfs_fd),EPD_APP_SRAM);
    io_path=epd_app_alloc(STORAGE_PATH_MAX,EPD_APP_SRAM);
    CHECK(io_fil && io_fd && io_path); ++io_opens;io_offset=0;
    if(flags&O_TRUNC) io_size=0;
    return 1;
}
static bool storage_file_owned_close(int fd)
{
    CHECK(io_lock && fd==1 && io_fil);
    epd_app_free(io_fil);epd_app_free(io_fd);epd_app_free(io_path);
    io_fil=io_fd=io_path=NULL;++io_closes;return true;
}
static bool storage_file_commit(const char *temporary,const char *path)
{
    (void)temporary;(void)path;CHECK(io_lock && !io_fil && io_lease);
    /* Rename's path copies and long-name workspace are still admitted. */
    void *a=epd_app_alloc(512,EPD_APP_SRAM),*b=epd_app_alloc(512,EPD_APP_SRAM),*lfn=epd_app_alloc(512,EPD_APP_SRAM);
    CHECK(a&&b&&lfn);epd_app_free(a);epd_app_free(b);epd_app_free(lfn);++io_commits;return true;
}
static int io_read(int fd,void *out,size_t bytes)
{
    CHECK(fd==1&&io_fil&&io_lock);if(bytes>io_size-io_offset)bytes=io_size-io_offset;
    memcpy(out,io_disk+io_offset,bytes);io_offset+=bytes;++io_reads;return (int)bytes;
}
static int io_write(int fd,const void *data,size_t bytes)
{
    CHECK(fd==1&&io_fil&&io_lock&&bytes<=sizeof(io_disk)-io_offset);
    memcpy(io_disk+io_offset,data,bytes);io_offset+=bytes;if(io_offset>io_size)io_size=io_offset;
    ++io_writes;return (int)bytes;
}
static long io_seek(int fd,long offset,int origin) { CHECK(fd==1&&io_fil&&origin==SEEK_SET&&offset==0);io_offset=0;return 0; }
static int io_sync(int fd)
{
    CHECK(fd==1&&io_fil&&io_lock);
    void *lfn=epd_app_alloc((FF_MAX_LFN+1u)*2u,EPD_APP_SRAM);CHECK(lfn);epd_app_free(lfn);return 0;
}
static bool cover_source_valid(const cover_job_t *j) { (void)j;return true; }
static bool cover_cancelled(const cover_job_t *j,epd_service_t *s) { (void)j;(void)s;return false; }
static bool cover_path(const unsigned *key,int v,char *out,size_t n) { (void)key;(void)v;snprintf(out,n,"/sdcard/cache/frame");return true; }
static void cover_own_change(cover_job_t *j) { (void)j; }
static bool cover_header_valid(const cover_job_t *j) { return j->header.bytes==16384; }
static uint32_t cover_checksum(uint32_t sum,const void *p,size_t n) { const uint8_t *b=p;while(n--)sum=(sum^*b++)*16777619u;return sum; }
static bool cover_pixels(cover_job_t *j,size_t n) { j->bitmap.image.data=epd_app_alloc(n,EPD_APP_PSRAM);return j->bitmap.image.data!=NULL; }
static void cover_pixels_release(cover_job_t *j) { epd_app_free((void *)j->bitmap.image.data);j->bitmap.image.data=NULL; }
static bool cover_cleanup(cover_job_t *j) { ++io_aborts;if(j->fd>=0){storage_lock();storage_file_owned_close(j->fd);storage_unlock();j->fd=-1;}return true; }
static void cover_free(cover_job_t *j) { if(j)cover_pixels_release(j);io_done=true; }
static void cover_resource_publish(void) {}
static void epd_service_ui_request(epd_service_t *s,unsigned flags) { (void)s;(void)flags; }
static bool entry_job_source_valid(const entry_store_job_t *j) { return cover_source_valid(j); }
static bool entry_job_cancelled(const entry_store_job_t *j,epd_service_t *s) { return cover_cancelled(j,s); }
static bool entry_store_path(const unsigned *key,int v,char *out,size_t n) { return cover_path(key,v,out,n); }
static void entry_job_own_change(entry_store_job_t *j) { (void)j; }
static bool entry_job_header_valid(const entry_store_job_t *j) { return cover_header_valid(j); }
static uint32_t entry_checksum(uint32_t sum,const void *p,size_t n) { return cover_checksum(sum,p,n); }
static entry_pixels_t *entry_pixels_new(size_t n)
{ entry_pixels_t *p=epd_app_alloc(sizeof(*p),EPD_APP_PSRAM);CHECK(p);p->data=epd_app_alloc(n,EPD_APP_PSRAM);CHECK(p->data);return p; }
static void entry_pixels_release(entry_pixels_t *p) { if(p){epd_app_free(p->data);epd_app_free(p);} }
static bool entry_job_cleanup(entry_store_job_t *j) { return cover_cleanup(j); }
static reader_entry_t *entry_job_result(entry_store_job_t *j)
{ reader_entry_t *r=epd_app_alloc(sizeof(*r),EPD_APP_PSRAM);CHECK(r);r->pixels=j->pixels;j->pixels=NULL;return r; }
static bool entry_read_current(const entry_store_job_t *j) { (void)j;return true; }
static void entry_job_free(entry_store_job_t *j) { entry_pixels_release(j->pixels);j->pixels=NULL;epd_activity_work_close(j->work);j->work=NULL;io_done=true; }
#define read io_read
#define write io_write
#define lseek io_seek
#define fsync io_sync
#include "io_phase_functions.inc"
#undef read
#undef write
#undef lseek
#undef fsync

typedef struct { unsigned serial;bool stopped; } document_t;
static void *idle_input;
static unsigned pressure_reclaims, document_opens;
static bool cancel_release;
static int document_open_error;
static bool cancelled(document_t *d) { return d->stopped; }
static document_t pressure_document;
static unsigned cache_inputs_release(void)
{
    CHECK(io_lock);++pressure_reclaims;
    if(cancel_release)pressure_document.stopped=true;
    if(!idle_input)return 0;epd_app_free(idle_input);idle_input=NULL;return 1;
}
static void rt_memory_info(uint32_t *total,uint32_t *used,uint32_t *maximum) { *total=CAPACITY;*used=(uint32_t)physical[0];(void)maximum; }
static void rt_kprintf(const char *format,...) { (void)format; }
static int document_open(const char *path,int flags,int mode)
{ (void)path;(void)flags;(void)mode;++document_opens;if(document_open_error){errno=document_open_error;return -1;}return 7; }
#define open document_open
#include "document_open_functions.inc"
#undef open
#include "document_directory_functions.inc"
static void directory_pressure_case(void)
{
    size_t peak=epd_resource_file_io_peak(STORAGE_PATH_MAX);
    directory_pressure=true;
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,peak,NULL,NULL);
    CHECK(work);drain();epd_activity_guard_t guard;CHECK(epd_activity_work_ready(work)==1&&epd_activity_work_enter(work,&guard));
    idle_input=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);CHECK(idle_input);
    storage_lock();CHECK(!storage_mkdirs("/sdcard/cache")&&errno==ENOMEM);
    CHECK(reader_directory_create(&pressure_document,"/sdcard/cache"));storage_unlock();
    CHECK(directory_calls==2&&!pressure_reclaims&&idle_input);
    epd_app_free(idle_input);idle_input=NULL;
    epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();
    work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,512,NULL,NULL);
    CHECK(work);drain();CHECK(epd_activity_work_ready(work)==1&&epd_activity_work_enter(work,&guard));
    epd_memory_owner_t previous=epd_memory_owner_enter(app);
    idle_input=epd_app_alloc(peak,EPD_APP_SRAM);CHECK(idle_input);epd_memory_owner_leave(previous);
    CHECK(epd_memory_owner_set_limit(app,backends[0],peak+32+512)==EPD_MEMORY_OK);
    storage_lock();CHECK(reader_directory_create(&pressure_document,"/sdcard/cache"));storage_unlock();
    CHECK(pressure_reclaims==1&&directory_calls==3&&!idle_input);
    directory_error=EIO;storage_lock();CHECK(!reader_directory_create(&pressure_document,"/sdcard/cache")&&errno==EIO);storage_unlock();
    CHECK(pressure_reclaims==1&&directory_calls==4);
    directory_error=ENOMEM;storage_lock();CHECK(!reader_directory_create(&pressure_document,"/sdcard/cache")&&errno==ENOMEM);storage_unlock();
    CHECK(pressure_reclaims==2&&directory_calls==5);
    pressure_document.stopped=true;storage_lock();CHECK(!reader_directory_create(&pressure_document,"/sdcard/cache")&&errno==ECANCELED);storage_unlock();
    pressure_document.stopped=false;storage_lock();CHECK(!reader_directory_create(&pressure_document,"")&&errno==EINVAL);storage_unlock();
    CHECK(pressure_reclaims==2&&directory_calls==5);
    CHECK(epd_memory_credit_end(epd_memory_credit_current()));
    storage_lock();CHECK(!reader_directory_create(&pressure_document,"/sdcard/cache")&&errno==ECANCELED);storage_unlock();
    CHECK(pressure_reclaims==2&&directory_calls==5);
    epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();
}
static void document_pressure_case(void)
{
    size_t peak=epd_resource_file_io_peak(512);
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,512,NULL,NULL);
    CHECK(work);drain();epd_activity_guard_t guard;CHECK(epd_activity_work_enter(work,&guard));
    epd_memory_owner_t token=epd_memory_credit_current();
    epd_memory_owner_t previous=epd_memory_owner_enter(app);
    idle_input=epd_app_alloc(peak,EPD_APP_SRAM);CHECK(idle_input);epd_memory_owner_leave(previous);
    CHECK(epd_memory_owner_set_limit(app,backends[0],peak+32+512)==EPD_MEMORY_OK);
    CHECK(epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,peak)==EPD_RESOURCE_DENIED);
    storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/book",O_RDONLY)==7);storage_unlock();
    CHECK(pressure_reclaims==1&&document_opens==1&&!idle_input);
    pressure_document.stopped=true;storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/book",0)<0&&errno==ECANCELED);storage_unlock();
    pressure_document.stopped=false;storage_lock();CHECK(reader_file_open(&pressure_document,NULL,0)<0&&errno==EINVAL);storage_unlock();
    CHECK(pressure_reclaims==1&&document_opens==1);
    document_open_error=EIO;storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/book",0)<0&&errno==EIO);storage_unlock();
    CHECK(pressure_reclaims==1&&document_opens==2);
    document_open_error=ENOMEM;storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/book",0)<0);storage_unlock();
    CHECK(pressure_reclaims==2&&document_opens==3);
    CHECK(epd_memory_credit_end(token));
    storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/book",0)<0&&errno==ECANCELED);storage_unlock();
    CHECK(pressure_reclaims==2&&document_opens==3);
    epd_activity_work_leave(&guard);epd_activity_work_close(work);drain();
}
static void document_multiple_open_case(void)
{
    epd_activity_work_t *other=epd_activity_work_queue(EPD_RESOURCE_OPTIONAL,0,4096,4096,NULL,NULL);
    epd_activity_work_t *work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,2288,NULL,NULL);
    CHECK(other&&work);drain();epd_activity_guard_t guard;CHECK(epd_activity_work_enter(work,&guard));
    epd_memory_credit_snapshot_t credit;CHECK(epd_memory_credit_get(epd_memory_credit_current(),&credit));
    epd_memory_backend_snapshot_t heap;CHECK(epd_memory_backend_get(backends[0],&heap));
    CHECK(credit.unused[backends[0]]==2288 && heap.usage.unused==6384);
    CHECK(!epd_app_alloc(sizeof(FIL),EPD_APP_SRAM));
    void *files[2];
    for(unsigned i=0;i<2;++i){
        storage_lock();CHECK(reader_file_open(&pressure_document,"/sdcard/cache/index",O_RDONLY)==7);storage_unlock();
        CHECK(epd_memory_credit_get(epd_memory_credit_current(),&credit));
        CHECK(credit.unused[backends[0]]>=epd_resource_file_io_peak(sizeof("/sdcard/cache/index")));
        files[i]=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);CHECK(files[i]);
    }
    CHECK(!pressure_reclaims&&document_opens==2);
    for(unsigned i=0;i<2;++i)epd_app_free(files[i]);
    epd_activity_work_leave(&guard);epd_activity_work_close(work);epd_activity_work_close(other);drain();
}
static void io_pressure_case(const char *name)
{
    if(!strcmp(name,"io_document_multiple")){document_multiple_open_case();return;}
    if(!strcmp(name,"io_document")){document_pressure_case();return;}
    if(!strcmp(name,"io_directory")){directory_pressure_case();return;}
    bool entry=!strcmp(name,"io_entry");size_t peak=epd_resource_file_io_peak(STORAGE_PATH_MAX);
    CHECK(epd_memory_owner_set_limit(app,backends[0],peak)==EPD_MEMORY_OK);
    for(unsigned pass=0;pass<2;++pass){
        cover_job_t job={0};job.writing=pass==0;job.fd=-1;job.header.bytes=16384;job.header.width=128;job.header.height=128;job.header.stride=128;
        job.work=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,65536,peak,NULL,NULL);CHECK(job.work);drain();
        if(job.writing){if(entry){job.pixels=entry_pixels_new(job.header.bytes);memset(job.pixels->data,0x5a,job.header.bytes);}
            else{CHECK(cover_pixels(&job,job.header.bytes));memset((void *)job.bitmap.image.data,0x5a,job.header.bytes);}}
        cache.active=&job;entry_store.job=&job;io_done=false;
        CHECK(entry?entry_phase_step(&job):cover_phase_step(&job));CHECK(job.fd==1&&io_fil);
        epd_activity_guard_t guard;CHECK(epd_activity_work_enter(job.work,&guard));
        epd_memory_credit_snapshot_t credit;CHECK(epd_memory_credit_get(epd_memory_credit_current(),&credit));
        CHECK(credit.unused[backends[0]]<sizeof(FIL)+32);
        CHECK(!epd_app_alloc(sizeof(FIL),EPD_APP_SRAM));
        CHECK(epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,peak)==EPD_RESOURCE_DENIED);
        epd_activity_work_leave(&guard);
        unsigned steps=0;while(!io_done&&steps++<20)CHECK(entry?entry_phase_step(&job):cover_phase_step(&job));
        CHECK(io_done&&!io_aborts&&!io_fil&&io_opens==pass+1&&io_closes==pass+1&&io_commits==1);
        CHECK(!job.disk_lease&&!io_lease&&io_leases_started==pass+1&&
              io_leases_finished==pass+1&&io_leases_succeeded==pass+1);
        if(!job.writing){
            const uint8_t *pixels=entry?entry_store.ready->pixels->data:cache.ready->bitmap.image.data;
            for(unsigned i=0;i<16384;++i)CHECK(pixels[i]==0x5a);
            if(entry){entry_pixels_release(entry_store.ready->pixels);epd_app_free(entry_store.ready);entry_store.ready=NULL;}
            else{CHECK(cache.ready->hit);cover_pixels_release(cache.ready);cache.ready=NULL;}
        }
        drain();CHECK(!physical[0]&&!physical[1]);
    }
    CHECK(io_reads>=6&&io_writes>=6);
}
