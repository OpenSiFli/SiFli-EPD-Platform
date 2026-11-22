#include "lvgl.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/draw/lv_image_decoder_private.h"
#include "ui_home_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define GUI_APP_ID_MAX_LEN 16
#define HOME_PAGE_SIZE 16
#define FOCUS_CAPACITY 32
#define TEXT_BINDING_CAPACITY 11
#define WIDTH 684
#define HEIGHT 1216
#define MARGIN epd_ui_px(32)
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define NAV_IDLE (-1)
#define NAV_BACK (-2)
#define POPUP_CLOSE (-5)
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_FLASH_APPS "/flash/apps"
#define STORAGE_SD_APPS "/sdcard/.epd/apps"
#define STORAGE_APP_CODE 0
#define RT_EOK 0
#define EPD_APP_PSRAM 1
#define RTM_EXPORT(x)
#define BUILTIN_APP_EXPORT(...)
#define LV_EXT_STR_ID(x) 0
#define S_ISREG(x) (((x) & _S_IFMT) == _S_IFREG)
typedef int rt_err_t, lv_ext_str_id_t, ui_page_id_t;
enum { UI_PAGE_HOME, UI_PAGE_SETTINGS };
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef void *intent_t;
typedef struct { const char *id; } builtin_app_desc_t;
#include "types_actual.h"

static page_context_t context, *current = &context;
static int pending_page = NAV_IDLE;
static epd_app_entry_t catalog[40];
static unsigned catalog_count, scans, sets, text_calls, source_sets, font_creates, font_destroys;
static uint32_t revision = 1, volume_revision = 1, session = 1, locale = 1, scale = 1;
static unsigned percent = 100;
static int content_bottom = HEIGHT;
static bool changing, release_requested, installer_busy, refresh_ok = true, fail_alloc;
static bool race_revision, fail_text, fail_timer;
static size_t available = 8 * 1024 * 1024, owned_bytes, owned_peak;
static size_t cached_clean_bytes;
static unsigned owned_count;
static struct { void *ptr; size_t size; } allocations[64];
static unsigned reads, opens, closes, live_files;
static uint8_t fallback_pixel[4] = {0, 0, 0, 255};
static const lv_image_dsc_t ui_icon_album = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4},
    .data_size = sizeof(fallback_pixel), .data = fallback_pixel
};

static void *epd_app_alloc(size_t bytes, int region)
{
    CHECK(region == EPD_APP_PSRAM);
    if (fail_alloc) return NULL;
    void *p = malloc(bytes); CHECK(p);
    for (unsigned i = 0; i < 64; ++i) if (!allocations[i].ptr) {
        allocations[i].ptr = p; allocations[i].size = bytes;
        owned_bytes += bytes; ++owned_count;
        if (owned_bytes > owned_peak) owned_peak = owned_bytes;
        return p;
    }
    CHECK(false); return NULL;
}
static void epd_app_free(void *p)
{
    if (!p) return;
    for (unsigned i = 0; i < 64; ++i) if (allocations[i].ptr == p) {
        owned_bytes -= allocations[i].size; --owned_count;
        allocations[i].ptr = NULL; free(p); return;
    }
    CHECK(false);
}
static size_t epd_app_psram_available(void) { return available > owned_bytes ? available - owned_bytes : 0; }
#define PSRAM_CACHE_WB 1
static int mpu_dcache_clean(void *data,uint32_t size)
{
    CHECK(data&&size);
    for(unsigned i=0;i<64;++i)if(allocations[i].ptr==data){
        CHECK(size==allocations[i].size); cached_clean_bytes+=size;
    }
    return 0;
}
static bool storage_changing(void) { return changing; }
static bool storage_release_requested(void) { return release_requested; }
static uint32_t storage_card_session(void) { return session; }
static uint32_t storage_revision(void) { return volume_revision; }
static bool storage_session_valid(storage_volume_t v, uint32_t s) { return v == STORAGE_FLASH || s == session; }
static bool storage_path_available(const char *p) { (void)p; return !changing && !release_requested; }
static void storage_lock(void) {}
static void storage_unlock(void) {}
static bool storage_record_load(const char *p, unsigned m, unsigned v, void *d, size_t n)
{ (void)p; (void)m; (void)v; (void)d; (void)n; return false; }
static bool storage_record_save(const char *p, unsigned m, unsigned v, const void *d, size_t n)
{ (void)p; (void)m; (void)v; (void)d; (void)n; return true; }
bool app_catalog_refresh(void) { ++scans; if (race_revision) { ++revision; race_revision = false; } return refresh_ok; }
uint32_t app_catalog_revision(void) { return revision; }
unsigned app_catalog_count(void) { return catalog_count; }
const epd_app_entry_t *app_catalog_get(unsigned i) { return i < catalog_count ? &catalog[i] : NULL; }
bool app_installer_busy(void) { return installer_busy; }
void app_installer_result(app_job_result_t *r) { memset(r, 0, sizeof(*r)); }
const char *app_installer_message(app_job_message_t m) { (void)m; return "result"; }
bool app_installer_start_uninstall(const char *id, storage_volume_t v, uint32_t *ticket)
{ (void)id; (void)v; (void)ticket; return false; }
static bool storage_app_path(char *p, size_t n, const char *id, int kind, const char *file)
{
    (void)kind;
    for (unsigned i = 0; i < catalog_count; ++i) if (!strcmp(catalog[i].id, id)) {
        snprintf(p, n, "%s/%s/%s", catalog[i].volume == STORAGE_SD ? STORAGE_SD_APPS : STORAGE_FLASH_APPS, id, file);
        return true;
    }
    return false;
}
static uint32_t epd_locale_revision(void) { return locale; }
static unsigned epd_ui_scale_percent(void) { return percent; }
static uint32_t epd_ui_scale_revision(void) { return scale; }
static int epd_ui_px(int v) { return v * (int)percent / 100; }
static int epd_ui_touch(int v) { return epd_ui_px(v); }
static int ui_shell_content_bottom(void) { return content_bottom; }
static const lv_font_t *ui_font_body(void) { return LV_FONT_DEFAULT; }
static const lv_font_t *ui_font_title(void) { return LV_FONT_DEFAULT; }
static lv_font_t *ui_font_reader_create(unsigned f, int size, unsigned w, uint32_t *id, lv_font_t **fallback)
{ (void)f; (void)size; (void)w; ++font_creates; *id = 1; *fallback = NULL; return NULL; }
static void ui_font_reader_destroy(lv_font_t *f) { if (f) ++font_destroys; }
static bool text_update(lv_obj_t *o, const char *text)
{
    ++text_calls;
    if (fail_text) { current->locale_retry = true; return false; }
    if (strcmp(lv_label_get_text(o), text)) { ++sets; lv_label_set_text(o, text); }
    return true;
}
static void object_visible(lv_obj_t *o, bool show)
{ if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static bool focus_eligible(lv_obj_t *o) { return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void focus_restore(unsigned i) { (void)i; }
static bool lv_refreshing_done(void) { return true; }
static void epd_obj_init(lv_obj_t *o)
{ lv_obj_remove_style_all(o); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE); }
static lv_obj_t *panel_create(lv_obj_t *p, int x, int y, int w, int h)
{ lv_obj_t *o = lv_obj_create(p); epd_obj_init(o); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h); return o; }
static lv_obj_t *button_create(lv_obj_t *p, int x, int y, int w, int h, const char *t, int action)
{ (void)t; (void)action; return panel_create(p,x,y,w,h); }
static lv_obj_t *centered_label(lv_obj_t *p, const char *t, int x, int y, int w, const lv_font_t *font)
{ lv_obj_t *o = lv_label_create(p); lv_label_set_text(o,t); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w); lv_obj_set_style_text_font(o,font,0); return o; }
static lv_obj_t *centered_label_i18n(lv_obj_t *p, int t, int x, int y, int w, const lv_font_t *font)
{ (void)t; return centered_label(p,"memory",x,y,w,font); }
static lv_obj_t *button_create_i18n(lv_obj_t *p, int x, int y, int w, int h, int t, int action)
{ (void)t; lv_obj_t *o = button_create(p,x,y,w,h,NULL,action); centered_label(o,"button",0,0,w,ui_font_body()); return o; }
static lv_obj_t *icon_create(lv_obj_t *p, const void *src, int x, int y)
{ lv_obj_t *o = lv_image_create(p); lv_image_set_src(o,src); lv_obj_set_pos(o,x,y); return o; }
static void popup_close(void) { if (current->popup) lv_obj_delete(current->popup); current->popup = NULL; }
static void popup_open(const char *s, const char *d) { (void)s; (void)d; }
static void popup_open_i18n(int s, const char *d) { (void)s; (void)d; }
static void popup_confirm_i18n(int s, const char *d, int a) { (void)s; (void)d; (void)a; }
static void epd_app_header(lv_obj_t *screen) { (void)screen; }
static int ui_navigation_start(const char *id, int page) { (void)id; (void)page; return 0; }
static bool ui_navigation_run(const char *id) { (void)id; return true; }
static const builtin_app_desc_t *gui_builtin_app_list_open(void) { return NULL; }
static const builtin_app_desc_t *gui_builtin_app_list_get_next(const builtin_app_desc_t *p) { (void)p; return NULL; }
static void gui_builtin_app_list_close(const builtin_app_desc_t *p) { (void)p; }
static int app_recovery_check_dynamic(void) { return 0; }
static void rt_set_errno(int e) { (void)e; }
static intent_t intent_init(const char *s) { (void)s; return NULL; }
static int intent_set_string(intent_t i, const char *k, const char *v) { (void)i; (void)k; (void)v; return 0; }
static int ui_navigation_request(intent_t i) { (void)i; return 0; }
static void intent_deinit(intent_t i) { (void)i; }
static void gui_app_exec_now(void) {}
static bool gui_app_is_actived(char *s) { (void)s; return true; }
void home_resume(void);

/* Simulated devices; all image decoding and widget/resource lifetimes use the
 * real LVGL, app_image and SDK decoder implementations. */
static struct file { char path[256]; uint8_t *bytes; size_t size; bool mapped; } files[40];
static struct handle { struct file *file; uint32_t offset; } handles[64];
static unsigned file_count;
static struct file *file_find(const char *path)
{ for (unsigned i = 0; i < file_count; ++i) if (!strcmp(path,files[i].path)) return &files[i]; return NULL; }
static int host_stat(const char *p, struct stat *s)
{ struct file *f = file_find(p); if (!f) return -1; memset(s,0,sizeof(*s)); s->st_mode = _S_IFREG; s->st_size = (long)f->size; return 0; }
#define stat(p,s) host_stat(p,s)
static lv_fs_res_t host_fs_open(lv_fs_file_t *f, const char *p, lv_fs_mode_t mode)
{
    (void)mode; struct file *file = file_find(p); if (!file || changing || release_requested) return LV_FS_RES_NOT_EX;
    for (unsigned i = 0; i < 64; ++i) if (!handles[i].file) {
        handles[i].file = file; handles[i].offset = 0; f->file_d = (void *)(uintptr_t)(i+1);
        ++opens; ++live_files; return LV_FS_RES_OK;
    }
    CHECK(false); return LV_FS_RES_OUT_OF_MEM;
}
static struct handle *handle(lv_fs_file_t *f) { return &handles[(uintptr_t)f->file_d-1]; }
static lv_fs_res_t host_fs_close(lv_fs_file_t *f)
{ CHECK(handle(f)->file); handle(f)->file = NULL; --live_files; ++closes; return LV_FS_RES_OK; }
static lv_fs_res_t host_fs_read(lv_fs_file_t *f, void *out, uint32_t n, uint32_t *got)
{
    struct handle *h=handle(f); ++reads; *got=(uint32_t)LV_MIN(n,h->file->size-h->offset);
    memcpy(out,h->file->bytes+h->offset,*got); h->offset+=*got; return LV_FS_RES_OK;
}
static lv_fs_res_t host_fs_seek(lv_fs_file_t *f,uint32_t p,lv_fs_whence_t whence)
{ struct handle *h=handle(f); h->offset=(whence==LV_FS_SEEK_END?(uint32_t)h->file->size:whence==LV_FS_SEEK_CUR?h->offset:0)+p; return LV_FS_RES_OK; }
static lv_fs_res_t host_fs_tell(lv_fs_file_t *f,uint32_t *pos) { *pos=handle(f)->offset; return LV_FS_RES_OK; }
static void *driver_open(lv_fs_drv_t *drv,const char *path,lv_fs_mode_t mode)
{ (void)drv; char full[256]; snprintf(full,sizeof(full),"/%s",path); lv_fs_file_t f={0}; return host_fs_open(&f,full,mode)==LV_FS_RES_OK?f.file_d:NULL; }
static lv_fs_res_t driver_close(lv_fs_drv_t *drv,void *fd)
{ (void)drv; lv_fs_file_t f={.file_d=fd}; return host_fs_close(&f); }
static lv_fs_res_t driver_read(lv_fs_drv_t *drv,void *fd,void *data,uint32_t n,uint32_t *got)
{ (void)drv; lv_fs_file_t f={.file_d=fd}; return host_fs_read(&f,data,n,got); }
static lv_fs_res_t driver_seek(lv_fs_drv_t *drv,void *fd,uint32_t pos,lv_fs_whence_t whence)
{ (void)drv; lv_fs_file_t f={.file_d=fd}; return host_fs_seek(&f,pos,whence); }
static lv_fs_res_t driver_tell(lv_fs_drv_t *drv,void *fd,uint32_t *pos)
{ (void)drv; lv_fs_file_t f={.file_d=fd}; return host_fs_tell(&f,pos); }
#define RT_USING_MTD_NOR 1
#define LV_USE_EZIP 1
#define RT_NULL NULL
#define RT_Device_Class_MTD 1
#define F_GET_PHY_ADDR 1
typedef uintptr_t rt_uint32_t; /* Host addresses must fit the mapped-flash shim. */
typedef struct { int type; } *rt_device_t;
static struct { int type; } flash_device = {RT_Device_Class_MTD}, sd_device = {0};
struct dfs_fd { struct { rt_device_t dev_id; } *fs; };
static struct dfs_fd *fd_get(int fd)
{ static struct { rt_device_t dev_id; } fs; static struct dfs_fd f; fs.dev_id=(rt_device_t)(handles[fd].file->mapped?&flash_device:&sd_device); f.fs=(void*)&fs; return &f; }
static void fd_put(struct dfs_fd *f) { (void)f; }
static int ioctl(int fd,int request,rt_uint32_t *addr)
{ (void)request; *addr=(uintptr_t)handles[fd].file->bytes; return 0; }
static void counted_image_set_src(lv_obj_t *image,const void *source)
{ ++source_sets; lv_image_set_src(image,source); }
static lv_timer_t *checked_timer_create(lv_timer_cb_t callback,uint32_t period,void *data)
{ return fail_timer?NULL:lv_timer_create(callback,period,data); }
#define lv_image_set_src counted_image_set_src
#define lv_timer_create checked_timer_create
#include "decoder_actual.inc"
#include "image_actual.inc"
#include "home_actual.inc"

static void entry_add(unsigned index, size_t bytes, bool mapped)
{
    epd_app_entry_t *entry=&catalog[index]; memset(entry,0,sizeof(*entry));
    snprintf(entry->id,sizeof(entry->id),"app%u",index);
    snprintf(entry->name,sizeof(entry->name),"Application %u",index);
    strcpy(entry->icon_file,"icon.ezip"); entry->volume=mapped?STORAGE_FLASH:STORAGE_SD;
    entry->version=1;
    struct file *f=&files[index];
    snprintf(f->path,sizeof(f->path),"%s/%s/icon.ezip",mapped?STORAGE_FLASH_APPS:STORAGE_SD_APPS,entry->id);
    f->size=sizeof(lv_image_header_t)+bytes; f->mapped=mapped; f->bytes=calloc(1,f->size); CHECK(f->bytes);
    lv_image_header_t header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RAW,.flags=LV_IMAGE_FLAGS_EZIP,.w=100,.h=100,.stride=300};
    memcpy(f->bytes,&header,sizeof(header)); memset(f->bytes+sizeof(header),(int)(index+1),bytes);
    file_count=catalog_count=index+1;
}
static void setup(unsigned count,size_t bytes)
{
    CHECK(!owned_count&&!live_files);
    memset(&context,0,sizeof(context)); current=&context; context.id=UI_PAGE_HOME; context.foreground=true;
    changing=release_requested=installer_busy=fail_alloc=fail_text=race_revision=fail_timer=false; refresh_ok=true;
    revision=volume_revision=session=locale=scale=1; percent=100; content_bottom=HEIGHT;
    available=8*1024*1024; scans=sets=text_calls=source_sets=reads=opens=closes=font_creates=0; owned_peak=cached_clean_bytes=0;
    catalog_count=file_count=0; saved_home_page=0;
    for(unsigned i=0;i<count;++i)entry_add(i,bytes,i%2==0);
    current->screen=lv_obj_create(NULL); lv_obj_set_size(current->screen,WIDTH,HEIGHT);
    current->group=lv_group_create();
    home_create(current->screen); lv_obj_update_layout(current->screen);
    CHECK(live_files==0&&opens==closes);
}
static void teardown(void)
{
    current=&context;
    lv_obj_t *images[HOME_SLOT_COUNT];
    for(unsigned i=0;i<HOME_SLOT_COUNT;++i)images[i]=home_state()->slots[i].image;
    home_stop(); CHECK(!current->feature&&!owned_bytes&&!owned_count);
    for(unsigned i=0;i<HOME_SLOT_COUNT;++i){
        const void *source=lv_image_get_src(images[i]);
        CHECK(!source||source==&ui_icon_album);
        app_image_t *resource=image_resource(images[i]); CHECK(!resource||!resource->decoder.decoded);
    }
    lv_obj_delete(current->screen); lv_group_delete(current->group);
    for(unsigned i=0;i<file_count;++i)free(files[i].bytes);
    CHECK(live_files==0&&opens==closes);
}
static uint8_t first_byte(unsigned slot)
{
    const lv_image_dsc_t *d=lv_image_get_src(home_state()->slots[slot].image);
    CHECK(d&&d->data); return d->data[0];
}
static void mutate(unsigned file,uint8_t value)
{ memset(files[file].bytes+sizeof(lv_image_header_t),value,files[file].size-sizeof(lv_image_header_t)); }

int main(void)
{
    lv_init(); lv_display_t *display=lv_display_create(WIDTH,HEIGHT); CHECK(display);
    static lv_fs_drv_t driver;
    lv_fs_drv_init(&driver); driver.letter='/'; driver.open_cb=driver_open; driver.close_cb=driver_close;
    driver.read_cb=driver_read; driver.seek_cb=driver_seek; driver.tell_cb=driver_tell; lv_fs_drv_register(&driver);
    lv_sifli_img_decoder();
    setup(12,2048);
    home_state_t *h=home_state(); unsigned cold_reads=reads,cold_scans=scans,cold_sets=sets,cold_text_calls=text_calls;
    for(unsigned i=0;i<12;++i){
        app_image_t *r=image_resource(h->slots[i].image);
        CHECK(r&&r->decoder.decoded&&r->decoder.args.no_cache&&!r->decoder.cache_entry);
        CHECK((r->decoder.user_data==NULL)==files[i].mapped);
    }
    lv_area_t before[HOME_SLOT_COUNT];
    for(unsigned i=0;i<HOME_SLOT_COUNT;++i)lv_obj_get_coords(h->slots[i].tile,&before[i]);
    unsigned cold_source_sets=source_sets;
    home_resume(); CHECK(scans==cold_scans&&reads==cold_reads&&source_sets==cold_source_sets&&text_calls==cold_text_calls);
    unsigned warm_source_sets=0;
    for(unsigned pass=0;pass<100;++pass){
        home_pause();
        CHECK(h->cache_bytes==12*2048&&owned_bytes==h->cache_bytes);
        if(!pass)warm_source_sets=source_sets;
        CHECK(source_sets==warm_source_sets);
        for(unsigned i=0;i<12;++i){
            app_image_t *r=image_resource(h->slots[i].image); CHECK(r&&!r->decoder.decoded);
            CHECK((h->slots[i].cached_icon.header.flags&LV_IMAGE_FLAGS_EZIP)&&
                  !(h->slots[i].cached_icon.header.flags&LV_IMAGE_FLAGS_ALLOCATED));
            lv_image_decoder_dsc_t decoded;
            CHECK(lv_image_decoder_open(&decoded,&h->slots[i].cached_icon,NULL)==LV_RESULT_OK);
            CHECK(decoded.decoded->data==h->slots[i].cached_icon.data&&!decoded.user_data);
            lv_image_decoder_close(&decoded);
        }
        home_cache_trim(h->cache_timer); CHECK(source_sets==warm_source_sets&&owned_bytes==12*2048);
        home_resume(); home_process();
        CHECK(scans==cold_scans&&reads==cold_reads&&sets==cold_sets&&text_calls==cold_text_calls&&source_sets==warm_source_sets);
    }
    lv_obj_update_layout(current->screen);
    CHECK(cached_clean_bytes==12*2048);
    for(unsigned i=0;i<HOME_SLOT_COUNT;++i){lv_area_t after;lv_obj_get_coords(h->slots[i].tile,&after);CHECK(!memcmp(&before[i],&after,sizeof(after)));}
    printf("PASS warm return: 100 cycles, catalog scans %u -> %u, image reads %u -> %u, cache %zu bytes\n",cold_scans,scans,cold_reads,reads,owned_bytes);

    /* Copy isolation covers NOR direct mapping as well as TF decoder RAM. */
    mutate(0,77); mutate(1,78); CHECK(first_byte(0)==1&&first_byte(1)==2);
    ++revision; home_resume(); CHECK(first_byte(0)==77&&first_byte(1)==78&&owned_bytes==0);
    CHECK(scans==cold_scans+1); home_pause(); home_resume();
    ++session; mutate(1,79); home_resume(); CHECK(first_byte(1)==79);
    home_pause(); ++volume_revision; home_resume(); CHECK(!owned_bytes);
    puts("PASS same-path replacement, mapped-flash isolation, card session and volume revision");

    home_pause(); changing=true; release_requested=true; home_pause();
    CHECK(!owned_bytes); unsigned blocked_reads=reads,blocked_scans=scans;
    home_resume(); CHECK(reads==blocked_reads&&scans==blocked_scans);
    changing=false; home_resume(); CHECK(reads==blocked_reads); /* Timed-out draining still has a release request. */
    release_requested=false; ++session; ++volume_revision; home_resume(); CHECK(reads>blocked_reads);
    home_pause(); current=NULL; available=0; home_cache_trim(h->cache_timer);
    CHECK(!owned_bytes); current=&context; available=8*1024*1024;
    unsigned scans_before=scans; home_resume(); CHECK(scans==scans_before&&first_byte(0)==77);
    available=0; home_pause(); CHECK(!owned_bytes); available=8*1024*1024; home_resume();
    fail_alloc=true; home_pause(); CHECK(!owned_bytes); fail_alloc=false; home_resume();
    home_pause(); current=NULL; release_requested=true; home_cache_trim(h->cache_timer); CHECK(!owned_bytes);
    current=&context; release_requested=false; home_resume();
    home_pause(); current=NULL; installer_busy=true; home_cache_trim(h->cache_timer); CHECK(!owned_bytes);
    current=&context; installer_busy=false; home_resume();
    puts("PASS USB/removal barriers, background pressure, allocation failure and installer trim");

    home_pause(); unsigned fonts=font_creates,scale_reads=reads; percent=125; ++scale; home_resume();
    CHECK(font_creates==fonts+1&&reads==scale_reads&&h->scale_revision==scale);
    content_bottom-=100; home_resume(); CHECK(h->content_bottom==content_bottom&&reads==scale_reads);
    ++locale; strcpy(catalog[0].name,"Localized"); home_resume(); CHECK(!strcmp(lv_label_get_text(h->slots[0].label),"Localized"));
    ++locale; fail_text=true; home_resume(); CHECK(h->locale_revision!=locale&&current->locale_retry);
    fail_text=false; current->locale_retry=false; home_process(); CHECK(h->locale_revision==locale);
    puts("PASS scale/content geometry, language and translation retry");

    home_pause(); ++revision; refresh_ok=false; home_resume(); CHECK(h->revision!=revision&&!owned_bytes);
    refresh_ok=true; home_process(); CHECK(h->revision==revision);
    ++revision; race_revision=true; home_resume(); CHECK(h->revision!=revision); home_process(); CHECK(h->revision==revision);
    home_pause(); catalog_count=3; ++revision; home_resume(); CHECK(!h->slots[3].id[0]&&h->grid_count==3);
    CHECK(lv_obj_has_flag(h->slots[3].tile,LV_OBJ_FLAG_HIDDEN));
    home_pause(); teardown();
    puts("PASS failed catalog refresh, concurrent revision, uninstall and stop cleanup");

    setup(24,20*1024); h=home_state();
    CHECK(ui_home_model_pin(&h->model,"app23")); home_update(false); home_pause();
    CHECK(owned_bytes<=HOME_ICON_CACHE_BYTES&&owned_peak<=HOME_ICON_CACHE_BYTES);
    unsigned page_reads=reads; home_resume(); CHECK(reads>page_reads); /* Uncached overflow is reloaded. */
    home_action(HOME_NEXT); CHECK(h->page==1&&first_byte(0)==17);
    home_pause(); home_action(HOME_PREVIOUS); CHECK(h->page==0&&first_byte(0)==1);
    CHECK(!strcmp(h->slots[UI_HOME_GRID_CAPACITY].id,"app23"));
    home_pause(); home_pause(); CHECK(owned_bytes<=HOME_ICON_CACHE_BYTES);
    teardown();
    puts("PASS 256 KiB budget, overflow reload, page/dock changes and repeated pause");

    setup(1,256*1024); h=home_state(); /* File limit includes its header. */
    CHECK(!h->slots[0].icon_ready&&lv_image_get_src(h->slots[0].image)==&ui_icon_album);
    home_pause(); home_resume(); CHECK(!owned_bytes); teardown();
    puts("PASS invalid image fallback remains retryable");
    setup(2,2048); h=home_state(); fail_timer=true; home_pause();
    CHECK(!h->cache_timer&&!owned_bytes&&!image_resource(h->slots[0].image)->decoder.decoded);
    fail_timer=false; home_resume(); home_pause(); CHECK(owned_bytes==4096); teardown();
    puts("PASS timer allocation failure, idle trim/repeated pause avoid source changes, no dangling stop sources");
    lv_sifli_img_decoder_deinit(); lv_display_delete(display); lv_deinit();
    return 0;
}
