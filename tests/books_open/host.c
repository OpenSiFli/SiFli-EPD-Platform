#include <windows.h>
#undef small
#include "activity_fixture.h"
#include "epd_resource_runtime.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <errno.h>
#define O_RDONLY _O_RDONLY
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define BOOKSHELF_NAME_MAX 128
#define BOOKSHELF_PATH_MAX 256
#define BOOKSHELF_MAX_ITEMS 32
#define STORAGE_PATH_MAX 512
#define EPD_READER_LAYOUT_CACHE_VERSION 3u
#define LV_IMAGE_HEADER_MAGIC 0x19u
static char last_diagnostic[384];
static void rt_kprintf(const char *format, ...) {
    va_list args;va_start(args,format);
    vsnprintf(last_diagnostic,sizeof(last_diagnostic),format,args);va_end(args);
}
typedef int epd_service_t, rt_base_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { bool mounted; uint64_t total,used; } storage_info_t;
#define STORAGE_FLASH_ROOT "./flash"
#define STORAGE_SD_ROOT "./sd"
static unsigned clock_ms,card_session=1;
static uint32_t lv_tick_get(void) { return clock_ms; }
typedef uint32_t rt_tick_t;
typedef int32_t rt_int32_t;
static rt_tick_t rt_tick_get(void) { return clock_ms; }
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
static bool sd_available,service_cancelled;
static unsigned io_opens,io_reads,io_writes,io_closes,io_commits,open_fds,close_failures,unlink_failures,write_failures;
static size_t io_written,max_io_write,max_io_read,io_read_bytes;
static unsigned read_cost_ms;
static void (*read_hook)(void), (*allocation_hook)(void), (*interrupt_hook)(void);
static unsigned allocation_calls,allocation_failures;
static unsigned page_allocations;
static uint64_t disk_free=64u*1024u*1024u;
static bool epd_service_cancelled(epd_service_t *service) { (void)service;return service_cancelled; }
static void rt_thread_mdelay(unsigned ms) { clock_ms+=ms; }
static bool storage_available(storage_volume_t v) { return v==STORAGE_FLASH || sd_available; }
static bool storage_session_valid(storage_volume_t v,unsigned session) { return v==STORAGE_FLASH || (sd_available&&session==card_session); }
static bool storage_info(storage_volume_t v,storage_info_t *info) { *info=(storage_info_t){true,64u*1024u*1024u,64u*1024u*1024u-disk_free};return storage_available(v); }
static int cache_open(const char *path,int flags,...) {
    CHECK(strstr(path,"/resume/")); ++io_opens;
    int fd=_open(path,flags|_O_BINARY,_S_IREAD|_S_IWRITE);if(fd>=0)++open_fds;return fd;
}
static int cache_read(int fd,void *data,unsigned bytes) {
    ++io_reads;if(bytes>max_io_read)max_io_read=bytes;int n=_read(fd,data,bytes);
    clock_ms+=read_cost_ms;if(n>0)io_read_bytes+=(unsigned)n;
    if(read_hook){void (*hook)(void)=read_hook;read_hook=NULL;hook();}return n;
}
static int cache_write(int fd,const void *data,unsigned bytes) {
    ++io_writes;if(bytes>max_io_write)max_io_write=bytes;
    if(write_failures){--write_failures;return -1;}
    int n=_write(fd,data,bytes);if(n>0)io_written+=(unsigned)n;return n;
}
static int cache_close(int fd) { ++io_closes;if(close_failures){--close_failures;return -1;}int rc=_close(fd);if(!rc){CHECK(open_fds);--open_fds;}return rc; }
static int cache_unlink(const char *path) { if(unlink_failures){--unlink_failures;errno=EACCES;return -1;}return _unlink(path); }
#define open cache_open
#define read cache_read
#define write cache_write
#define close cache_close
#define unlink cache_unlink
#define fsync _commit
#define lseek _lseek
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) {
    (void)level;
    if(interrupt_hook){void (*hook)(void)=interrupt_hook;interrupt_hook=NULL;hook();}
}
static unsigned service_wakes,ui_requests,power_notifies;
static bool power_event;
static void epd_power_notify(void) { ++power_notifies;power_event=true; }
static void epd_service_wake(epd_service_t *service) { (void)service;++service_wakes;epd_power_notify(); }
static void epd_service_ui_request(epd_service_t *service, unsigned delay) {
    (void)delay;++ui_requests;if(service)epd_power_notify();
}
static void storage_lock(void) {}
static void storage_unlock(void) {}
static const char *storage_path_root(const char *path) {
    return !strncmp(path, STORAGE_SD_ROOT, strlen(STORAGE_SD_ROOT)) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT;
}
static bool storage_path_available(const char *path) {
    return strcmp(storage_path_root(path), STORAGE_SD_ROOT) || sd_available;
}
static unsigned storage_card_session(void) { return card_session; }
/* OWNERSHIP */
static bool storage_mkdirs(const char *path) {
    char temp[512];snprintf(temp,sizeof(temp),"%s",path);
    for(char *p=temp+2;*p;++p)if(*p=='/'){*p=0;_mkdir(temp);*p='/';}
    return _mkdir(temp)==0 || errno==EEXIST;
}
static bool storage_file_recover(const char *path) { (void)path; return true; }
static bool storage_file_commit(const char *temporary,const char *path) { ++io_commits;_unlink(path);return rename(temporary,path)==0; }
/* COLD BUDGET */
static bool bookshelf_file_signature(const char *path, unsigned size, uint32_t *signature) { (void)path;(void)size;(void)signature;CHECK(false);return false; }
static bool document_split_identity(const char *path, uint32_t *size, uint32_t *modified) { (void)path;(void)size;(void)modified;CHECK(false);return false; }

#define DOCUMENT_CONVERSION_VERSION 10u
#define DOCUMENT_SPLIT_EPUB 3u
#define EPD_APP_PSRAM 1
#define EPD_FONT_BODY 1
#define LV_COLOR_FORMAT_L8 6
#define LV_EVENT_DELETE 10
#define UI_SETTING_FONT_SIZE 0u
#define UI_SETTING_ENCODING 5u
typedef int lv_font_t, reader_error_t;
/* TYPES */
typedef reader_view_t ui_reader_view_t;
typedef reader_work_t ui_reader_work_t;
#define UI_READER_WORK_IDLE READER_WORK_IDLE
#define UI_READER_WORK_TARGET READER_WORK_TARGET
typedef struct { unsigned unit, chapter, offset, skip_utf8; } document_anchor_t;
typedef struct { char name[128], path[256]; uint32_t size, modified; unsigned format; } bookshelf_item_t;
typedef struct { uint32_t path, session, storage; } book_file_version_t;
typedef struct { unsigned magic, cf, w, h, stride; } image_header_t;
typedef struct { image_header_t header; uint32_t data_size; const uint8_t *data; } lv_image_dsc_t;
typedef struct { lv_image_dsc_t image; void *memory; } document_bitmap_t;
typedef struct {
    bookshelf_item_t file;
    struct { unsigned document_version, document_kind; document_anchor_t source; } position;
} ui_bookshelf_book_t;
typedef struct { int width, top, bottom; } reader_geometry_t;
typedef struct lv_obj lv_obj_t;
typedef struct { lv_obj_t *target; } lv_event_t;
struct lv_obj {
    lv_obj_t *parent, *children[8]; unsigned count;
    const void *src; bool image;
    void (*deleted)(lv_event_t *);
};
static const int lv_image_class = 1;
static lv_font_t font;
static lv_obj_t host_body, *core_root;
static ui_bookshelf_book_t books[24];
static ui_reader_view_t state;
static unsigned settings[6], font_id=2, font_revision=3, file_revision=4, volume_revision, source_session, font_signature;
static unsigned calls, closes, turns, binds, selected, live_allocations;
static size_t available = 8u * 1024u * 1024u;
static bool memory_pressure;
typedef struct { bool pending; } app_memory_pressure_status_t;
static app_memory_pressure_status_t app_memory_pressure_query(void) { return (app_memory_pressure_status_t){memory_pressure}; }
static bool fail_open, waiting, settings_changed;
static reader_geometry_t host_area = {684, 54, 1168};
static uint8_t frame_pixels[16384];
static lv_image_dsc_t frame = {{LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_L8,128,128,128},16384,frame_pixels};
void ui_reader_close(void);
void ui_reader_cache_entry(lv_obj_t *body);
void ui_reader_cache_preload(unsigned first, unsigned count);
bool ui_reader_cache_process(void);
void ui_reader_cache_quiesce(void);
static bool document_cache_directory(const bookshelf_item_t *file, const char *app, char *out, size_t capacity) { (void)file;(void)app;snprintf(out,capacity,".");return true; }
static bool epd_reader_font_fingerprint(unsigned id, uint32_t *value) { *value=id*313u+77u+font_signature;return true; }
static void *epd_app_alloc(size_t bytes, int kind) {
    if(budget_control_entered) {
        CHECK(kind==EPD_APP_PSRAM && !budget_control_memory && bytes>7000 && bytes<8192);
        budget_control_memory=malloc(bytes);CHECK(budget_control_memory);return budget_control_memory;
    }
    (void)kind;++allocation_calls;void *p=NULL;
    if(bytes>=600u*1024u)++page_allocations;
    if(allocation_failures)--allocation_failures;
    else if(bytes<=available) {p=malloc(bytes);if(p)++live_allocations;}
    if(allocation_hook){void (*hook)(void)=allocation_hook;allocation_hook=NULL;hook();}return p;
}
static void epd_app_free(void *p) {
    if(p==budget_control_memory) {budget_control_memory=NULL;free(p);return;}
    if(p) { CHECK(live_allocations); --live_allocations; } free(p);
}
static size_t epd_app_psram_available(void) { return available; }
static void lv_image_cache_drop(const lv_image_dsc_t *image) { CHECK(image->data); }
static const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index) { return index < 24 ? &books[index] : NULL; }
static book_file_version_t book_file_version(const char *path) { (void)path; return (book_file_version_t){file_revision,source_session,volume_revision}; }
static bool book_file_version_valid(const char *path, const book_file_version_t *version) { (void)path; return version->path == file_revision&&version->session==source_session; }
static unsigned epd_app_reading_font_id(void) { return font_id; }
static unsigned epd_app_reading_font_revision(void) { return font_revision; }
static unsigned ui_settings_index(unsigned index) { return settings[index]; }
static reader_geometry_t reader_geometry(void) { return host_area; }
static const lv_font_t *epd_app_font_role(int role) { (void)role; return &font; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *obj, unsigned index) { return index < obj->count ? obj->children[index] : NULL; }
static bool lv_obj_check_type(lv_obj_t *obj, const int *type) { (void)type; return obj->image; }
static const void *lv_image_get_src(lv_obj_t *obj) { return obj->src; }
static lv_obj_t *lv_event_get_target_obj(lv_event_t *event) { return event->target; }
static void lv_obj_delete(lv_obj_t *obj) {
    while (obj->count) lv_obj_delete(obj->children[0]);
    if (obj->deleted) { lv_event_t event={obj}; obj->deleted(&event); }
    lv_obj_t *parent=obj->parent;
    for (unsigned i=0; i<parent->count; ++i) if (parent->children[i]==obj) {
        memmove(parent->children+i,parent->children+i+1,(parent->count-i-1)*sizeof(obj)); --parent->count; break;
    }
    free(obj);
}
static lv_obj_t *lv_image_create(lv_obj_t *parent) {
    lv_obj_t *obj=calloc(1,sizeof(*obj)); CHECK(obj && parent->count<8);
    obj->parent=parent; obj->image=true; parent->children[parent->count++]=obj; return obj;
}
static void lv_image_set_src(lv_obj_t *obj,const void *src) { obj->src=src; }
static void lv_obj_add_event_cb(lv_obj_t *obj,void (*cb)(lv_event_t *),int event,void *data) { (void)event;(void)data;obj->deleted=cb; }
static void lv_obj_set_pos(lv_obj_t *obj,int x,int y) { (void)obj;(void)x;(void)y; }
static void lv_obj_set_size(lv_obj_t *obj,unsigned w,unsigned h) { (void)obj; CHECK(w&&h); }
static const ui_reader_view_t *document_view_get(void) { return &state; }
static bool document_view_waiting(void) { return waiting; }
static bool document_view_settings_changed(void) { return settings_changed; }
static void document_view_close(void) { ++closes; if(core_root) lv_obj_delete(core_root);core_root=NULL;memset(&state,0,sizeof(state)); }
static bool document_view_open(unsigned index) {
    ++calls; selected=index;
    if(fail_open) { state.error_code=1;strcpy(state.error,"error");return false; }
    state.title=books[index].file.name;state.text=""; state.font=&font;return true;
}
static bool document_view_bind(lv_obj_t *target) {
    if(!state.ready) return false;
    if(!core_root) {
        core_root=lv_image_create(target);core_root->image=false;
        lv_image_set_src(lv_image_create(core_root),&frame); ++binds;
        books[selected].position.document_version=DOCUMENT_CONVERSION_VERSION;
        books[selected].position.document_kind=books[selected].file.format;
        books[selected].position.source=(document_anchor_t){2,selected+1,state.page*100,0};
    }
    return true;
}
static const lv_image_dsc_t *epd_reader_bound_image(lv_obj_t *body) { return core_root&&core_root->parent==body?&frame:NULL; }
static bool document_view_save(void) { return true; }
static bool document_view_reflow(void) { return true; }
static ui_reader_work_t document_view_step(uint32_t budget,bool foreground,bool commit) { (void)budget;(void)foreground;(void)commit;return UI_READER_WORK_IDLE; }
static void epd_reader_background_pause(bool paused) { (void)paused; }
static void document_view_cancel_pending(void) {}
static void document_view_seek(unsigned page) { (void)page; ++turns; }
static void epd_reader_turn(int direction) { (void)direction; ++turns; }
static bool epd_reader_render_image_init(void) { return true; }
static void lv_draw_wait_for_finish(void) {}
#include "cache_bridge.h"
/* PRODUCTION */
static bool reader_session=true;
static unsigned priority_reader_rounds,priority_delays;
static size_t priority_read_mark;
static bool ui_bookshelf_worker_step(epd_service_t *service) { (void)service;return false; }
static bool books_cover_cache_worker_step(epd_service_t *service,bool reading) { (void)service;(void)reading;return false; }
static uint32_t open_trace_worker_begin(void) { return 0; }
static void open_trace_worker_mark(uint32_t trace,unsigned stage) { (void)trace;(void)stage; }
#define OPEN_SHELF_DONE 0
#define OPEN_READER_DONE 1
static bool epd_reader_worker_step(epd_service_t *service) {
    (void)service;CHECK(++priority_reader_rounds<200);
    CHECK(io_read_bytes>priority_read_mark&&io_read_bytes-priority_read_mark<=64u*1024u);
    priority_read_mark=io_read_bytes;
    clock_ms+=16;
    if(entry_store.ready) {
        CHECK(ui_reader_cache_process()&&ui_reader_bind(&host_body));
        CHECK(preview_object&&!state.ready);service_cancelled=true;
    }
    return true;
}
static void rt_thread_delay(rt_tick_t ticks) { CHECK(ticks==1);++priority_delays;clock_ms+=ticks; }
static rt_int32_t ui_bookshelf_worker_delay(void) { return 1; }
static uint32_t epd_service_wait(epd_service_t *service,rt_int32_t delay) { (void)service;(void)delay;CHECK(false);return 0; }
static void ui_bookshelf_worker_shutdown(void) {}
static void epd_reader_worker_shutdown(void) {}
/* WORKER */
static void drain(void) {
    unsigned iterations = 0;
    while (ui_reader_cache_worker_step(NULL,false)) { ui_reader_cache_process(); CHECK(++iterations < 2000); }
}
static void shutdown_cache(void) { ui_reader_cache_worker_shutdown();ui_reader_shutdown();CHECK(!open_fds&&!ui_reader_cache_busy()); }
static void check_diagnostic(const char *expected) {
    unsigned reads=io_reads,writes=io_writes,opens=io_opens,wakes=service_wakes,allocs=allocation_calls;
    unsigned serial=entry_store.serial,epoch=entry_store.epoch;
    ui_reader_cache_diagnostic();
    if(!strstr(last_diagnostic,expected))fprintf(stderr,"expected %s; got %s",expected,last_diagnostic);
    CHECK(strstr(last_diagnostic,expected));
    CHECK(io_reads==reads&&io_writes==writes&&io_opens==opens&&service_wakes==wakes&&allocation_calls==allocs);
    CHECK(entry_store.serial==serial&&entry_store.epoch==epoch);
}
static void publish(unsigned index) {
    CHECK(ui_reader_open(index));ui_reader_first_frame();ui_reader_step(8,true,true);
    state.ready=true;state.margin=24;state.top=54;state.revision=9;state.page=4;
    CHECK(ui_reader_bind(&host_body));clock_ms+=ENTRY_IDLE_MS;ui_reader_cache_idle(&host_body,true);drain();
}
static void show_page(unsigned page) {
    if(core_root)lv_obj_delete(core_root);core_root=NULL;
    state.ready=true;state.page=page;state.margin=24;state.top=54;
    CHECK(ui_reader_bind(&host_body));
}
static void settle(void) { clock_ms+=ENTRY_IDLE_MS;ui_reader_cache_idle(&host_body,true); }
static void constrained_cache_tests(void) {
    shutdown_cache();CHECK(!live_allocations);
    unsigned writes=io_writes,opens=io_opens,commits=io_commits;
    CHECK(ui_reader_open(20));ui_reader_first_frame();ui_reader_step(8,true,true);
    for(unsigned i=1;i<=80;++i){
        clock_ms+=100;ui_reader_turn(1);show_page(i);ui_reader_cache_idle(&host_body,true);
        CHECK(!ui_reader_cache_worker_step(NULL,true));
        CHECK(entry_dirty&&!entry_store.pending&&!ui_reader_cache_busy());
        CHECK(!live_allocations&&io_writes==writes&&io_opens==opens);
    }
    clock_ms+=ENTRY_IDLE_MS-1;ui_reader_cache_idle(&host_body,true);CHECK(!live_allocations);
    ++clock_ms;ui_reader_cache_idle(&host_body,true);
    CHECK(!entry_dirty&&entry_store.pending&&live_allocations==2&&!ui_reader_cache_busy());
    CHECK(entry_store.pending->pixels->references==2&&entry_store.pending->header.page==80);
    CHECK(io_writes==writes&&!ui_reader_cache_worker_step(NULL,true));
    CHECK(ui_reader_cache_worker_step(NULL,false)&&ui_reader_cache_busy()&&open_fds==1);
    CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(entry_store.job->offset==ENTRY_IO_BYTES&&max_io_write<=ENTRY_IO_BYTES);
    unsigned before_cancel=io_writes;
    ui_reader_turn(1);close_failures=2;unlink_failures=1;
    CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy()&&open_fds==1);
    CHECK(storage_file_release(STORAGE_FLASH,10)&&!open_fds&&!unlink_failures);
    CHECK(io_writes==before_cancel&&io_commits==commits&&live_allocations==1);
    puts("PASS 80 continuous turns: no capture/allocation/file I/O; one shared final-page capture after idle; reader priority and real abort retain active through failed close/unlink");

    show_page(81);settle();CHECK(entry_store.pending&&!ui_reader_cache_busy());
    ui_reader_cache_quiesce();CHECK(!entry_store.pending&&!ui_reader_cache_busy()&&live_allocations==1);
    CHECK(!ui_reader_cache_worker_step(NULL,false)&&io_writes==before_cancel);
    show_page(82);ui_reader_cache_entry(&host_body);CHECK(entry_store.pending);
    ui_reader_cache_entry(&host_body);CHECK(live_allocations==2);
    ui_reader_cache_idle(&host_body,true);drain();CHECK(io_commits==commits+1);
    puts("PASS queued optimization cancels without I/O; exit capture is at most once for the last bound page");

    shutdown_cache();opens=io_opens;ui_reader_cache_preload(20,1);
    unsigned serial=entry_store.serial;ui_reader_cache_preload(20,1);CHECK(serial==entry_store.serial);
    ++volume_revision;ui_reader_cache_preload(20,1);CHECK(serial==entry_store.serial);
    drain();CHECK(live_allocations==1&&io_opens==opens+1);
    opens=io_opens;CHECK(ui_reader_open(20)&&ui_reader_preview_active());
    ui_reader_cache_preload(20,1);drain();CHECK(io_opens==opens);
    shutdown_cache();ui_reader_cache_preload(20,1);
    CHECK(ui_reader_cache_worker_step(NULL,false));CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(ui_reader_cache_worker_step(NULL,false)&&entry_store.job->offset==ENTRY_IO_BYTES);
    close_failures=1;ui_reader_cache_quiesce();CHECK(ui_reader_cache_busy());
    CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy()&&!live_allocations);
    ui_reader_cache_preload(20,1);CHECK(ui_reader_cache_worker_step(NULL,false));
    ++file_revision;drain();CHECK(!live_allocations&&!open_fds);--file_revision;
    puts("PASS resident preload emits no file I/O; unchanged path survives unrelated volume changes; active read cancellation and source mutation reject partial results");

    shutdown_cache();sd_available=true;
    publish(21);CHECK(io_commits==commits+2);
    entry_store_key_t key;reader_entry_t saved={0};saved.file=books[21].file;entry_store_key(&key,&saved);
    char disk_path[STORAGE_PATH_MAX];CHECK(entry_store_path(&key,STORAGE_SD,disk_path,sizeof(disk_path)));
    CHECK(_access(disk_path,0)==0);
    shutdown_cache();ui_reader_cache_preload(21,1);CHECK(ui_reader_cache_worker_step(NULL,false));
    ++card_session;drain();CHECK(!live_allocations&&!open_fds);
    sd_available=false;disk_free=ENTRY_DISK_RESERVE;opens=io_opens;
    publish(22);CHECK(io_opens==opens&&!ui_reader_cache_busy());
    disk_free=64u*1024u*1024u;shutdown_cache();
    puts("PASS TF destination preferred; media session change cancels active read; insufficient internal space skips disk optimization");

    for(unsigned i=0;i<24;++i)publish(i);
    char paths[24][STORAGE_PATH_MAX];
    unsigned distinct=0,first=UINT_MAX,last=0;
    for(unsigned i=0;i<24;++i){
        reader_entry_t e={0};e.file=books[i].file;entry_store_key(&key,&e);
        CHECK(entry_store_path(&key,STORAGE_FLASH,paths[i],sizeof(paths[i])));
        bool unique=true;
        for(unsigned j=0;j<i;++j)if(!strcmp(paths[i],paths[j])){unique=false;first=j;last=i;}
        if(unique)++distinct;
    }
    CHECK(distinct==ENTRY_DISK_SLOTS&&first!=UINT_MAX);
    /* Find the newest writer in the chosen slot, then probe its older occupant. */
    for(unsigned i=last+1;i<24;++i)if(!strcmp(paths[i],paths[first]))last=i;
    shutdown_cache();ui_reader_cache_preload(first,1);drain();CHECK(!live_allocations);
    ui_reader_cache_preload(last,1);drain();CHECK(live_allocations==1);
    struct _finddata_t found;intptr_t handle=_findfirst("./flash/cache/books/resume/entry-*.frame",&found);
    unsigned files=0;CHECK(handle!=-1);do{++files;}while(_findnext(handle,&found)==0);_findclose(handle);
    CHECK(files==ENTRY_DISK_SLOTS);shutdown_cache();CHECK(!live_allocations);
    CHECK(max_io_write<=ENTRY_IO_BYTES&&max_io_read<=ENTRY_IO_BYTES);
    puts("PASS 24 books use exactly eight fixed files; full-path collision rejects old occupant; every data read/write is at most 4 KiB");
}
static entry_store_job_t *start_preload(unsigned target) {
    shutdown_cache();ui_reader_cache_preload(0,4);
    unsigned steps=0;
    while(!entry_store.job || strcmp(entry_store.job->key.path,books[target].file.path) || entry_store.job->offset<ENTRY_IO_BYTES) {
        CHECK(ui_reader_cache_worker_step(NULL,false));ui_reader_cache_process();CHECK(++steps<100);
    }
    return entry_store.job;
}
static void select_first(void) { CHECK(ui_reader_open(0)); }
static void preload_promotion_tests(void) {
    shutdown_cache();for(unsigned i=0;i<4;++i)publish(i);
    for(unsigned target=0;target<3;target+=2) {
        entry_store_job_t *job=start_preload(target);
        int fd=job->fd;uint32_t checksum=job->checksum;
        unsigned reads=io_reads,opens=io_opens,closed=io_closes,prepared=calls;
        CHECK(ui_reader_open(target)&&entry_store.job==job&&!ui_reader_preview_active());
        CHECK(job->offset==ENTRY_IO_BYTES&&job->fd==fd&&job->checksum==checksum);
        CHECK(io_reads==reads&&io_opens==opens&&io_closes==closed&&calls==prepared);
        CHECK(!ui_reader_cache_worker_step(NULL,true));
        CHECK(job->serial==entry_store.serial&&entry_store.count==1&&entry_store.next==1);
        CHECK(job->offset==ENTRY_IO_BYTES&&io_reads==reads);
        CHECK(ui_reader_cache_worker_step(NULL,false)&&job->offset==2*ENTRY_IO_BYTES);
        drain();CHECK(ui_reader_preview_active()&&ui_reader_bind(&host_body));
        CHECK(io_opens==opens&&io_closes==closed+1&&io_reads==reads+4&&calls==prepared);
        CHECK(entry_store.next==entry_store.count&&!open_fds&&!ui_reader_cache_busy());
    }
    puts("PASS 4-to-1 selection preserves the active descriptor, checksum and pixel offset, skips other queued books and binds without document preparation");

    for(unsigned late=0;late<2;++late) {
        entry_store_job_t *job=start_preload(0);unsigned opens=io_opens,closed=io_closes;
        if(!late) { read_hook=select_first;CHECK(ui_reader_cache_worker_step(NULL,false)); }
        else {
            while(job->phase!=ENTRY_CLOSE)CHECK(ui_reader_cache_worker_step(NULL,false));
            allocation_hook=select_first;CHECK(ui_reader_cache_worker_step(NULL,false));
        }
        ui_reader_cache_process();drain();CHECK(!read_hook&&!allocation_hook&&ui_reader_preview_active());
        CHECK(io_opens==opens&&io_closes==closed+1&&entry_store.next==1);
    }
    puts("PASS selecting during a pixel read or final result allocation preserves completed work through publication");

    for(unsigned change=0;change<9;++change) {
        entry_store_job_t *job=start_preload(0);
        unsigned old_serial=job->serial,reads=io_reads,opens=io_opens;
        switch(change) {
        case 1: ++file_revision;break;
        case 2: ++font_id;break;
        case 3: ++font_signature;break;
        case 4: ++settings[0];break;
        case 5: ++books[0].position.source.offset;break;
        case 6: ++host_area.width;break;
        case 7: ++books[0].file.modified;break;
        case 8: ++source_session;break;
        }
        CHECK(ui_reader_open(change==0?1:0));
        CHECK(entry_store.serial!=old_serial);
        close_failures=1;
        CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy()&&!entry_store.job);
        CHECK(entry_store.next==0&&io_reads==reads&&io_opens==opens);
        CHECK(!open_fds);
        if(change==0) { drain();CHECK(ui_reader_preview_active()&&!strcmp(preview->file.path,books[1].file.path)); }
        shutdown_cache();
        switch(change) {
        case 1: --file_revision;break;
        case 2: --font_id;break;
        case 3: --font_signature;break;
        case 4: --settings[0];break;
        case 5: --books[0].position.source.offset;break;
        case 6: --host_area.width;break;
        case 7: --books[0].file.modified;break;
        case 8: --source_session;break;
        }
        CHECK(!live_allocations);
    }
    puts("PASS different book, source revision/session, font identity/signature, layout, anchor and metadata cancel instead of promoting stale work");

    for(unsigned cancel=0;cancel<3;++cancel) {
        start_preload(0);unsigned reads=io_reads,opens=io_opens;
        if(cancel==0)ui_reader_cache_quiesce();
        CHECK(ui_reader_open(0));
        if(cancel==1)ui_reader_cache_quiesce();
        if(cancel==2)service_cancelled=true;
        epd_service_t service=0;close_failures=1;
        CHECK(ui_reader_cache_worker_step(&service,true)&&!ui_reader_cache_busy());
        CHECK(!entry_store.job&&io_reads==reads&&io_opens==opens&&!open_fds);
        if(cancel==0)CHECK(entry_store.next==0);
        memory_pressure=service_cancelled=false;
        if(cancel==0){drain();CHECK(ui_reader_preview_active()&&io_opens==opens+1);}
        shutdown_cache();CHECK(!live_allocations);
    }
    books[0].file.format=DOCUMENT_SPLIT_EPUB;publish(0);
    entry_store_job_t *job=start_preload(0);unsigned serial=job->serial;
    ++volume_revision;CHECK(ui_reader_open(0));
    CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy());
    CHECK(entry_store.serial!=serial&&entry_store.next==0&&!open_fds);
    shutdown_cache();books[0].file.format=2;publish(0);
    job=start_preload(0);++volume_revision;CHECK(ui_reader_open(0));
    CHECK(!ui_reader_cache_worker_step(NULL,true)&&job->serial==entry_store.serial);
    drain();CHECK(ui_reader_preview_active());shutdown_cache();
    puts("PASS explicit cancellation cannot resurrect an old read; pressure/service cancellation and split-source volume changes still abort, unrelated normal-book volume changes preserve progress");
}
static void pixel_budget_tests(void) {
    shutdown_cache();CHECK(!entry_pixel_bytes&&!live_allocations);
    static uint8_t large_pixels[628*1114];
    lv_image_dsc_t small=frame;
    frame=(lv_image_dsc_t){{LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_L8,628,1114,628},sizeof(large_pixels),large_pixels};
    for(unsigned i=0;i<4;++i){
        publish(i);
        unsigned resident=0;for(unsigned j=0;j<ENTRY_SLOTS;++j)if(entries[j].pixels)++resident;
        CHECK(resident==1&&entry_pixel_bytes==sizeof(entry_pixels_t)+sizeof(large_pixels));
        CHECK(entry_pixel_bytes<=ENTRY_PIXEL_BUDGET);
    }
    show_page(90);settle();CHECK(entry_store.pending);
    CHECK(entry_store.pending->pixels->references==2);
    CHECK(entry_pixel_bytes==sizeof(entry_pixels_t)+sizeof(large_pixels));
    CHECK(ui_reader_cache_worker_step(NULL,false)&&open_fds==1);
    size_t released=0;
    entry_resource_shrink(NULL,1,SIZE_MAX,&released);
    CHECK(!released&&entry_pixel_bytes&&entry_store.job->pixels->references==2);
    ui_reader_cache_quiesce();close_failures=1;
    CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy());
    entry_resource_publish();CHECK(epd_resource_reclaim(1,SIZE_MAX,32)>0);
    epd_resource_process(EPD_RESOURCE_UI,32);
    CHECK(!entry_pixel_bytes&&!live_allocations);
    allocation_failures=1;CHECK(!entry_pixels_new(16)&&!allocation_failures);
    shutdown_cache();publish(4);CHECK(entry_pixel_bytes);
    ui_reader_close();CHECK(ui_reader_open(4)&&ui_reader_preview_active());ui_reader_bind(&host_body);
    available=128u*1024u;memory_pressure=true;CHECK(!ui_reader_cache_process());
    released=0;entry_resource_shrink(NULL,1,SIZE_MAX,&released);
    CHECK(!released&&ui_reader_preview_active()&&entry_pixel_bytes&&live_allocations);
    ui_reader_close();entry_resource_shrink(NULL,1,SIZE_MAX,&released);
    CHECK(released&&!entry_pixel_bytes&&!live_allocations);
    available=sizeof(large_pixels)+sizeof(entry_pixels_t)-1;
    CHECK(!entry_pixels_new(sizeof(large_pixels))&&!entry_pixel_bytes);
    available=8u*1024u*1024u;memory_pressure=false;frame=small;shutdown_cache();
    puts("PASS 699592-byte pages retain the local 1 MiB ceiling; provider protects active writer/preview, releases idle pixels, and allocation failure preserves accounting");
}
static void pixel_reuse_tests(void) {
    shutdown_cache();CHECK(!entry_pixel_bytes&&!live_allocations);
    static uint8_t pixels[628*1114];
    lv_image_dsc_t saved=frame;
    frame=(lv_image_dsc_t){{LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_L8,628,1114,628},sizeof(pixels),pixels};
    publish(3);
    reader_entry_t *entry=NULL;
    for(unsigned i=0;i<ENTRY_SLOTS;++i)if(entries[i].pixels)entry=&entries[i];
    CHECK(entry&&entry->pixels->references==1);
    entry_pixels_t *original=entry->pixels;
    size_t charge=entry_pixel_bytes;
    unsigned allocations_before=page_allocations;
    for(unsigned i=0;i<100;++i) {
        memset(pixels,(int)(i+1),sizeof(pixels));
        show_page(100+i);settle();drain();
        CHECK(entry->pixels==original&&entry->pixels->references==1);
        CHECK(!memcmp(entry->pixels->data,pixels,sizeof(pixels)));
        CHECK(entry_pixel_bytes==charge&&page_allocations==allocations_before);
    }
    entry_pixels_ref(original);
    memset(pixels,0xee,sizeof(pixels));
    show_page(201);settle();
    CHECK(original->references==1&&original->data[0]==100);
    CHECK(!entry->pixels&&entry_pixel_bytes==charge);
    entry_pixels_release(original);
    CHECK(!entry_pixel_bytes);
    show_page(202);settle();drain();CHECK(entry->pixels);
    frame.data_size=128*128;frame.header.w=frame.header.h=frame.header.stride=128;
    show_page(203);settle();drain();
    CHECK(entry->pixels->bytes==sizeof(entry_pixels_t)+frame.data_size);
    CHECK(entry_pixel_bytes==entry->pixels->bytes);
    frame=saved;shutdown_cache();CHECK(!entry_pixel_bytes&&!live_allocations);
    puts("PASS 100 full-page snapshots reuse one pixel allocation; in-use pixels remain immutable; resized snapshots preserve capacity accounting");
}
/* Model a high-priority main loop consuming its power event before the lower
 * priority tasks can run. Execute the actual cache worker in those wait slots. */
static void queue_first(void) { ui_reader_cache_preload(0,1); }
static void queue_second(void) { ui_reader_cache_preload(1,1); }
static void consume_ready(void) { ui_reader_cache_process(); }
static void preload_snapshot_race_tests(void) {
    shutdown_cache();publish(0);publish(1);shutdown_cache();
    unsigned allocated=allocation_calls;
    interrupt_hook=queue_first;
    CHECK(!ui_reader_cache_worker_step(NULL,false)&&!interrupt_hook);
    CHECK(entry_store.count==1&&!entry_store.next&&allocation_calls==allocated);
    drain();CHECK(ui_reader_open(0)&&ui_reader_preview_active());
    CHECK(ui_reader_bind(&host_body)&&!ui_reader_view()->ready);
    check_diagnostic("preview=1 preview_bound=1 entrymatch=1");shutdown_cache();

    ui_reader_cache_preload(0,2);
    while(!entry_store.ready)CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(entry_store.next==1&&entry_store.count==2);
    allocated=allocation_calls;interrupt_hook=consume_ready;
    CHECK(!ui_reader_cache_worker_step(NULL,false)&&!interrupt_hook);
    CHECK(!entry_store.ready&&entry_store.next==1&&allocation_calls==allocated);
    drain();CHECK(ui_reader_open(1)&&ui_reader_preview_active());shutdown_cache();

    for(unsigned fail=0;fail<2;++fail) {
        ui_reader_cache_preload(0,1);unsigned serial=entry_store.serial;
        allocation_hook=queue_second;allocation_failures=fail;
        bool worked=ui_reader_cache_worker_step(NULL,false);
        CHECK(!allocation_hook&&!allocation_failures&&entry_store.serial!=serial);
        CHECK(entry_store.count==1);
        if(fail)CHECK(!worked&&!entry_store.next&&!ui_reader_cache_busy());
        else CHECK(worked&&entry_store.next==1&&entry_store.job&&
                   !strcmp(entry_store.job->key.path,books[1].file.path));
        drain();CHECK(ui_reader_open(1)&&ui_reader_preview_active());shutdown_cache();
    }
    ui_reader_cache_preload(0,1);allocation_hook=ui_reader_cache_quiesce;
    CHECK(!ui_reader_cache_worker_step(NULL,false));
    CHECK(!allocation_hook&&!entry_store.count&&!live_allocations&&!ui_reader_cache_busy());

    for(unsigned reserve=0;reserve<2;++reserve) {
        ui_reader_cache_preload(0,1);
        if(reserve)available=sizeof(entry_store_job_t)-1;
        else allocation_failures=1;
        CHECK(!ui_reader_cache_worker_step(NULL,false)&&!allocation_failures);
        CHECK(entry_store.next==entry_store.count&&!live_allocations);
        allocated=allocation_calls;unsigned wakes=service_wakes,serial=entry_store.serial;
        available=8u*1024u*1024u;
        for(unsigned i=0;i<1000;++i) {
            ui_reader_cache_preload(0,1);
            CHECK(!ui_reader_cache_worker_step(NULL,false));
        }
        CHECK(allocation_calls==allocated&&service_wakes==wakes&&entry_store.serial==serial);
        shutdown_cache();
    }
    puts("PASS unlocked preload windows: new queue and consumed ready result survive; allocation failure cannot consume a newer generation; successful allocation follows current target; cancellation frees unused allocation; genuine OOM/reserve failure causes no allocation or wake polling in 2000 rounds");
}
static unsigned pressure_rounds(unsigned rounds) {
    static epd_service_t service;
    unsigned wakes=service_wakes,serial=entry_store.serial,epoch=entry_store.epoch;
    unsigned cancel_epoch=entry_store.cancel_epoch,worker_slots=0,shell_slots=0,portal_slots=0,cleanup=0;
    unsigned reads=io_reads,writes=io_writes,commits=io_commits;
    for(unsigned i=0;i<rounds;++i) {
        unsigned notifications=power_notifies,requests=ui_requests;
        CHECK(!ui_reader_cache_process());
        ui_reader_cache_idle(&host_body,true);
        ui_reader_cache_preload(0,4);
        ui_reader_cache_entry(&host_body);
        CHECK(!entry_store.write_allowed&&!entry_store.pending&&!entry_store.ready);
        CHECK(service_wakes==wakes&&power_notifies==notifications&&ui_requests==requests);
        CHECK(entry_store.serial==serial&&entry_store.epoch==epoch&&entry_store.cancel_epoch==cancel_epoch);
        if(power_event) { power_event=false;continue; }
        ++clock_ms;++worker_slots;++shell_slots;++portal_slots;
        if(ui_reader_cache_worker_step(&service,true))++cleanup;
    }
    CHECK(worker_slots>=rounds-2&&shell_slots==worker_slots&&portal_slots==worker_slots);
    CHECK(service_wakes==wakes&&io_reads==reads&&io_writes==writes&&io_commits==commits);
    unsigned held=0;for(unsigned i=0;i<STORAGE_CLOSE_SLOTS;++i)if(owned_files[i].used){CHECK(owned_files[i].pending);++held;}
    CHECK(!ui_reader_cache_busy()&&open_fds==held&&!live_allocations&&!entry_pixel_bytes);
    return cleanup;
}
static void sustained_pressure_tests(void) {
    for(unsigned pressure=0;pressure<2;++pressure) {
        shutdown_cache();available=128u*1024u;memory_pressure=pressure!=0;
        publish(6);CHECK(entry_pixel_bytes&&!entry_store.job&&!entry_store.pending);
        ui_reader_close();
        unsigned wakes=service_wakes,serial=entry_store.serial,epoch=entry_store.epoch;
        unsigned reads=io_reads,writes=io_writes;
        for(unsigned i=0;i<1000;++i)CHECK(!ui_reader_cache_process());
        CHECK(service_wakes==wakes&&entry_store.serial==serial&&entry_store.epoch==epoch);
        CHECK(io_reads==reads&&io_writes==writes&&entry_pixel_bytes);
        CHECK(epd_resource_reclaim(1,SIZE_MAX,32)>0);epd_resource_process(EPD_RESOURCE_UI,32);
        CHECK(!entry_pixel_bytes&&!live_allocations);
        shutdown_cache();
        ui_reader_cache_preload(6,1);drain();CHECK(ui_reader_open(6)&&ui_reader_preview_active());
        shutdown_cache();
        for(unsigned writing=0;writing<2;++writing) {
            publish(6);
            if(writing){show_page(92);settle();CHECK(entry_store.pending);}
            else {shutdown_cache();ui_reader_cache_preload(6,1);}
            CHECK(ui_reader_cache_worker_step(NULL,false)&&ui_reader_cache_busy());
            CHECK(ui_reader_cache_worker_step(NULL,false));
            CHECK(ui_reader_cache_worker_step(NULL,false)&&entry_store.job->offset==ENTRY_IO_BYTES);
            close_failures=2;unlink_failures=writing?1:0;ui_reader_cache_quiesce();
            CHECK(ui_reader_cache_worker_step(NULL,true)&&!ui_reader_cache_busy());
            CHECK(storage_file_release(STORAGE_SD,100+pressure*2+writing));
            CHECK(storage_file_release(STORAGE_FLASH,100+pressure*2+writing));
            CHECK(!close_failures&&!unlink_failures&&!open_fds);
            reads=io_reads;writes=io_writes;wakes=service_wakes;
            for(unsigned i=0;i<1000;++i)CHECK(!ui_reader_cache_worker_step(NULL,true));
            CHECK(io_reads==reads&&io_writes==writes&&service_wakes==wakes);
            shutdown_cache();
        }
    }
    memory_pressure=false;available=8u*1024u*1024u;
    puts("PASS admitted entry read/write below former 1 MiB floor during global pressure; 6000 idle/cleanup rounds produce no repeated I/O or wakes; provider reclaim and failed descriptor cleanup remain bounded");
}

static void resident_cleanup_tests(void) {
    shutdown_cache();sd_available=true;publish(0);
    entry_store_job_t *job=start_preload(0);int fd=job->fd;storage_volume_t volume=job->volume;
    char path[STORAGE_PATH_MAX];strcpy(path,job->path);
    close_failures=10000;ui_reader_cache_worker_shutdown();ui_reader_shutdown();
    CHECK(!entry_store.job&&!ui_reader_cache_busy()&&!live_allocations&&open_fds==1&&owned_find(fd));
    unsigned close_count=io_closes;
    for(unsigned i=0;i<1000;++i)CHECK(!ui_reader_cache_worker_step(NULL,true));
    CHECK(io_closes==close_count&&!strcmp(owned_find(fd)->path,path));
    CHECK(storage_file_owned_open(path,O_RDWR|O_TRUNC,false)<0&&io_closes==close_count+1);
    close_failures=0;++card_session;CHECK(storage_file_release(volume,500)&&!open_fds);
    ui_reader_cache_preload(0,1);drain();CHECK(ui_reader_open(0)&&ui_reader_preview_active());shutdown_cache();
    publish(0);show_page(99);settle();CHECK(entry_store.pending&&ui_reader_cache_worker_step(NULL,false));
    unlink_failures=10000;ui_reader_cache_worker_shutdown();ui_reader_shutdown();
    CHECK(!open_fds&&!ui_reader_cache_busy()&&!live_allocations&&unlink_failures==9999);
    unlink_failures=0;publish(0);shutdown_cache();
    puts("PASS persistent close is resident-owned after module teardown, no worker polling, held pathname rejects truncation, new session reclaims; persistent unlink leaves reusable temporary without blocking");
}
static void priority_other_queue(void) { ui_reader_cache_preload(1,1); }
static void priority_cancel(void) { ui_reader_cache_quiesce(); }
static void priority_target_tests(void) {
    shutdown_cache();sd_available=false;
    static uint8_t large_pixels[628*1114];
    lv_image_dsc_t small=frame;
    frame=(lv_image_dsc_t){{LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_L8,628,1114,628},sizeof(large_pixels),large_pixels};
    publish(0);shutdown_cache();
    unsigned writes=io_writes,prepared=calls;
    CHECK(ui_reader_open(0)&&!preview&&!entry_store.job&&!live_allocations);
    priority_reader_rounds=priority_delays=0;priority_read_mark=io_read_bytes;
    books_worker(NULL);service_cancelled=false;
    CHECK(priority_reader_rounds==11&&priority_delays==10);
    CHECK(preview_object&&!state.ready&&calls==prepared&&io_writes==writes);
    printf("PASS actual Books worker: %u-byte disk preview, 11 read batches before 11 busy 16ms document steps, 10 real one-tick sleeps, preview bound before document ready; <=64KiB per batch and <=4KiB per read\n",(unsigned)sizeof(large_pixels));
    shutdown_cache();CHECK(ui_reader_open(0));
    read_cost_ms=3;clock_ms=UINT32_MAX-4;
    unsigned started=clock_ms,reads=io_reads;
    CHECK(ui_reader_cache_worker_priority(NULL));
    CHECK(clock_ms-started==9&&io_reads-reads==3&&entry_store.job->offset==2*ENTRY_IO_BYTES);
    read_cost_ms=0;shutdown_cache();
    puts("PASS 8ms soft read slice stops after the crossing 4KiB call (9ms injected), including tick wrap");

    /* Speculative reads, active/queued writes and already-ready views get no urgent I/O. */
    ui_reader_cache_preload(0,1);reads=io_reads;unsigned opens=io_opens;
    CHECK(!ui_reader_cache_worker_priority(NULL)&&io_reads==reads&&io_opens==opens);
    CHECK(ui_reader_open(0));state.ready=true;
    CHECK(!ui_reader_cache_worker_priority(NULL)&&io_reads==reads&&io_opens==opens);
    state.ready=false;priority_other_queue();
    CHECK(!ui_reader_cache_worker_priority(NULL)&&io_reads==reads&&io_opens==opens);
    shutdown_cache();publish(0);show_page(97);settle();
    entry_store_job_t *pending=entry_store.pending;CHECK(pending);
    writes=io_writes;CHECK(!ui_reader_cache_worker_priority(NULL)&&entry_store.pending==pending&&io_writes==writes);
    CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(!ui_reader_cache_worker_priority(NULL)&&entry_store.job==pending&&io_writes==writes);
    shutdown_cache();publish(0);shutdown_cache();CHECK(ui_reader_open(0));
    /* Even an allowed optional writer must not be dequeued by the urgent path. */
    pending=epd_app_alloc(sizeof(*pending),EPD_APP_PSRAM);CHECK(pending);memset(pending,0,sizeof(*pending));pending->fd=-1;pending->writing=true;pending->work=entry_work_new(16384u,false);CHECK(pending->work);
    entry_store.pending=pending;entry_store.write_allowed=true;writes=io_writes;
    CHECK(ui_reader_cache_worker_priority(NULL)&&entry_store.pending==pending&&entry_store.job&&!entry_store.job->writing&&io_writes==writes);
    shutdown_cache();
    puts("PASS selected-read priority excludes shelf-only/other-book/ready-page reads and active or queued optional writes");

    for(unsigned change=0;change<6;++change) {
        CHECK(ui_reader_open(0));
        CHECK(ui_reader_cache_worker_step(NULL,false)&&ui_reader_cache_worker_step(NULL,false));
        CHECK(entry_store.job&&entry_store.job->phase==ENTRY_PIXELS);
        reads=io_reads;writes=io_writes;
        if(change==0)read_hook=priority_cancel;
        if(change==1)read_hook=ui_reader_close;
        if(change==2)read_hook=priority_other_queue;
        if(change==3)ui_reader_cache_quiesce();
        if(change==4)service_cancelled=true;
        if(change==5)++file_revision;
        epd_service_t service=0;
        CHECK(ui_reader_cache_worker_priority(&service));
        CHECK(!read_hook&&io_reads-reads==(change<3?1u:0u)&&io_writes==writes);
        CHECK(!entry_store.ready&&!preview);
        if(change!=1)CHECK(!entry_store.job);
        memory_pressure=service_cancelled=false;if(change==5)--file_revision;
        shutdown_cache();
    }
    CHECK(ui_reader_open(0));allocation_hook=priority_other_queue;reads=io_reads;opens=io_opens;
    CHECK(!ui_reader_cache_worker_priority(NULL)&&!allocation_hook&&entry_store.next==0&&entry_store.count==1);
    CHECK(io_reads==reads&&io_opens==opens&&!entry_store.job);
    shutdown_cache();frame=small;
    puts("PASS urgent read cancellation/close/switch/source/session gates and low-memory/service stop; allocation-window replacement preserves the new queue without reading it");
}
static void disk_budget_entry_test(void) {
    shutdown_cache();publish(0);drain();shutdown_cache();
    ui_reader_cache_preload(0,1);CHECK(ui_reader_cache_worker_step(NULL,false));
    CHECK(entry_store.job && entry_store.job->fd>=0 && entry_store.job->disk_lease);
    char path[STORAGE_PATH_MAX];strcpy(path,entry_store.job->path);
    epd_cold_budget_snapshot_t before,after;epd_cold_budget_snapshot(STORAGE_FLASH,&before);
    CHECK(before.active==1);
    epd_cold_lease_t rejected=0;
    CHECK(epd_cold_budget_begin(STORAGE_FLASH_ROOT "/system/new-state",EPD_COLD_REQUIRED,
        before.limit-before.index_bytes,0,&rejected)==EPD_COLD_BUDGET_FULL);
    CHECK(!rejected && _access(path,0)==0);
    drain();CHECK(ui_reader_open(0) && ui_reader_preview_active());shutdown_cache();
    epd_cold_budget_snapshot(STORAGE_FLASH,&after);
    CHECK(!after.active && !after.reserved && !after.deferred);
    puts("PASS real entry read lease survives common-budget eviction; preview completes and disk obligations drain");
}
int main(void) {
    CHECK(storage_mkdirs(STORAGE_FLASH_ROOT "/system") && cold_budget_init()==RT_EOK);
    manager_setup();
    _set_fmode(_O_BINARY);
    for(unsigned i=0;i<24;++i) { snprintf(books[i].file.path,256,"./flash/%u.epub",i);snprintf(books[i].file.name,128,"book %u",i); books[i].file.format=2; }
    CHECK(ui_reader_open(0)&&calls==0&&!ui_reader_view()->ready&&ui_reader_waiting());
    check_diagnostic("preview=0 preview_bound=0 entrymatch=0");
    CHECK(ui_reader_view()->font && !strcmp(ui_reader_identity(),books[0].file.path));
    CHECK(ui_reader_step(8,true,true)==UI_READER_WORK_TARGET&&calls==0);
    ui_reader_first_frame();ui_reader_step(8,false,true);ui_reader_step(8,true,false);CHECK(calls==0);
    ui_reader_turn(1);ui_reader_seek(3);CHECK(turns==0);
    ui_reader_close();ui_reader_step(8,true,true);CHECK(calls==0);
    publish(0);CHECK(calls==1&&live_allocations==1);
    CHECK(ui_reader_open(0)&&ui_reader_preview_active()&&calls==1);
    CHECK(ui_reader_bind(&host_body)&&preview_object&&preview->bitmap.image.data!=frame.data);
    check_diagnostic("preview=1 preview_bound=1 entrymatch=1");
    CHECK(!memcmp(preview->bitmap.image.data,frame.data,frame.data_size));
    ui_reader_first_frame();ui_reader_step(8,true,true);CHECK(calls==2&&preview_object);
    state.ready=true;CHECK(ui_reader_bind(&host_body)&&!preview_object&&!preview&&host_body.count==1);
    /* The backend double binds page zero, advancing the anchor away from the cached page. */
    check_diagnostic("preview=0 preview_bound=0 entrymatch=0");
    ui_reader_close();CHECK(!host_body.count);
    ++font_revision;CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--font_revision;
    ++settings[2];CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--settings[2];
    ++host_area.width;CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--host_area.width;
    ++books[0].position.source.chapter;CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--books[0].position.source.chapter;
    ++books[0].file.modified;CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--books[0].file.modified;
    ++file_revision;CHECK(ui_reader_open(0)&&!ui_reader_preview_active());--file_revision;
    publish(1);publish(2);publish(3);publish(4);CHECK(live_allocations==4);
    CHECK(ui_reader_open(0)&&!ui_reader_preview_active());
    CHECK(ui_reader_open(1)&&ui_reader_preview_active());ui_reader_bind(&host_body);
    ++settings[0];CHECK(!ui_reader_bind(&host_body)&&!preview_object);--settings[0];
    /* Restart removes all RAM entries. The worker restores a durable record. */
    ui_reader_shutdown(); CHECK(!live_allocations);
    ++font_revision;
    ui_reader_cache_preload(1,1);drain();
    CHECK(ui_reader_open(1)&&ui_reader_preview_active());
    CHECK(ui_reader_bind(&host_body));
    shutdown_cache(); ++books[1].file.modified;
    ui_reader_cache_preload(1,1);drain();CHECK(!live_allocations);
    --books[1].file.modified;
    /* A layout-key change and truncated/corrupt pixels must fall back safely. */
    ++settings[1];ui_reader_cache_preload(1,1);drain();CHECK(!live_allocations);--settings[1];
    ui_reader_cache_preload(1,1);drain();CHECK(live_allocations==1);
    entry_store_key_t disk_key;entry_store_key(&disk_key,&entries[3]);
    for(unsigned i=0;i<ENTRY_SLOTS;++i)if(entries[i].bitmap.memory)entry_store_key(&disk_key,&entries[i]);
    char disk_path[STORAGE_PATH_MAX];CHECK(entry_store_path(&disk_key,STORAGE_FLASH,disk_path,sizeof(disk_path)));
    ui_reader_shutdown();FILE *corrupt=fopen(disk_path,"wb");CHECK(corrupt);fwrite("bad",1,3,corrupt);fclose(corrupt);
    ui_reader_cache_preload(1,1);drain();CHECK(!live_allocations);
    /* File mutation after open cannot tag an old frame with a fresh version. */
    ui_reader_shutdown();CHECK(!live_allocations);CHECK(ui_reader_open(0));++file_revision;
    ui_reader_first_frame();ui_reader_step(8,true,true);state.ready=true;ui_reader_bind(&host_body);ui_reader_cache_entry(&host_body);CHECK(!live_allocations);
    ui_reader_shutdown();available=0;publish(0);CHECK(!live_allocations);
    ui_reader_shutdown();available=8u*1024u*1024u;fail_open=true;
    CHECK(ui_reader_open(0));ui_reader_first_frame();ui_reader_step(8,true,true);
    CHECK(ui_reader_view()->error_code&&!ui_reader_preview_active());
    ui_reader_shutdown();CHECK(!host_body.count&&!live_allocations);
    puts("PASS immediate loading, actual-frame admission, cancellation, independent cached pixels, 4-entry bound, durable restart, metadata identity, reserve, font/geometry/anchor/file invalidation, failed attach");
    fail_open=false;constrained_cache_tests();
    preload_promotion_tests();
    pixel_budget_tests();
    pixel_reuse_tests();
    preload_snapshot_race_tests();
    sustained_pressure_tests();
    resident_cleanup_tests();
    priority_target_tests();
    CHECK(activity_queued==activity_closed && !activity_tasks);
    activity_admission=0; unsigned reads=io_reads,opens=io_opens;
    ui_reader_cache_preload(0,1); CHECK(entry_store.work && activity_tasks==1);
    CHECK(!ui_reader_cache_worker_step(NULL,false) && entry_store.job && entry_store.job->fd==-1);
    CHECK(io_reads==reads && io_opens==opens);
    ui_reader_cache_quiesce(); CHECK(ui_reader_cache_worker_step(NULL,false));
    shutdown_cache(); CHECK(!activity_tasks && activity_queued==activity_closed);
    activity_admission=1;
    puts("PASS queued entry admission waits without I/O; cancellation bypasses admission and drains TASKS exactly once");
    disk_budget_entry_test();
    return 0;
}
