#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "ui_settings.h"
#define LV_FONT_KERNING_NONE 0
#define VIEW_LAYOUT_VERSION 2u
#define READER_ERROR_NONE 0
#define READER_ERROR_FONT_MEMORY 1
typedef struct { unsigned family,line_height,kerning,slot; } lv_font_t;
typedef struct { int width,top,bottom,height; } reader_geometry_t;
typedef struct { uint32_t record,byte; } document_cursor_t;
typedef struct { document_cursor_t start; } page_t;
typedef struct {
 const lv_font_t *font;unsigned margin,pages,error_code;int content_width,top,bottom,line_space;bool ready,indexing;
} reader_view_t;
typedef struct view_state {
 unsigned sparse_stride,sparse_pages;bool sparse_seek,toc_limited,count_loaded,count_saved,numeric_target;
 struct view_state *next;void *document;lv_font_t *fonts[3],*oblique[3],*fallbacks[3],*retired[6],*retired_oblique[3];
 unsigned font_id,font_revision,font_identity,failed_font_id,failed_font_revision,settings[5],failed_font_settings[5],layout,appearance;
 reader_geometry_t font_area,failed_font_area;
 bool font_failed,font_metrics_ready,frame_published,complete,index_loaded,index_loading,index_saved,from_cache,index_save_failed,display_pending,notified;
 reader_view_t status;
 unsigned cache_blocked,prefetch_number,cache,count,toc_count,toc_scanned,index_saved_count,failure;
 document_cursor_t anchor;page_t *display,*index;char link_target[32];
 uint32_t navigation_link,navigation_toc;
 unsigned chapter_percent;
 void *progress_request;
} view_state_t;
static view_state_t *state;
#define view (*state)
static void document_navigation_release(void *d) { (void)d; }
static void document_record_cancel(void *request) { (void)request; }
static int document_target_require(void *d,bool required) { (void)d;assert(!required);return 1; }
#define VIEW_HEIGHT (view.status.bottom-view.status.top)
static struct { view_state_t *session;unsigned font_id,font_revision,settings[5],appearance;reader_geometry_t area;bool configuration_failed,appearance_pending; } ui;
static struct { view_state_t *retired; } mailbox;
static unsigned requested_id=1,requested_revision=1,settings[5],live,calls,failures,fail_slot;
static bool fail_italic;
static reader_geometry_t area={684,54,1168,1216};
static unsigned locks,error;
static unsigned epd_app_reading_font_id(void) { return requested_id; }
static unsigned epd_app_reading_font_revision(void) { return requested_revision; }
unsigned ui_settings_index(ui_setting_id_t id)
{
 if (id == UI_SETTING_READER_APPEARANCE) return 0;
 assert(id >= UI_SETTING_FONT_SIZE && id <= UI_SETTING_ENCODING);
 return settings[id - UI_SETTING_FONT_SIZE];
}
static reader_geometry_t reader_geometry(void) { return area; }
static lv_font_t *new_font(unsigned id,unsigned size,unsigned slot) { lv_font_t *p=calloc(1,sizeof(*p));assert(p);p->family=id;p->line_height=size;p->slot=slot;++live;return p; }
static lv_font_t *epd_app_reading_font_create(unsigned id,unsigned size,unsigned weight,uint32_t *identity,lv_font_t **fallback)
{
 static const unsigned sizes[]={UI_READING_FONT_SIZES};
 unsigned slot=size>sizes[settings[0]]?2:weight?1:0;++calls;*fallback=NULL;
 if(!fail_italic && slot==fail_slot && failures){--failures;return NULL;}
 *identity=id*1000+size*10+weight;return new_font(id,size,slot);
}
static lv_font_t *document_font_oblique(const lv_font_t *font)
{ if(fail_italic && font->slot==fail_slot && failures){--failures;return NULL;}return new_font(font->family,font->line_height,font->slot); }
static void epd_app_font_destroy(lv_font_t *font) { if(font){assert(live);--live;free(font);} }
static void document_font_free(lv_font_t *font) { epd_app_font_destroy(font); }
#define rt_kprintf(...) ((void)0)
static void reader_page_cache_clear(unsigned *c) { *c=0; }
static void frame_cache_clear(void) {}
static void visited_clear(view_state_t *s) { (void)s; }
static void compute_cancel(void) {}
static void index_save(bool force) { assert(force); }
static void page_reset(page_t *p,document_cursor_t cursor) { p->start=cursor; }
static void preview_start(document_cursor_t cursor) { view.anchor=cursor; }
static void images_cancel_pending(void) {}
static void images_prune(bool optional) { assert(optional); }
static void trace_start(void) {}
static void rt_enter_critical(void) { ++locks; }
static void rt_exit_critical(void) { assert(locks);--locks; }
static void epd_reader_set_error_code(unsigned value) { error=value; }
/* PRODUCTION */
static void destroy(view_state_t *s)
{
 for(unsigned i=0;i<3;++i) {document_font_free(s->oblique[i]);document_font_free(s->retired_oblique[i]);epd_app_font_destroy(s->fonts[i]);epd_app_font_destroy(s->fallbacks[i]);}
 for(unsigned i=0;i<6;++i)epd_app_font_destroy(s->retired[i]);
}
int main(void)
{
 for(unsigned format=1;format<=2;++format) {
  for(unsigned italic=0;italic<2;++italic) for(unsigned slot=0;slot<3;++slot) {
   view_state_t active={0},configuration={0},rejected={0};page_t display={{4567,11}},index={{0,0}};
   state=&active;requested_id=1;requested_revision=1;settings[0]=0;failures=0;
   active.document=(void*)(uintptr_t)format;active.display=&display;active.index=&index;
   assert(prepare_fonts(&active) && live==6);active.status.ready=true;
   unsigned old_layout=active.layout;lv_font_t *old_font=active.fonts[0];
   ui.session=&active;ui_configuration(&active);assert(!epd_reader_settings_changed());
   requested_id=2;settings[0]=1;assert(epd_reader_settings_changed());
   fail_italic=italic!=0;fail_slot=slot;failures=1;
   assert(prepare_fonts(&configuration) && live==12 && active.fonts[0]==old_font && active.font_id==1);
   unsigned new_identity=configuration.font_identity;assert(new_identity!=active.font_identity);
   ui_configuration(&configuration);state_reflow(&configuration);
   assert(active.document==(void*)(uintptr_t)format && active.anchor.record==4567 && active.anchor.byte==11);
   assert(active.font_id==2 && active.font_identity==new_identity && active.layout!=old_layout);
   assert(configuration.fonts[0]==old_font && mailbox.retired==&configuration);
   assert(!epd_reader_settings_changed() && !active.font_metrics_ready && active.status.indexing);
   destroy(&configuration);mailbox.retired=NULL;assert(live==6);
   requested_id=3;failures=2;unsigned before=calls;
   assert(!prepare_fonts(&rejected) && failures==0 && calls-before<=6 && live==6 && active.font_id==2);
   ui_configuration_rejected();assert(ui.configuration_failed && error==READER_ERROR_FONT_MEMORY);
   for(unsigned i=0;i<100;++i)assert(!epd_reader_settings_changed());
   ++requested_revision;assert(epd_reader_settings_changed());
   failures=0;view_state_t retry={0};assert(prepare_fonts(&retry));ui_configuration(&retry);state_reflow(&retry);
   assert(active.font_id==3 && active.anchor.record==4567 && active.anchor.byte==11 && !epd_reader_settings_changed());
   destroy(&retry);destroy(&active);assert(!live && !locks);mailbox.retired=NULL;
  }
 }
 puts("PASS TXT/EPUB shared global font: all 3 normal/italic failure slots, bounded retry, retained visible fonts, revision changes and reflow anchor preservation");return 0;
}
