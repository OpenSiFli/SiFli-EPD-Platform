#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include "ids.inc"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define LV_MIN(a,b) ((a)<(b)?(a):(b))
#define LV_MAX(a,b) ((a)>(b)?(a):(b))
#define MARGIN 32
#define WIDTH 684
#define CONTENT_WIDTH 620
#define SETTINGS_TITLE_X 50
#define SETTINGS_TITLE_Y 100
#define PAGE_TITLE_X 50
#define PAGE_TITLE_Y 100
#define DEVICE_IDENTITY_NAME_CAPACITY 64
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sdcard"
#define APP_SAP_PATH_MAX 512
#define GUI_APP_ID_MAX_LEN 32
#define FILE_PAGE_ROWS 8
#define PORTAL_WIFI_QR_CAPACITY 224
#define RT_EOK 0
#define RT_EIO 5
#define NAV_IDLE 0
#define LV_EXT_STR_ID(x) #x
#define LV_EXT_STR_GET(x) (x)
#define epd_get_str(x) #x
#define epd_ui_px(x) (x)
#define epd_ui_touch(x) (x)
#define rt_snprintf snprintf
#define EPD_FMT_platform_used_total(o,n,a,b) snprintf(o,n,"%s/%s",a,b)
#define EPD_FMT_platform_allocatable(o,n,a) snprintf(o,n,"free %s",a)
#define EPD_FMT_platform_unassigned(o,n,a) snprintf(o,n,"unassigned %s",a)
#define EPD_FMT_platform_app_location(o,n,a,b) snprintf(o,n,"%s %s",a,b)
#define EPD_FMT_portal_clients(o,n,a) snprintf(o,n,"clients %u",a)
enum {LV_LABEL_LONG_WRAP, LV_TEXT_ALIGN_CENTER, LV_OBJ_FLAG_SCROLLABLE=1, LV_OBJ_FLAG_HIDDEN=2,
      LV_STATE_DISABLED=4, LV_DIR_VER, LV_SIZE_CONTENT, LV_RADIUS_CIRCLE, LV_RESULT_OK=0};
typedef int rt_err_t;
typedef int ui_page_id_t;
typedef int system_reset_target_t;
typedef int system_info_t;
typedef const char *lv_ext_str_id_t;
typedef struct {int line_height;} lv_font_t;
typedef struct object {struct object *child; bool hidden; void *group; char text[512];} lv_obj_t;
typedef struct {lv_obj_t *focused;} lv_group_t;
typedef void lv_timer_t;
typedef void file_page_cache_t;
typedef struct {char id[32], name[64];} app_content_handler_t;
typedef void (*files_picker_callback_t)(void);
typedef struct {size_t total, used, available, unassigned;} app_ram_usage_t;
typedef struct {app_ram_usage_t ram[2];} app_memory_snapshot_t;
typedef enum {STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT} storage_volume_t;
typedef struct {uint64_t total, used;} storage_info_t;
typedef struct {int timezone_minutes; bool automatic, hour24;} system_time_settings_t;
typedef struct {unsigned option_count;} ui_setting_t;
typedef struct {bool ready; int source;} network_snapshot_t;
enum {NETWORK_WIFI, FRONTLIGHT_COOL, FRONTLIGHT_WARM, FRONTLIGHT_MIXED};
typedef int frontlight_mode_t;
typedef struct {bool output_valid, available, mode_available, preference_valid, locked, suspended; unsigned output_percent; int mode;} frontlight_snapshot_t;
typedef struct {bool available; char name[64];} reading_font_info_t;
typedef struct {bool supported, error, busy, ready, enabled, connected;} usb_card_reader_status_t;
typedef struct {char id[32], name[64]; storage_volume_t volume; bool has_settings;} epd_app_entry_t;
typedef int app_job_message_t;
typedef struct {unsigned ticket; int state, message;} app_job_result_t;
enum {APP_JOB_RUNNING, APP_JOB_DONE, APP_JOB_FAILED, APP_PACKAGE_OK};
enum {PORTAL_OFF, PORTAL_STARTING, PORTAL_ON, PORTAL_STOPPING, PORTAL_ERROR};
typedef struct {int phase; unsigned revision, clients; bool lan, hotspot_requested, lan_available; char ssid[33], password[17], url[96];} portal_snapshot_t;
typedef struct {bool visible;} ui_portal_accounts_t;
enum {UI_SETTINGS_ROW_LINK, UI_SETTINGS_ROW_INFO, UI_SETTINGS_ROW_SWITCH, UI_SETTINGS_ROW_CHOICE};
typedef struct {int key, kind; const char *title, *value; const void *icon; bool enabled, checked, no_chevron; int min_height, icon_size, bottom_gap;} ui_settings_list_row_t;
typedef struct {size_t count; ui_settings_list_row_t rows[128]; char titles[128][80], values[128][512]; lv_group_t *group; int shown;} ui_settings_list_t;
typedef struct {int id; void *feature; lv_obj_t *screen, *popup, *popup_text, *popup_detail; lv_group_t *group; bool foreground, locale_retry; unsigned popup_serial; uint32_t scale_revision, locale_revision;} context_t;
static context_t context, *current=&context;
static int pending_page;
static unsigned fills, snapshots, scans, qr_updates, show_calls, generation=1, card_session=7;
static unsigned option_index=35;
static int timezone=487, alloc_fail=-1;
static bool rows_fail, show_fail, unavailable, changing, releasing, snapshot_fail, qr_fail;
static portal_snapshot_t portal_state;
static lv_font_t font={24};
static lv_obj_t objects[256];
static unsigned object_count;
static lv_obj_t *new_object(void) {CHECK(object_count<256); return &objects[object_count++];}
static void *host_alloc(size_t n) {if (alloc_fail==0) return NULL; if (alloc_fail>0) --alloc_fail; return calloc(1,n);}
#define lv_malloc host_alloc
#define lv_malloc_zeroed host_alloc
#define lv_free free
static void *lv_realloc(void *p,size_t n) {if (alloc_fail==0) return NULL; if(alloc_fail>0)--alloc_fail; return realloc(p,n);}
#define ui_font_title() (&font)
#define ui_font_body() (&font)
#define ui_font_small() (&font)
#define ui_font_caption() (&font)
#define ui_font_settings_title() (&font)
#define ui_font_settings_body() (&font)
#define ui_font_settings_value() (&font)
#define lv_obj_create(p) new_object()
#define lv_qrcode_create(p) new_object()
#define epd_obj_init(...) ((void)0)
#define lv_obj_set_size(...) ((void)0)
#define lv_obj_set_pos(...) ((void)0)
#define lv_obj_set_width(...) ((void)0)
#define lv_obj_set_height(...) ((void)0)
#define lv_obj_set_style_text_font(...) ((void)0)
#define lv_obj_set_style_text_align(...) ((void)0)
#define lv_obj_set_style_text_color(...) ((void)0)
#define lv_obj_set_style_border_width(...) ((void)0)
#define lv_obj_set_style_bg_color(...) ((void)0)
#define lv_obj_set_style_radius(...) ((void)0)
#define lv_obj_update_layout(...) ((void)0)
#define lv_obj_set_scroll_dir(...) ((void)0)
#define lv_obj_add_flag(...) ((void)0)
#define lv_obj_add_state(...) ((void)0)
#define lv_obj_remove_state(...) ((void)0)
#define lv_label_set_long_mode(...) ((void)0)
#define lv_obj_center(...) ((void)0)
#define lv_qrcode_set_dark_color(...) ((void)0)
#define lv_qrcode_set_light_color(...) ((void)0)
#define lv_qrcode_set_quiet_zone(...) ((void)0)
#define lv_qrcode_set_size(...) ((void)0)
#define lv_obj_get_width(o) 684
#define lv_obj_get_height(o) 1000
#define lv_obj_get_style_text_font(o,s) (&font)
#define ui_shell_content_bottom() 1000
#define epd_locale_revision() 1u
#define epd_ui_scale_revision() 1u
#define lv_tick_get() 1u
#define lv_refreshing_done() true
#define ui_shell_modal_visible() false
static lv_obj_t *lv_obj_get_child(lv_obj_t *o,int i) {(void)i; CHECK(o); if(!o->child)o->child=new_object(); return o->child;}
static void text_update(lv_obj_t *o,const char *s) {CHECK(o); snprintf(o->text,sizeof(o->text),"%s",s?s:"");}
static lv_obj_t *label_create(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *f) {(void)p;(void)x;(void)y;(void)w;(void)f;lv_obj_t *o=new_object();text_update(o,s);return o;}
#define centered_label label_create
#define centered_label_i18n label_create
#define page_title_id(id) "title"
#define page_title_create(s,id,f) label_create(s,id,0,0,1,f)
static lv_obj_t *button_create(lv_obj_t *p,int x,int y,int w,int h,const char *s,int key) {(void)h;(void)key;lv_obj_t *o=new_object();o->child=label_create(p,s,x,y,w,&font);return o;}
#define button_create_i18n button_create
static bool lv_obj_has_flag(lv_obj_t *o,int f) {(void)f;return o&&o->hidden;}
static void object_visible(lv_obj_t *o,bool visible) {if(o)o->hidden=!visible;}
static void lv_group_add_obj(lv_group_t *g,lv_obj_t *o) {o->group=g;if(!g->focused)g->focused=o;}
static void lv_group_remove_obj(lv_obj_t *o) {if(o)o->group=NULL;}
static void *lv_obj_get_group(lv_obj_t *o) {return o->group;}
static ui_settings_list_t *ui_settings_list_create(lv_obj_t *p,const lv_font_t *a,const lv_font_t *b,void (*cb)(int,void *),void *ctx) {(void)p;(void)a;(void)b;(void)cb;(void)ctx;return host_alloc(sizeof(ui_settings_list_t));}
static void ui_settings_list_destroy(ui_settings_list_t *l) {free(l);}
static void ui_settings_list_set_group(ui_settings_list_t *l,lv_group_t *g) {if(l)l->group=g;}
static bool ui_settings_list_refresh_fonts(ui_settings_list_t *l,const lv_font_t *a,const lv_font_t *b) {(void)l;(void)a;(void)b;return true;}
static bool ui_settings_list_update_layout(ui_settings_list_t *l) {(void)l;return true;}
static bool ui_settings_list_set_rows(ui_settings_list_t *l,const ui_settings_list_row_t *r,size_t n) {
    ++fills; if(rows_fail)return false;CHECK(l&&n<=128);l->count=n;
    for(size_t i=0;i<n;++i){l->rows[i]=r[i];snprintf(l->titles[i],80,"%s",r[i].title?r[i].title:"");snprintf(l->values[i],512,"%s",r[i].value?r[i].value:"");l->rows[i].title=l->titles[i];l->rows[i].value=l->values[i];}
    return true;
}
static bool ui_settings_list_show_key(ui_settings_list_t *l,int key) {++show_calls;if(show_fail)return false;for(size_t i=0;i<l->count;++i)if(l->rows[i].key==key){l->shown=key;return true;}return false;}
static bool ui_settings_list_set_initial_rows(ui_settings_list_t *l,const ui_settings_list_row_t *r,size_t n,int key) {return ui_settings_list_set_rows(l,r,n)&&ui_settings_list_show_key(l,key);}
static bool firmware_update_page(const void *view) {(void)view;return false;}
static void firmware_update_clear(void *view) {(void)view;}
static void epd_file_picker_cancel(void *owner) {(void)owner;}
static bool storage_changing(void) {return changing;}
static bool storage_release_requested(void) {return releasing;}
static bool storage_available(int v) {(void)v;return !unavailable;}
static bool storage_info(int v,storage_info_t *info) {(void)v;++snapshots;*info=(storage_info_t){1000,generation};return !unavailable;}
static void storage_format_size(uint64_t n,char *out,size_t size) {snprintf(out,size,"%llu",(unsigned long long)n);}
static void storage_lock(void) {}
static void storage_unlock(void) {}
static uint32_t storage_card_session(void) {return card_session;}
static bool storage_session_valid(int v,uint32_t s) {(void)v;return !unavailable&&s==card_session;}
static const char *storage_path_root(const char *p) {(void)p;return STORAGE_SD_ROOT;}
static bool storage_apps_enabled(int v) {(void)v;return true;}
static const char *storage_app_directory(int v) {return v==STORAGE_FLASH?"/flash/apps":"/sdcard/apps";}
static bool storage_path_available(const char *p) {(void)p;return !unavailable;}
static bool storage_app_locate(const char *id,storage_volume_t *v) {(void)id;*v=STORAGE_FLASH;return true;}
static bool app_catalog_refresh(void) {return true;}
static uint32_t app_catalog_revision(void) {return generation;}
static bool app_installer_busy(void) {return false;}
static bool app_installer_volume_ready(int v) {return v==STORAGE_FLASH&&!unavailable;}
static const char *app_installer_message(int v) {(void)v;return "message";}
static void app_installer_result(app_job_result_t *r) {*r=(app_job_result_t){0};}
static bool app_package_read_display(const char *p,const char *id,epd_app_entry_t *e) {(void)p;snprintf(e->id,sizeof(e->id),"%s",id);snprintf(e->name,sizeof(e->name),"app %u",generation);e->volume=STORAGE_FLASH;e->has_settings=true;return true;}
static int app_sap_metadata(const char *p,epd_app_entry_t *e) {app_package_read_display(p,"demo",e);return APP_PACKAGE_OK;}
static const char *app_content_error(int e) {(void)e;return "error";}
static int app_content_query(const char *p,unsigned first,app_content_handler_t *h,unsigned n,unsigned *total) {(void)p;(void)first;(void)h;(void)n;*total=0;return RT_EOK;}
static void popup_close(void) {current->popup=NULL;}
static void popup_open(const char *a,const char *b) {(void)a;(void)b;current->popup=new_object();}
static void system_time_get_settings(system_time_settings_t *s) {*s=(system_time_settings_t){timezone,false,true};}
static int system_time_get_utc(void) {return 0;}
#define epd_locale_get() 0
static void system_time_format(int t,int l,bool b,char *s,size_t n) {(void)t;(void)l;(void)b;snprintf(s,n,"time");}
static const ui_setting_t *ui_settings_item(int id) {(void)id;static ui_setting_t s={60};return &s;}
static const char *ui_settings_label(int id) {(void)id;return "option";}
static const char *ui_settings_value(int id) {(void)id;return "value";}
static unsigned ui_settings_index(int id) {(void)id;return option_index;}
static const char *ui_settings_option(int id,unsigned i) {(void)id;(void)i;return "choice";}
static bool settings_wireless_handles(int id) {(void)id;return false;}
static void settings_wireless_resume(void) {}
static void settings_wireless_restore_focus(void) {}
static void settings_wireless_stop(void) {}
static void network_snapshot(network_snapshot_t *s) {*s=(network_snapshot_t){true,NETWORK_WIFI};}
static void frontlight_get(frontlight_snapshot_t *s) {*s=(frontlight_snapshot_t){0};}
static int reading_font_selected(void) {return 0;}
static bool reading_font_get(int id,reading_font_info_t *f) {(void)id;memset(f,0,sizeof(*f));return false;}
static void usb_card_reader_status(usb_card_reader_status_t *s) {memset(s,0,sizeof(*s));}
static void device_identity_get_name(char *s,size_t n) {snprintf(s,n,"device");}
static bool app_memory_snapshot(app_memory_snapshot_t *s) {++snapshots;for(unsigned i=0;i<2;++i)s->ram[i]=(app_ram_usage_t){1000,generation,900,0};return !snapshot_fail;}
static void portal_snapshot(portal_snapshot_t *s) {++snapshots;*s=portal_state;}
static bool ui_portal_wifi_qr(char *s,size_t n,const char *ssid,const char *password) {snprintf(s,n,"WIFI:%s:%s",ssid,password);return true;}
static int lv_qrcode_update(lv_obj_t *o,const char *s,uint32_t n) {(void)n;++qr_updates;if(qr_fail)return 1;text_update(o,s);return LV_RESULT_OK;}
static ui_portal_accounts_t accounts;
static ui_portal_accounts_t *ui_portal_accounts_create(lv_obj_t *s) {(void)s;return &accounts;}
static bool ui_portal_accounts_visible(ui_portal_accounts_t *a) {return a&&a->visible;}
static void ui_portal_accounts_process(ui_portal_accounts_t *a,bool layout) {(void)a;(void)layout;}
static void ui_portal_accounts_destroy(ui_portal_accounts_t *a) {(void)a;}
static int ui_icon_wifi_entry,ui_icon_settings_display,ui_icon_settings_lock,ui_icon_volume,ui_icon_settings_apps,
    ui_icon_settings_more,ui_icon_settings_update,ui_icon_settings_manual,ui_icon_settings_about,ui_icon_bt_entry,ui_icon_usb,ui_icon_settings_network;
enum {DT_DIR=1, DT_REG=2};
struct dirent {char d_name[64]; int d_type;};
struct stat {int st_mode;};
typedef struct {unsigned offset;} DIR;
static DIR directory;
static DIR *opendir(const char *p) {(void)p;++scans;directory.offset=0;return unavailable?NULL:&directory;}
static struct dirent *readdir(DIR *d) {static struct dirent e={"demo.sap",DT_REG};return d->offset++?NULL:&e;}
static void closedir(DIR *d) {(void)d;}
static int stat(const char *p,struct stat *s) {(void)p;s->st_mode=DT_REG;return 0;}
#define S_ISDIR(s) ((s)==DT_DIR)
#define S_ISREG(s) ((s)==DT_REG)
