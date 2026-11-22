/* The production record queue runs against real owner, credit and activity
 * lifetimes. Spool bytes and service notifications are deterministic doubles. */
#define document_t record_document_t
#define cancelled record_cancelled
enum { DOCUMENT_TEXT=1, DOCUMENT_PARAGRAPH, DOCUMENT_ANCHOR, DOCUMENT_LINK, DOCUMENT_IMAGE, DOCUMENT_METADATA };
typedef struct record_document record_document_t;
typedef struct { bool memory_failed, reserve_denied; } document_reader_t;
#include "record_queue_types.inc"
struct record_document {
    unsigned serial;
    char cache[STORAGE_PATH_MAX];
    bool stopped, allow_flow;
    document_progress_t status;
    document_reader_t *record_reader;
};
static document_record_request_t *record_requests;
static unsigned record_allocating, record_reads;
static bool record_open_needed, record_create_failed;
static epd_memory_credit_t record_attempt_credit;
static void *record_file;
static bool worker_stopping;
static epd_service_t *worker_service;
static void epd_service_wake(epd_service_t *service) { (void)service; }
static void wake_worker(void) {}
static void worker_changed(void) {}
static bool cancelled(record_document_t *d) { return d->stopped; }
static bool flow_allows(record_document_t *d) { return d->allow_flow; }
static bool epd_reader_suspended(void) { return false; }
static void *allocate(size_t bytes) { return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void document_progress(record_document_t *d, document_progress_t *p)
{ *p=d->status; if(d->stopped)p->result=BOOK_CANCELLED; }
static document_reader_t *document_reader_create(record_document_t *d)
{ (void)d;return record_create_failed ? NULL : allocate(sizeof(document_reader_t)); }
static bool document_reader_record(document_reader_t *reader,uint32_t offset,document_record_t *record)
{
    (void)offset;CHECK(reader);
    if(record_open_needed && !record_file){
        storage_lock();
        int fd=reader_file_open_status(&pressure_document,"/sdcard/cache/record",O_RDONLY,&reader->reserve_denied);
        storage_unlock();
        if(fd<0){reader->memory_failed=open_pressure(errno);return false;}
        record_file=epd_app_alloc(sizeof(FIL),EPD_APP_SRAM);CHECK(record_file);
    }
    memset(record,0,sizeof(*record));record->kind=DOCUMENT_TEXT;record->length=4;return true;
}
static bool document_reader_read(document_reader_t *reader,uint32_t offset,void *out,size_t bytes)
{ (void)offset;CHECK(reader&&bytes==4);memcpy(out,"text",4);++record_reads;return true; }
static bool document_reader_image_info(document_reader_t *reader,uint32_t offset,document_image_t *image)
{ (void)reader;(void)offset;(void)image;CHECK(false);return false; }
static bool record_cached(document_record_request_t *job,uint32_t end)
{ (void)job;(void)end;return false; }
#include "record_queue_functions.inc"
static void record_activity_case(const char *name)
{
    bool foreground=!strcmp(name,"record_foreground"), cancel=!strcmp(name,"record_cancel");
    bool denied=!strcmp(name,"record_denied"), error=!strcmp(name,"record_error");
    bool sram_reuse=!strcmp(name,"record_sram_reuse");
    bool recover=!strcmp(name,"record_open_recover"), exhausted=!strcmp(name,"record_open_exhausted");
    bool io_error=!strcmp(name,"record_open_error"), seek_oom=!strcmp(name,"record_seek_oom");
    bool retry_timeout=!strcmp(name,"record_competing_timeout_retry");
    bool competing=retry_timeout || !strcmp(name,"record_competing_release") || !strcmp(name,"record_competing_cancel");
    bool cancel_pending=!strcmp(name,"record_competing_cancel");
    bool seek_release=!strcmp(name,"record_seek_competing_release");
    competing=competing||seek_release;
    record_open_needed=competing||recover||exhausted||io_error||seek_oom;
    record_create_failed=!strcmp(name,"record_reader_oom");
    epd_activity_work_t *held[3]={0};
    void *occupied=NULL;
    epd_memory_owner_t service=epd_memory_scope_create(app,EPD_MEMORY_SCOPE_SERVICE);
    epd_memory_owner_t worker=epd_memory_scope_create(service,EPD_MEMORY_SCOPE_TASK);
    CHECK(service!=EPD_MEMORY_OWNER_INVALID&&worker!=EPD_MEMORY_OWNER_INVALID);
    epd_activity_work_t *caller=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,65536,4096,NULL,NULL);
    CHECK(caller);drain();epd_activity_guard_t guard;CHECK(epd_activity_work_ready(caller)==1&&epd_activity_work_enter(caller,&guard));
    record_document_t doc={0};strcpy(doc.cache,"/sdcard/cache/record");doc.status.complete=!competing;doc.status.end=sizeof(document_record_t)+4;
    document_record_request_t *job=document_record_request(&doc,0,foreground);CHECK(job);
    CHECK(document_record_result(job)==BOOK_OK && document_record_result(NULL)==BOOK_CANCELLED);
    if(seek_oom||seek_release){job->seek_end=doc.status.end;job->seek_target=1;}
    epd_activity_work_leave(&guard);
    epd_activity_work_close(caller);drain();
    if(recover){
        size_t peak=epd_resource_file_io_peak(STORAGE_PATH_MAX);
        idle_input=epd_app_alloc(peak,EPD_APP_SRAM);CHECK(idle_input);
        CHECK(epd_memory_owner_set_limit(app,backends[0],peak+32u)==EPD_MEMORY_OK);
    }
    if(exhausted||seek_oom)CHECK(epd_memory_owner_set_limit(app,backends[0],4096)==EPD_MEMORY_OK);
    if(io_error)document_open_error=EIO;
    if(sram_reuse){
        size_t peak=epd_resource_file_io_peak(STORAGE_PATH_MAX);
        epd_memory_owner_t previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
        occupied=epd_app_alloc(CAPACITY-3*peak-4996u-32u,EPD_APP_SRAM);CHECK(occupied);
        epd_memory_owner_leave(previous);
        for(unsigned i=0;i<3;++i){
            held[i]=epd_activity_work_queue(EPD_RESOURCE_OPTIONAL,0,4096,peak,NULL,NULL);
            CHECK(held[i]);drain();CHECK(epd_activity_work_ready(held[i])==1);
        }
        epd_memory_backend_snapshot_t heap;CHECK(epd_memory_backend_get(backends[0],&heap));
        CHECK(heap.available-heap.usage.unused==4996u && peak>4996u);
    }
    if(competing){
        epd_memory_owner_t previous=epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
        occupied=epd_app_alloc(CAPACITY-43168u-32u,EPD_APP_SRAM);CHECK(occupied);
        epd_memory_owner_leave(previous);
        held[0]=epd_activity_work_queue(EPD_RESOURCE_CONTINUOUS,0,4096,40000,NULL,NULL);
        CHECK(held[0]);drain();CHECK(epd_activity_work_ready(held[0])==1);
        epd_memory_backend_snapshot_t heap;CHECK(epd_memory_backend_get(backends[0],&heap));
        CHECK(heap.available==43168u && heap.usage.unused==40000u);
        CHECK(epd_resource_file_io_peak(strlen(doc.cache)+1u)>3168u);
    }
    /* The caller may finish and the flow gate may remain closed for seconds. */
    ticks+=rt_tick_from_millisecond(2000);
    actor=&actors[2];CHECK(rt_thread_mem_context_set(NULL,worker)==0);
    if(cancel){document_record_cancel(job);CHECK(!record_work());}
    else {
        if(!foreground)CHECK(!record_work());
        doc.allow_flow=true;
        if(error)doc.status.result=BOOK_CANCELLED;
        if(denied)CHECK(epd_memory_owner_set_limit(app,backends[1],physical[1]+4096)==EPD_MEMORY_OK);
        if(competing){
            CHECK(!record_work());drain();
            CHECK(record_work());
            if(job->done){CHECK(document_record_result(job)==BOOK_NO_MEMORY);CHECK(pressure_reclaims==1&&!document_opens);}
            CHECK(epd_memory_owner_current()==worker && epd_memory_credit_current()==EPD_MEMORY_OWNER_INVALID);
            CHECK(record_attempt_credit!=EPD_MEMORY_OWNER_INVALID);
            drain();
            epd_memory_credit_snapshot_t retired;
            CHECK(!epd_memory_credit_get(record_attempt_credit,&retired) ||
                  (retired.state==EPD_MEMORY_CREDIT_ENDED && !retired.unused[backends[0]]));
            CHECK(!job->done && "RESERVE_DENIAL_MUST_WAIT_FOR_FILE_ADMISSION");
            CHECK(!record_work());drain();
            CHECK(!job->done&&!record_reads&&!document_opens&&job->work);
            CHECK(epd_activity_work_ready(job->work)==0);
            if(cancel_pending){document_record_cancel(job);CHECK(!record_work());goto cleanup;}
            if(retry_timeout){
                ticks+=rt_tick_from_millisecond(501);drain();
                CHECK(record_work() && job->done && document_record_result(job)==BOOK_NO_MEMORY);
                CHECK(!document_opens && !record_reads && doc.status.result==BOOK_OK);
                document_record_cancel(job);
                document_record_request_t *released=job;
                job=document_record_request(&doc,0,false);CHECK(job==released);
                CHECK(!job->file_admission&&!job->work&&!job->done&&!job->seek_end);
            }
            epd_activity_work_close(held[0]);held[0]=NULL;drain();
        }
        for(unsigned i=0;i<8&&!job->done;++i){
            record_work();
            if(job->work){
                epd_resource_activity_snapshot_t activity;
                CHECK(epd_resource_activity_get(epd_activity_work_activity(job->work), &activity)==0);
                epd_memory_owner_snapshot_t owner;CHECK(epd_memory_owner_get(activity.owner,&owner));
                CHECK(owner.parent==worker&&owner.account==app);
            }
            drain();
            if((sram_reuse || exhausted || seek_oom) && !job->done)ticks+=rt_tick_from_millisecond(150);
        }
        document_record_data_t data;
        if(error||denied||exhausted||seek_oom||record_create_failed||io_error){
            int expected=error?BOOK_CANCELLED:io_error?BOOK_IO_ERROR:BOOK_NO_MEMORY;
            CHECK(document_record_take(job,&data)==-1&&document_record_result(job)==expected);
            CHECK(!record_reads&&!job->work);
            CHECK(doc.status.result==BOOK_OK || error);
            if(exhausted||seek_oom)CHECK(pressure_reclaims==1&&!document_opens);
            if(io_error)CHECK(!pressure_reclaims&&document_opens==1);
            for(unsigned i=0;i<100;++i)CHECK(!record_work());
        }else if(seek_release){
            CHECK(job->done&&document_record_result(job)==BOOK_OK);
            CHECK(job->seek_headers==1 && document_opens==1 && !record_reads);
        }else{
            CHECK(document_record_take(job,&data)==1&&"RECORD_MUST_OUTLIVE_CALLER_ACTIVITY");
            CHECK(record_reads==1&&!memcmp(data.text,"text",4));
            CHECK(document_record_result(job)==BOOK_OK);
            if(sram_reuse)CHECK(!document_opens&&!pressure_reclaims);
            if(recover)CHECK(pressure_reclaims==1&&document_opens==1&&!idle_input);
        }
        document_record_cancel(job);
        CHECK(document_record_result(job)==BOOK_CANCELLED);
    }
cleanup:
    record_collect();drain();CHECK(!record_requests&&!record_allocating);
    epd_app_free(doc.record_reader);
    epd_app_free(record_file);
    for(unsigned i=0;i<3;++i)epd_activity_work_close(held[i]);
    epd_app_free(occupied);drain();
    CHECK(rt_thread_mem_context_set(NULL,app)==0);
    epd_memory_owner_close(worker);epd_memory_owner_release(worker);
    epd_memory_owner_close(service);epd_memory_owner_release(service);
    CHECK(!epd_memory_owner_get(worker,&(epd_memory_owner_snapshot_t){0}));
    CHECK(!epd_memory_owner_get(service,&(epd_memory_owner_snapshot_t){0}));
    CHECK(!physical[0]&&!physical[1]&&!task_count&&!group_count);
}
#undef cancelled
#undef document_t
