#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while(0)
#define EPD_APP_PSRAM 1
typedef void epd_service_t;
static bool epd_service_sleep_acquire(epd_service_t *);
static bool epd_service_reclaim_hold(epd_service_t *owner) { return owner != NULL; }
static void epd_service_reclaim_release(epd_service_t *owner) { (void)owner; }
static bool epd_service_mark_dirty(epd_service_t *owner) { return owner != NULL; }
#include "../unified_operation/cleanup_ports.h"
#include "app_work.inc"
static unsigned checks,locks,wakes,requests,allocations;
static bool fail_alloc;
static bool admit=true;
static void lock(void){CHECK(!locks);++locks;}
static void unlock(void){CHECK(locks==1);--locks;}
static void *epd_app_alloc(size_t bytes,int region)
{CHECK(region==EPD_APP_PSRAM);if(fail_alloc)return NULL;void *p=calloc(1,bytes);CHECK(p);++allocations;return p;}
static void epd_app_free(void *p){if(p){CHECK(allocations);--allocations;free(p);}}
#define music_model_free epd_app_free
typedef struct {char *path,*name;} music_library_entry_t;
typedef struct {music_library_entry_t *entries;size_t count,capacity,string_bytes;} music_library_t;
typedef struct {uint16_t *pixels;unsigned width,height;} music_cover_image_t;
typedef struct {uint32_t track_id;} cover_job_t;
static struct {
    bool initialized,active,stopping;
    cover_job_t *pending,*ready;
    music_cover_image_t image;
    uint32_t revision;
} cover;
static bool initialized=true,ui_active,cover_refresh,session_active,has_pending_media;
static app_work_t work;
static bool accepting=true,retire_ready;
static unsigned flush_requested,flush_completed;
static int guard;
#define RT_EOK 0
static int rt_mutex_take(int *mutex,int delay){CHECK(mutex==&guard&&!delay);lock();return RT_EOK;}
static void *commands,*worker=(void*)1,*scan;
static music_library_t library;
static struct {uint32_t current_id;unsigned state;bool playing;} working;
static struct {char *path,*cover_path;} metadata;
static struct {uint64_t position;bool dirty;void *queue,*backend,*media_owner;} playback;
static void invalidate(void){}
static void epd_service_wake(void *owner){CHECK(owner==worker);++wakes;}
static bool epd_service_sleep_acquire(void *owner){CHECK(owner==worker);return admit;}
static void music_view_update(void){}
static void music_view_stop(void){}
static bool music_view_present(void){return true;}
static void epd_service_ui_request(void *service,unsigned delay){CHECK(service==worker&&!delay);}
static void app_service_stop(const char *id){(void)id;CHECK(false);}
static int rt_kprintf(const char *line){return printf("%s",line);}
static bool music_cover_request(uint32_t id,const char *path,const char *fallback)
{CHECK(id==working.current_id&&path==metadata.path&&fallback==metadata.cover_path);++requests;return true;}
#include "actual.inc"
static void populate(void)
{
    CHECK(!allocations);memset(&cover,0,sizeof(cover));cover.initialized=true;
    cover.ready=epd_app_alloc(sizeof(*cover.ready),EPD_APP_PSRAM);cover.ready->track_id=7;
    cover.image=(music_cover_image_t){epd_app_alloc(16,EPD_APP_PSRAM),4,2};
    for(unsigned i=0;i<8;++i)cover.image.pixels[i]=(uint16_t)(i+100);
    library.entries=epd_app_alloc(sizeof(*library.entries),EPD_APP_PSRAM);library.count=library.capacity=1;
    library.entries[0].path=epd_app_alloc(32,EPD_APP_PSRAM);
    library.entries[0].name=epd_app_alloc(16,EPD_APP_PSRAM);
    ui_active=cover_refresh=has_pending_media=false;commands=scan=NULL;working.current_id=7;
}
static void copies_and_worker(void)
{
    populate();music_cover_image_t ui={0};CHECK(music_cover_copy(7,&ui));
    CHECK(ui.pixels!=cover.image.pixels&&allocations==6);
    music_ui_active(true);trim_caches();CHECK(library.count&&cover.image.pixels);
    music_ui_active(false);cover.active=true;trim_caches();CHECK(!library.entries&&cover.image.pixels&&cover.ready);
    cover.active=false;cover.pending=cover.ready;trim_caches();CHECK(cover.ready&&cover.image.pixels);
    cover.pending=NULL;cover.stopping=true;trim_caches();CHECK(cover.ready&&cover.image.pixels);
    cover.stopping=false;trim_caches();CHECK(!cover.ready&&!cover.image.pixels&&allocations==1);
    for(unsigned i=0;i<8;++i)CHECK(ui.pixels[i]==i+100);
    music_cover_image_free(&ui);CHECK(!allocations);
}
int main(void)
{
    copies_and_worker();
    for(unsigned state=0;state<8;++state){
        populate();working.state=state;working.playing=state==2;session_active=true;
        playback.position=123456;playback.dirty=true;playback.queue=(void*)2;playback.backend=(void*)3;playback.media_owner=worker;
        commands=(void*)4;trim_caches();CHECK(library.count&&cover.ready);
        commands=NULL;has_pending_media=true;trim_caches();CHECK(library.count&&cover.ready);
        has_pending_media=false;scan=(void*)5;trim_caches();CHECK(library.count&&!cover.ready);
        scan=NULL;trim_caches();CHECK(!library.entries&&!allocations);
        CHECK(working.state==state&&working.playing==(state==2)&&session_active);
        CHECK(playback.position==123456&&playback.dirty&&playback.queue==(void*)2&&playback.backend==(void*)3&&playback.media_owner==worker);
        unsigned before=requests,awakes=wakes;music_ui_active(true);trim_caches();
        CHECK(requests==before+1&&wakes==awakes+1);music_ui_active(true);trim_caches();CHECK(requests==before+1);
        music_ui_active(false);trim_caches();CHECK(!allocations);
    }
    populate();music_cover_image_t copy={0};fail_alloc=true;CHECK(!music_cover_copy(7,&copy)&&!copy.pixels);fail_alloc=false;
    trim_caches();CHECK(!allocations);
    unsigned before=requests;admit=false;music_ui_active(true);trim_caches();CHECK(cover_refresh&&requests==before);
    unsigned before_wakes=wakes;CHECK(!ui_process(worker,false)&&wakes==before_wakes);
    /* Platform resume wakes service workers after sleep admission reopens. */
    admit=true;epd_service_wake(worker);trim_caches();CHECK(!cover_refresh&&requests==before+1);
    printf("PASS music cache ownership: %u checks\n",checks);return 0;
}
