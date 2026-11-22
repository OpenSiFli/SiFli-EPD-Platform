#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"
#include "ui_control_center.h"
#include "ui_settings.h"
#include "platform/epd_system.h"
#include "platform/epd_sound.h"
#define L2_RET_BSS_SECT_BEGIN(name)
#define L2_RET_BSS_SECT(name)
#define L2_RET_BSS_SECT_END
#define INIT_COMPONENT_EXPORT(fn)

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
enum { RT_EOK, STORAGE_SD, UI_PAGE_WIFI_SETTINGS, UI_PAGE_BLUETOOTH_SETTINGS,
       EPD_FONT_TITLE, EPD_FONT_BODY, EPD_FONT_CAPTION,
       platform_notification_center, platform_control_center, platform_no_notifications,
       platform_no_media, platform_control_failed };
enum { FRONTLIGHT_STORE_READY, FRONTLIGHT_STORE_SAVE_ERROR };
typedef struct { uint32_t key; uint16_t repeat; } epd_input_event_t;
typedef struct { bool (*move)(const epd_input_event_t *, unsigned); bool (*key)(uint32_t); } epd_input_ops_t;
typedef struct { bool available, locked, output_valid; unsigned output_percent; int store_status; } frontlight_snapshot_t;
typedef struct { char id[32], name[64], icon_file[64]; const void *icon; int volume; } epd_app_entry_t;
typedef void *intent_t;
static bool locked, idle = true, sound_ready = true, storage_busy, group_fail;
static unsigned scale = 100, scale_revision = 1, locale_revision = 1, catalog_revision = 1;
static unsigned panels, labels, icons, font_creates, font_destroys, groups, full, interaction, writes;
static unsigned notice_revision = 1, notice_count, sound_reads, sound_max = 16;
static unsigned reconcile_count;
static epd_sound_save_status_t sound_save;
static bool light_save_failed;
static bool light_ready = true, radios_enabled = true;
static unsigned light_percent = 50, sound_percent = 50, navigation_calls;
static bool navigation_accept = true;
static epd_media_t published_media;
static bool media_present;
static unsigned media_generation=1, filesystem_opens;
static epd_notification_t notices[128];
static epd_app_entry_t *catalog;
static unsigned catalog_count;
static uint32_t now;
static lv_font_t font;
static void *ttf_data;
static size_t ttf_size;
static lv_font_t *role_fonts[3];
/* Alpha edges and contrasting pixels expose lost background/disabled blending. */
static const uint8_t pixel[] = {
    0,0,0,0,       32,32,32,128, 32,32,32,128, 0,0,0,0,
    32,32,32,128, 32,32,32,255, 96,96,96,255, 32,32,32,128,
    32,32,32,128, 96,96,96,255, 32,32,32,255, 32,32,32,128,
    0,0,0,0,       32,32,32,128, 32,32,32,128, 0,0,0,0
};
static const lv_image_dsc_t ui_icon_quick = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 4, .h = 4, .stride = 16 },
    .data_size = sizeof(pixel), .data = pixel
};
#define ui_icon_notice_close ui_icon_quick
#define ui_icon_pause ui_icon_quick
#define ui_icon_play ui_icon_quick
#define ui_icon_light ui_icon_quick
#define ui_icon_volume ui_icon_quick
#define ui_icon_wifi_entry ui_icon_quick
#define ui_icon_bt_entry ui_icon_quick
#define ui_icon_lock ui_icon_quick
#define ui_icon_center_settings ui_icon_quick
#define ui_icon_music ui_icon_quick
#define ui_icon_previous ui_icon_quick
#define ui_icon_next ui_icon_quick
#define rt_malloc malloc
#define rt_free free
static int epd_ui_px(int n) { return (n * (int)scale + 50) / 100; }
static int epd_ui_touch(int n) { return LV_MAX(56, epd_ui_px(n)); }
static unsigned epd_ui_scale_percent(void) { return scale; }
static unsigned epd_ui_scale_revision(void) { return scale_revision; }
static unsigned epd_locale_revision(void) { return locale_revision; }
static const char *epd_get_str(int id) {
    switch (id) {
    case platform_notification_center: return locale_revision == 1 ? "Notifications" : "Messages";
    case platform_control_center: return "Controls";
    case platform_no_notifications: return "No notifications";
    case platform_no_media: return "No media";
    default: return "Failed";
    }
}
static const lv_font_t *epd_app_font_role(int role) {
    return ttf_data ? role_fonts[role - EPD_FONT_TITLE] : &font;
}
static lv_font_t *epd_app_font_create(int a, int b, int c, uint32_t *id, lv_font_t **fallback) {
    (void)a; (void)c; *id = ++font_creates; *fallback = NULL;
    if (ttf_data) {
        lv_font_t *out = lv_tiny_ttf_create_data_ex(ttf_data,ttf_size,b,LV_FONT_KERNING_NONE,0);
        CHECK(out); return out;
    }
    lv_font_t *out = malloc(sizeof(*out)); CHECK(out); *out = font; out->line_height = (uint16_t)b; return out;
}
static void epd_app_font_destroy(lv_font_t *f) {
    if (f) { CHECK(idle); ++font_destroys; if(ttf_data)lv_tiny_ttf_destroy(f);else free(f); }
}
static lv_obj_t *epd_app_panel(lv_obj_t *p, int x, int y, int w, int h) {
    ++panels; lv_obj_t *o = lv_obj_create(p); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o,lv_color_white(),0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h); return o;
}
static lv_obj_t *epd_app_label(lv_obj_t *p,const char *t,int x,int y,int w,const lv_font_t *f) {
    ++labels; lv_obj_t *o = lv_label_create(p); lv_label_set_text(o,t);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w);
    if(ttf_data) {
        lv_obj_set_height(o,f->line_height);
        lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);
    }
    lv_obj_set_style_text_color(o,lv_color_hex(0x222222),0);
    lv_obj_set_style_text_font(o,f,0); return o;
}
static lv_obj_t *epd_app_centered_label(lv_obj_t *p,const char *t,int x,int y,int w,const lv_font_t *f) {
    lv_obj_t *o=epd_app_label(p,t,x,y,w,f);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static lv_obj_t *epd_app_icon(lv_obj_t *p,const lv_image_dsc_t *src,int x,int y) {
    ++icons; lv_obj_t *o=lv_image_create(p); lv_image_set_src(o,src); lv_obj_set_pos(o,x,y); return o;
}
static bool epd_app_text(lv_obj_t *o,const char *t) { if(strcmp(lv_label_get_text(o),t))lv_label_set_text(o,t); return true; }
static lv_group_t *epd_app_input_group(lv_obj_t *o) { (void)o; if(group_fail)return NULL; ++groups; return lv_group_create(); }
static void epd_input_set_ops(lv_group_t *g,const epd_input_ops_t *ops) { (void)g; (void)ops; }
static bool ui_shell_locked(void) { return locked; }
static void ui_shell_lock(void) { locked=true; }
static bool epd_refresh_idle(void) { return idle; }
static void epd_refresh_request_full(void) { ++full; }
static void epd_refresh_request_interaction(void) { ++interaction; }
static unsigned app_catalog_count(void) { return catalog_count; }
static const epd_app_entry_t *app_catalog_get(unsigned i) { return i<catalog_count ? &catalog[i] : NULL; }
static unsigned app_catalog_revision(void) { return catalog_revision; }
static void frontlight_get(frontlight_snapshot_t *s) { *s=(frontlight_snapshot_t){light_ready,false,true,light_percent,light_save_failed?FRONTLIGHT_STORE_SAVE_ERROR:FRONTLIGHT_STORE_READY}; }
static bool frontlight_commit(uint8_t value) { CHECK(value<=100); light_percent=value; ++writes; return true; }
bool epd_sound_read(epd_sound_snapshot_t *s) { ++sound_reads; *s=(epd_sound_snapshot_t){0}; s->initialized=s->available=s->output_ready=sound_ready; s->max_level=(uint8_t)sound_max; s->volume[EPD_SOUND_MASTER]=(uint8_t)sound_percent; return sound_ready; }
bool epd_sound_set_volume(epd_sound_channel_t c,unsigned v) { (void)c; CHECK(v<=100); ++writes; return true; }
bool epd_sound_set_muted(bool muted) { (void)muted; ++writes; return true; }
epd_sound_save_status_t epd_sound_save_status(void) { return sound_save; }
bool ui_settings_enabled(ui_setting_id_t id) { (void)id; return radios_enabled; }
bool ui_settings_cycle(ui_setting_id_t id) { (void)id; ++writes; return true; }
static bool epd_media_read_display(epd_media_t *m,uint32_t *r,char *id,size_t n) {
    *m=published_media;*r=media_generation;if(n)snprintf(id,n,"%s",published_media.app_id);return media_present;
}
bool epd_media_send(const char *id,epd_media_command_t c) { (void)id; (void)c; return true; }
uint32_t epd_notification_revision(void) { return notice_revision; }
size_t epd_notification_read(size_t offset,epd_notification_t *out,size_t capacity,size_t *total,uint32_t *revision) {
    *total=notice_count; *revision=notice_revision; size_t count=offset<notice_count ? LV_MIN(capacity,notice_count-offset) : 0;
    memcpy(out,notices+LV_MIN(offset,notice_count),count*sizeof(*out)); return count;
}
bool epd_notification_find(const char *app,const char *id,epd_notification_t *out) {
    for(unsigned i=0;i<notice_count;++i)if(!strcmp(notices[i].app_id,app)&&!strcmp(notices[i].id,id)){*out=notices[i];return true;} return false;
}
bool epd_notification_dismiss(const char *app,const char *id) { (void)app;(void)id; if(notice_count)--notice_count; ++notice_revision; return true; }
static bool storage_changing(void) { return storage_busy; }
static bool storage_available(int volume) { (void)volume; return !storage_busy; }
static bool storage_apps_enabled(int volume) { (void)volume; return !storage_busy; }
static void epd_system_reconcile_apps(const char (*ids)[32],size_t count) { (void)ids; reconcile_count=(unsigned)count; }
static int app_recovery_check_dynamic(void) { return RT_EOK; }
static intent_t intent_init(const char *id) { return (void *)id; }
static int intent_set_string(intent_t i,const char *k,const char *v) { (void)i;(void)k;(void)v;return RT_EOK; }
static const char *intent_get_string(intent_t i,const char *k) { (void)i;(void)k;return ""; }
static void intent_deinit(intent_t i) { (void)i; }
static int ui_navigation_request(intent_t i) { (void)i; return RT_EOK; }
static bool ui_navigation_run(const char *id) { CHECK(!strcmp(id,"settings"));++navigation_calls;return navigation_accept; }
static bool ui_navigation_run_page(const char *id,int page) { (void)id;(void)page;return true; }
#ifndef CONTROL_CENTER_SOURCE
#define CONTROL_CENTER_SOURCE "control_center_actual.inc"
#endif
#include CONTROL_CENTER_SOURCE

static unsigned object_count(lv_obj_t *o) {
    unsigned count=1; for(unsigned i=0;i<lv_obj_get_child_count(o);++i)count+=object_count(lv_obj_get_child(o,i)); return count;
}
static uint32_t ticks(void) { return now; }
static void *forbidden_open(lv_fs_drv_t *driver,const char *path,lv_fs_mode_t mode) {
    (void)driver;(void)path;(void)mode;++filesystem_opens;CHECK(!"UI attempted disk I/O");return NULL;
}
static void populate(unsigned count) {
    free(catalog); catalog=calloc(count?count:1,sizeof(*catalog));CHECK(catalog);
    notice_count=catalog_count=count; ++notice_revision; ++catalog_revision;
    for(unsigned i=0;i<count;++i) {
        snprintf(catalog[i].id,sizeof(catalog[i].id),"app%u",i); strcpy(catalog[i].name,catalog[i].id);
        catalog[i].icon="S:/apps/icon.ezip"; strcpy(catalog[i].icon_file,"icon.ezip"); catalog[i].volume=STORAGE_SD;
        strcpy(notices[i].app_id,catalog[i].id); snprintf(notices[i].id,sizeof(notices[i].id),"notice%u",i);
        snprintf(notices[i].text,sizeof(notices[i].text),"Message %u",i);
    }
}

/* Exercise the actual LVGL refresh and hit testing, including the production
 * 40-row partial-buffer geometry. Frame storage exists only in this host test. */
__declspec(align(32)) static uint8_t draw_pixels[684 * 40 * 4];
static uint8_t frame_pixels[684 * 1216 * 4];
static unsigned pixel_bytes = 4;
static unsigned flush_calls, flushed_pixels, page_draws, page_clicks;
static lv_point_t touch_point;
static bool touch_down;
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels) {
    unsigned width=(unsigned)lv_area_get_width(area), height=(unsigned)lv_area_get_height(area);
    unsigned stride=lv_draw_buf_width_to_stride(width,lv_display_get_color_format(display));
    ++flush_calls;flushed_pixels+=width*height;
    for(unsigned y=0;y<height;++y)
        memcpy(frame_pixels+(((unsigned)area->y1+y)*684+(unsigned)area->x1)*pixel_bytes,pixels+y*stride,width*pixel_bytes);
    lv_display_flush_ready(display);
}
static uint64_t render(lv_display_t *display) {
    flush_calls=flushed_pixels=page_draws=0;
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(display);
    CHECK(flush_calls==31 && flushed_pixels==684*1216);
    uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<sizeof(frame_pixels);++i)if((i&3)!=3)hash=(hash^frame_pixels[i])*1099511628211ull;
    return hash;
}
static void page_event(lv_event_t *event) {
    if(lv_event_get_code(event)==LV_EVENT_DRAW_MAIN)++page_draws;
    if(lv_event_get_code(event)==LV_EVENT_SHORT_CLICKED)++page_clicks;
}
static void read_pointer(lv_indev_t *input,lv_indev_data_t *data) {
    (void)input;data->point=touch_point;data->state=touch_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void tap(lv_indev_t *input,lv_obj_t *object) {
    lv_obj_update_layout(lv_layer_top());lv_obj_update_layout(lv_screen_active());
    lv_area_t area;lv_obj_get_coords(object,&area);
    touch_point.x=(area.x1+area.x2)/2;touch_point.y=(area.y1+area.y2)/2;
    now+=20;touch_down=true;lv_indev_read(input);
    now+=60;touch_down=false;lv_indev_read(input);
}
static void rendering_test(lv_display_t *display) {
    locked=false;storage_busy=false;scale=100;++scale_revision;
    populate(3);ui_control_center_process();
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,draw_pixels,NULL,sizeof(draw_pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *screen=lv_screen_active();
    unsigned screen_events=lv_obj_get_event_count(screen);
    lv_obj_t *page=lv_obj_create(screen);lv_obj_remove_style_all(page);
    lv_obj_set_size(page,684,1216);lv_obj_set_pos(page,0,0);
    lv_obj_set_style_bg_color(page,lv_color_hex(0xCC0000),0);lv_obj_set_style_bg_opa(page,LV_OPA_COVER,0);
    lv_obj_remove_flag(page,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page,page_event,LV_EVENT_ALL,NULL);
    lv_obj_t *label=lv_label_create(page);lv_label_set_text(label,"Live page before center");
    render(display);CHECK(page_draws==31);
    lv_style_value_t value;
    CHECK(lv_obj_get_local_style_prop(screen,LV_STYLE_OPA_LAYERED,&value,0)==LV_STYLE_RES_NOT_FOUND);
    for(unsigned mode=0;mode<2;++mode) {
        ui_control_center_open(mode!=0);ui_control_center_process();
        CHECK(covered_page.screen==screen && !lv_obj_has_flag(screen,LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_event_count(screen)==screen_events+1);
        uint64_t optimized=render(display);CHECK(page_draws==0);
        page_draw_restore();
        uint64_t baseline=render(display);CHECK(page_draws==31);
        CHECK(optimized==baseline);
        page_draw_suspend();
        lv_obj_set_style_bg_color(page,lv_color_hex(0x0000CC),0);
        lv_label_set_text(label,"Live page updated behind center");
        CHECK(render(display)==optimized && page_draws==0);
        ui_control_center_close();render(display);CHECK(page_draws==31);
        CHECK(!covered_page.screen && lv_obj_get_event_count(screen)==screen_events);
        CHECK(lv_obj_get_local_style_prop(screen,LV_STYLE_OPA_LAYERED,&value,0)==LV_STYLE_RES_NOT_FOUND);
        CHECK(!strcmp(lv_label_get_text(label),"Live page updated behind center"));
        printf("PASS center render mode %u: identical pixels, underlying draw callbacks 31 -> 0, 31 flushes\n",mode);
    }
    lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input,read_pointer);lv_timer_pause(lv_indev_get_read_timer(input));
    tap(input,page);CHECK(page_clicks==1);
    light_percent=0;
    ui_control_center_open(false);ui_control_center_process();
    unsigned before_writes=writes;
    tap(input,center.light);ui_control_center_process();CHECK(writes==before_writes+1 && light_percent>0);
    CHECK(page_clicks==1 && covered_page.screen==screen);
    uint64_t before=render(display);
    light_percent=100;sound_percent=100;ui_control_center_process();
    CHECK(center.light_level==8 && center.volume_level==center.volume_max);
    CHECK(render(display)!=before && page_draws==0);
    navigation_accept=false;tap(input,center.settings);ui_control_center_process();
    CHECK(navigation_calls==1 && center.visible && center.failed && covered_page.screen==screen);
    navigation_accept=true;tap(input,center.settings);ui_control_center_process();
    CHECK(navigation_calls==2 && !center.visible && !covered_page.screen && page_clicks==1);
    render(display);CHECK(page_draws==31);
    tap(input,page);CHECK(page_clicks==2);

    /* Preserve an existing local property, and do not accumulate delete hooks. */
    lv_obj_set_style_opa_layered(screen,LV_OPA_70,0);
    for(unsigned i=0;i<1000;++i) {
        ui_control_center_open((i&1)!=0);
        CHECK(lv_obj_get_event_count(screen)==screen_events+1);
        ui_control_center_close();
        CHECK(lv_obj_get_event_count(screen)==screen_events);
        CHECK(lv_obj_get_local_style_prop(screen,LV_STYLE_OPA_LAYERED,&value,0)==LV_STYLE_RES_FOUND && value.num==LV_OPA_70);
    }
    lv_obj_remove_local_style_prop(screen,LV_STYLE_OPA_LAYERED,0);
    ui_control_center_open(false);
    lv_obj_t *replacement=lv_obj_create(NULL);lv_screen_load(replacement);
    ui_control_center_process();
    CHECK(covered_page.screen==replacement && lv_obj_get_style_opa_layered(screen,0)==LV_OPA_COVER);
    CHECK(lv_obj_get_event_count(screen)==screen_events);
    lv_obj_delete(screen);CHECK(covered_page.screen==replacement);
    /* Deleting the covered screen before the next UI turn clears the borrowed
     * pointer. Reopening and closing then operate on the replacement only. */
    screen=lv_obj_create(NULL);lv_screen_load(screen);lv_obj_delete(replacement);
    CHECK(!covered_page.screen);
    ui_control_center_process();CHECK(covered_page.screen==screen);
    locked=true;ui_control_center_process();
    CHECK(!center.visible && !covered_page.screen && lv_obj_get_style_opa_layered(screen,0)==LV_OPA_COVER);
    locked=false;
    lv_obj_add_flag(screen,LV_OBJ_FLAG_HIDDEN);
    ui_control_center_open(false);ui_control_center_close();
    CHECK(lv_obj_has_flag(screen,LV_OBJ_FLAG_HIDDEN));
    replacement=lv_obj_create(NULL);lv_screen_load(replacement);lv_obj_delete(screen);
    lv_indev_delete(input);
    CHECK(!filesystem_opens);
    puts("PASS center interaction/lifecycle: live state, pointer hit testing, settings rejection/acceptance, exact style restore, 1000 hook cycles, screen switch/delete and lock");
}
#include "render_equivalence.inc"

int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    lv_init(); lv_tick_set_cb(ticks); lv_display_t *display=lv_display_create(684,1216); font=*LV_FONT_DEFAULT;
    if(argc>1)load_render_font(argv[1]);
    lv_fs_drv_t disk;lv_fs_drv_init(&disk);disk.letter='S';disk.open_cb=forbidden_open;lv_fs_drv_register(&disk);
    ui_control_center_init(); CHECK(groups==2 && !ui_control_center_visible());
    if(ttf_data) {
        if(argc>3) {
            compare_trace=!strcmp(argv[2],"compare");
            CHECK(compare_trace || !strcmp(argv[2],"record"));
            pixel_trace=fopen(argv[3],compare_trace?"rb":"wb");CHECK(pixel_trace);
        }
        lv_display_set_flush_cb(display,flush);
        background_equivalence_test(display);
        if(pixel_trace) {
            if(compare_trace)CHECK(fgetc(pixel_trace)==EOF);
            CHECK(fclose(pixel_trace)==0);
        }
        return 0;
    }
    CHECK(lv_obj_has_flag(views[0].root,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(views[1].root,LV_OBJ_FLAG_HIDDEN));
    unsigned objects=object_count(lv_layer_top()), creates=panels+labels+icons, fonts=font_creates;
    ui_control_center_init(); CHECK(groups==2 && font_creates==fonts);
    populate(128);
    for(unsigned i=0;i<1000;++i) {
        unsigned reads=sound_reads;
        ui_control_center_open((i&1)!=0); CHECK(ui_control_center_visible()); CHECK(sound_reads==reads);
        CHECK(center.rows[0].populated && !strcmp(lv_label_get_text(center.rows[0].text),"Message 0"));
        CHECK(lv_image_get_src(center.rows[0].icon)==&ui_icon_quick);
        ui_control_center_process();
        CHECK(object_count(lv_layer_top())==objects && panels+labels+icons==creates && font_creates==fonts);
        ui_control_center_close(); CHECK(!ui_control_center_visible());
    }
    CHECK(!writes && !font_destroys);
    /* Catalog metadata and application text have independent lifetimes. A
     * packaged icon must never leave its borrowed descriptor in a widget. */
    populate(1);ui_control_center_open(true);
    CHECK(lv_label_get_text(center.rows[0].name)!=catalog[0].name);
    CHECK(lv_label_get_text(center.rows[0].text)!=notices[0].text);
    CHECK(lv_obj_get_event_count(center.rows[0].panel)==1);
    lv_event_dsc_t *event=lv_obj_get_event_dsc(center.rows[0].panel,0);
    CHECK(lv_event_dsc_get_cb(event)==notice_event && lv_event_dsc_get_user_data(event)==&center.rows[0]);
    ui_control_center_close();
    /* Poison the package descriptor, replace the catalog allocation, and
     * update the service snapshot while both cached roots are hidden. */
    populate(1);catalog[0].icon=(const void *)(uintptr_t)1;
    strcpy(catalog[0].name,"Reloaded app");strcpy(notices[0].text,"After module unload");
    ui_control_center_open(true);CHECK(lv_image_get_src(center.rows[0].icon)==&ui_icon_quick);
    CHECK(!strcmp(lv_label_get_text(center.rows[0].name),"Reloaded app"));
    CHECK(!strcmp(lv_label_get_text(center.rows[0].text),"After module unload"));
    CHECK(lv_event_dsc_get_cb(event)==notice_event && lv_event_dsc_get_user_data(event)==&center.rows[0]);
    ui_control_center_close();populate(128);
    ui_control_center_open(true); CHECK(lv_obj_get_child_count(center.dots)==NOTICE_DOTS);
    for(unsigned i=0;i<40;++i){center.page_delta=1;ui_control_center_process();}
    CHECK(center.page>0 && center.page*center.page_rows<notice_count);
    populate(1); ui_control_center_process(); CHECK(center.page==0 && center.rows[0].populated && !center.rows[1].populated);
    strcpy(notices[0].text,"Updated in place"); ++notice_revision; ui_control_center_process();
    CHECK(!strcmp(lv_label_get_text(center.rows[0].text),"Updated in place"));
    populate(0); ui_control_center_process(); CHECK(!lv_obj_has_flag(center.empty,LV_OBJ_FLAG_HIDDEN));
    center.action=ACTION_CONTROLS; ui_control_center_process(); CHECK(selected==&views[0] && ui_control_center_visible());
    sound_ready=false; ui_control_center_process(); CHECK(lv_obj_has_state(center.volume,LV_STATE_DISABLED));
    idle=false; sound_ready=true; sound_max=8; ui_control_center_process(); CHECK(center.layout_volume_max==center.volume_max);
    idle=true; ui_control_center_process(); CHECK(center.layout_volume_max==8);
    CHECK(lv_obj_has_flag(lv_obj_get_child(lv_obj_get_child(center.volume,0),8),LV_OBJ_FLAG_HIDDEN));
    sound_save=EPD_SOUND_SAVE_PENDING;ui_control_center_process();CHECK(!lv_label_get_text(center.error)[0]);
    sound_save=EPD_SOUND_SAVE_FAILED;ui_control_center_process();CHECK(!strcmp(lv_label_get_text(center.error),"Failed"));
    sound_save=EPD_SOUND_SAVE_OK;ui_control_center_process();CHECK(!lv_label_get_text(center.error)[0]);
    light_save_failed=true;ui_control_center_process();CHECK(!strcmp(lv_label_get_text(center.error),"Failed"));
    light_save_failed=false;ui_control_center_process();CHECK(!lv_label_get_text(center.error)[0]);
    ui_control_center_close();media_present=true;++media_generation;
    strcpy(published_media.app_id,"app0");strcpy(published_media.artist,"Reloaded artist");strcpy(published_media.title,"New session");
    ui_control_center_open(false);ui_control_center_process();
    CHECK(!strcmp(lv_label_get_text(center.title),"New session") && !strcmp(lv_label_get_text(center.artist),"Reloaded artist"));
    CHECK(lv_label_get_text(center.title)!=published_media.title);
    media_present=false;memset(&published_media,0,sizeof(published_media));++media_generation;
    ui_control_center_process();CHECK(!strcmp(lv_label_get_text(center.title),"No media"));
    ui_control_center_close(); idle=false; scale=120; ++scale_revision; ++locale_revision;
    ui_control_center_process(); CHECK(font_creates==fonts);
    idle=true; ui_control_center_process(); CHECK(font_creates==fonts+4 && font_destroys==4);
    CHECK(views[0].scale_revision==scale_revision && views[1].scale_revision==scale_revision);
    CHECK(!strcmp(lv_label_get_text(views[1].tab_labels[0]),"Messages"));
    populate(3); storage_busy=true; ui_control_center_storage_changed(); CHECK(reconcile_count==0);
    ui_control_center_open(false); center.action=ACTION_WIFI; center.light_request=3; center.volume_request=4;
    ui_control_center_close(); ui_control_center_process(); CHECK(!writes);
    locked=true; ui_control_center_open(true); CHECK(!ui_control_center_visible());
    CHECK(object_count(lv_layer_top())==objects && panels+labels+icons==creates && !filesystem_opens);
    printf("PASS control center: 1000 cached opens, %u objects, bounded pages/tracks, unload-safe copies, latest snapshots, no disk opens, idle maintenance, storage and lock\n",objects);
    rendering_test(display);
    background_equivalence_test(display);
    return 0;
}
