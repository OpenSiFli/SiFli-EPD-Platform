#define HOST_SERVICE
#include "host.h"
typedef struct { bool stopped; } epd_service_t;
typedef struct {
    void (*run)(epd_service_t *);
    unsigned stack_size, priority;
    bool (*ui_process)(epd_service_t *, bool);
} epd_background_t;
enum {
    EPD_SERVICE_UI_DISPLAY = 1u << 0,
    EPD_SERVICE_UI_SETTINGS = 1u << 4,
    EPD_SERVICE_UI_STORAGE = 1u << 5,
    EPD_SERVICE_UI_FILES = 1u << 11
};
static epd_service_t service;
static bool epd_service_cancelled(epd_service_t*s){return !s||s->stopped;}
static void epd_service_wake(epd_service_t*s){(void)s;}
static void epd_service_ui_watch(epd_service_t *s, uint32_t events) { CHECK(s && events); }
static void epd_service_ui_request(epd_service_t *s, uint32_t delay) { CHECK(s); (void)delay; }
void gallery_ui_update(void) {}
static int rt_tick_from_millisecond(int n){return n;}
static void epd_service_wait(epd_service_t*s,int n){(void)s;(void)n;}
static void rt_thread_mdelay(int n){(void)n;CHECK(false);}
static void*dlmodule_find(const char*n){(void)n;return &service;}
static epd_service_t*app_service_start(const char*n,const epd_background_t*d,void*m){(void)n;(void)d;(void)m;return &service;}
static void app_service_stop(const char*n){(void)n;service.stopped=true;}
#include "decode.inc"
#include "store.inc"
#include "catalog.inc"
static void raster_decoder_clear(raster_decoder_t*d){CHECK(d->fd==-1);epd_app_free(d->gray);memset(d,0,sizeof(*d));d->fd=-1;}
#define gallery_decoder_clear raster_decoder_clear
#define allocation_cancelled service_allocation_cancelled
uint32_t gallery_submit_view(gallery_job_kind_t,const char*,unsigned,bool);
#include "service.inc"
#undef allocation_cancelled
static void process_requested(const gallery_job_t *job) {
    epd_activity_work_t *work = requested_work; requested_work = NULL;
    CHECK(process_job(job, work)); epd_activity_work_close(work);
}
static gallery_result_t*request(gallery_job_kind_t kind,const char*path) {
    uint32_t serial=gallery_submit(kind,path,0);CHECK(serial&&pending);gallery_job_t job=requested;pending=false;process_requested(&job);gallery_result_t*r=gallery_take(serial);CHECK(r);return r;
}
static void cancel_decode(void){gallery_cancel();}
static void locate(const char*anchor) {
    uint32_t serial=gallery_submit_location("/sdcard/photos",anchor,false,1);gallery_job_t job=requested;pending=false;process_requested(&job);gallery_result_t*r=gallery_take(serial);CHECK(r&&!r->error_code);gallery_result_free(r);
}
int main(void) {
    add("/sdcard",true,0);add("/sdcard/photos",true,0);add("/sdcard/photos/a.jpg",false,0);add("/sdcard/photos/b.jpg",false,0);add("/sdcard/photos/c.jpg",false,0);
    CHECK(gallery_start());active_service=&service;
    gallery_result_t*image=request(GALLERY_IMAGE,"/sdcard/photos/a.jpg");CHECK(image->pixels&&!image->error_code&&saving&&image->references==2);
    CHECK(!find_file(GALLERY_STORE "/render.tmp")); /* Published before any cache write. */
    gallery_result_free(image);while(gallery_cache_step()){}CHECK(!live&&!open_fds);
    gallery_result_t*scan=request(GALLERY_SCAN,"/sdcard/photos");gallery_result_free(scan);while(gallery_catalog_step()){}
    locate("/sdcard/photos/a.jpg");CHECK(prefetch_pending);unsigned before=decodes;background_step();CHECK(prefetched&&decodes==before+1&&!strcmp(prefetched->path,"/sdcard/photos/b.jpg"));
    const uint8_t*pixels=prefetched->image.data;image=request(GALLERY_IMAGE,"/sdcard/photos/b.jpg");CHECK(image->image.data==pixels&&decodes==before+1&&!prefetched);gallery_result_free(image);while(gallery_cache_step()){}
    locate("/sdcard/photos/b.jpg");background_step();CHECK(prefetched&&!strcmp(prefetched->path,"/sdcard/photos/c.jpg"));++content_token;before=decodes;image=request(GALLERY_IMAGE,"/sdcard/photos/c.jpg");CHECK(image->pixels&&decodes==before+1);gallery_result_free(image);gallery_cache_clear();
    locate("/sdcard/photos/a.jpg");gallery_cancel();before=decodes;background_step();CHECK(!prefetch_pending&&decodes==before&&!prefetched);
    locate("/sdcard/photos/a.jpg");decode_error=true;++content_token;find_file("/sdcard/photos/b.jpg")->data[0]^=1;before=decodes;background_step();CHECK(!prefetched&&!prefetch_pending);unsigned failed_decodes=decodes;
    for(unsigned i=0;i<5;++i)background_step();CHECK(decodes==failed_decodes);
    locate("/sdcard/photos/a.jpg");background_step();CHECK(decodes==failed_decodes+1);decode_error=false;
    ++content_token;locate("/sdcard/photos/a.jpg");background_step();CHECK(prefetched);present=false;++card;background_step();CHECK(!prefetched&&!saving);present=true;
    gallery_cache_clear();prefetch_clear();
    find_file("/sdcard/photos/a.jpg")->data[0]^=1;++content_token;
    uint32_t cancelled_serial=gallery_submit(GALLERY_IMAGE,"/sdcard/photos/a.jpg",0);gallery_job_t cancelled_job=requested;pending=false;decode_hook=cancel_decode;process_requested(&cancelled_job);decode_hook=NULL;CHECK(!completed&&!gallery_take(cancelled_serial)&&!saving);
    locate("/sdcard/photos/a.jpg");fail_at=allocations+1;background_step();fail_at=0;CHECK(!prefetched&&!prefetch_pending);before=allocations;
    for(unsigned i=0;i<5;++i)background_step();CHECK(allocations==before);
    locate("/sdcard/photos/a.jpg");background_step();CHECK(prefetched);
    gallery_cancel();prefetch_clear();gallery_cache_clear();gallery_catalog_clear();CHECK(!open_fds&&!live&&!locks);
    int held=gallery_file_open("/sdcard/photos/a.jpg",O_RDONLY,card);CHECK(held>=0);
    close_permanent=true;CHECK(!gallery_file_close(held)&&owned_find(held));
    before=close_calls;gallery_stop();run(&service);
    CHECK(!worker&&!active_service&&!live&&!locks&&!activity_count&&close_calls==before&&open_fds==1);
    CHECK(gallery_start());service.stopped=false;active_service=&service;
    close_permanent=false;++card;CHECK(storage_file_release(STORAGE_SD,1)&&!open_fds);
    printf("gallery service: %u checks; publication before write, single-slot prefetch hit/cancel/invalidation/failure, card removal and reference release\n",checks);return 0;
}
