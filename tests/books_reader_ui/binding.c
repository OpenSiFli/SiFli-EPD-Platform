#include "reader_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define rt_kprintf(...) do { if(0)printf(__VA_ARGS__); } while(0)
#define EPD_APP_PSRAM 0
#define EPD_FONT_BODY 0
#define BOOK_STYLE_NOTEREF 1
#define UI_SETTING_FONT_SIZE 0
#define UI_SETTING_ENCODING 4
typedef unsigned rt_tick_t;
typedef void document_t;
typedef struct { unsigned record,byte; } document_cursor_t;
typedef struct { unsigned unit,chapter,offset,skip_utf8; } document_anchor_t;
typedef struct { unsigned source_total,chapters,chapter; } document_progress_t;
typedef struct { int width,top,bottom; } reader_geometry_t;
typedef struct { unsigned link,style,image,bytes;int x,y,width,height; } view_run_t;
typedef struct { document_cursor_t start; unsigned count; view_run_t runs[1]; } page_t;
typedef struct view_state {
    unsigned session,intent,references,layout,font_id,font_revision,settings[5],wanted;
    unsigned published_intent,published_generation;
    bool frame_published,selected;
    reader_geometry_t font_area;
    reader_view_t status;
    document_progress_t progress;
    document_t *document;
} view_state_t;
typedef struct view_frame {
    struct view_frame *next,*backing;
    view_state_t *owner;
    unsigned references,layout,intent,generation,requested_at,ready_at;
    unsigned window_charge,completed_at;
    bool complete;
    void *renderer,*pins;
    lv_image_dsc_t image;
    reader_view_t info;
    struct { document_cursor_t cursor;document_anchor_t source;unsigned page,pages,percent; } position;
    page_t page;
    char title[64];
} view_frame_t;
typedef struct view_widget { struct view_widget *next;lv_obj_t *object;unsigned link;bool note; } view_widget_t;
enum { INTENT_NONE,INTENT_SEEK,INTENT_TURN,INTENT_CURSOR,INTENT_CANCEL,INTENT_TOC,INTENT_CHAPTER,INTENT_LINK,INTENT_REFLOW };
typedef int view_intent_t;
typedef struct { int kind,value;document_cursor_t cursor;unsigned session,serial;view_frame_t *origin; } view_command_t;
static struct {
    view_state_t *session;
    view_frame_t *bound,*ready;
    lv_obj_t *root,*bitmap;
    view_widget_t *widgets;
    unsigned id,intent,requested_at,font_id,font_revision,settings[5];
    reader_geometry_t area;
    int turn_delta;
    bool paused,pending,configuration_failed;
    reader_view_t info;
    char title[64];
} ui;
static struct {
    view_state_t *configuration,*incoming;
    view_frame_t *ready,*ack;
    view_command_t command;
    unsigned intent,session,requested_at,result_intent,pages,page,percent,layout;
    bool indexing,replace;
    reader_work_t work;
    reader_error_t error;
    document_t *document;
    document_cursor_t cursor;
} mailbox;
static struct {
    view_state_t *context;
    view_frame_t *origin;
    view_command_t command;
    uint32_t session;
    bool opening;
} font_request;
static view_state_t *state;
#define view (*state)
static char visible_chapter[64];
static unsigned checks,locks,wakes,saves,cancelled,requested_font=1,requested_size=28,requested_line=6;
static bool settings_changed,font_capacity=true,font_prepare=true;
static int font_begin=1;
static unsigned font_prepares,disposed;
static void rt_enter_critical(void){++locks;}
static void rt_exit_critical(void){CHECK(locks);--locks;}
static unsigned rt_tick_get(void){return lv_tick_get();}
static unsigned duration_ms(unsigned ticks){return ticks;}
static unsigned elapsed_ms(unsigned start){return lv_tick_get()-start;}
static void *epd_app_alloc(size_t n,int kind){(void)kind;return calloc(1,n);}
static void epd_app_free(void *p){free(p);}
static void state_dispose(view_state_t *s){++disposed;free(s);}
static const lv_font_t *epd_app_font_role(unsigned role){(void)role;return LV_FONT_DEFAULT;}
static void epd_app_object_init(lv_obj_t *o){lv_obj_remove_style_all(o);}
static void epd_reader_note_close(void){}
static bool epd_reader_settings_changed(void){return settings_changed;}
static bool epd_app_reading_font_can_create(unsigned count){CHECK(count==6);return font_capacity;}
static int epd_reader_font_begin(void){return font_begin;}
static void first_font_cancel(void){}
static unsigned epd_app_reading_font_id(void){return requested_font;}
static unsigned epd_app_reading_font_revision(void){return requested_font;}
static reader_geometry_t reader_geometry(void){return (reader_geometry_t){80,12,90};}
static unsigned ui_settings_index(unsigned i){return i==0?requested_size:i==2?requested_line:0;}
static void epd_reader_set_error_code(reader_error_t error){ui.info.error_code=error;}
static void ui_configuration_rejected(void){ui.configuration_failed=true;epd_reader_set_error_code(READER_ERROR_FONT_MEMORY);}
static bool prepare_fonts(view_state_t *s){
    ++font_prepares;if(!font_prepare)return false;
    s->font_id=requested_font;s->font_revision=requested_font;
    s->settings[0]=requested_size;s->settings[2]=requested_line;
    s->layout=requested_font*10000+requested_size*100+requested_line;
    s->status=(reader_view_t){.ready=true,.indexing=true,.margin=8,.top=12,.line_space=(int)requested_line};
    s->font_area=(reader_geometry_t){80,12,90};settings_changed=false;return true;
}
static void frame_ref(view_frame_t *f){if(f)++f->references;}
static void frame_release(view_frame_t *f){
    if(!f)return;CHECK(f->references);
    if(--f->references)return;
    if(f->backing){frame_release(f->backing);CHECK(f->owner->references);--f->owner->references;}
    free(f);
}
static void view_cancel(void){++cancelled;}
static void turn_cancel(void){}
static void wake_view(void){++wakes;}
static void epd_reader_ui_wake(void){++wakes;}
static void first_font_ready(uint32_t session,uint32_t intent,uint32_t font_id)
{ CHECK(!locks && session==view.session && intent==view.intent && font_id==view.font_id); }
static void fail(reader_error_t error){(void)error;CHECK(false);}
static bool epd_reader_save(void){++saves;return true;}
static bool epd_reader_suspended(void){return false;}
static void epd_reader_collect(void){}
static void pagination_snapshot(view_state_t *s,reader_view_t *info){(void)s;(void)info;}
static bool document_current_title(document_t *d,unsigned record,char *text,size_t n,unsigned *index){
    (void)d;(void)record;(void)index;snprintf(text,n,"chapter");return true;
}
static void ui_link_clicked(lv_event_t *e){(void)e;CHECK(false);}
#include "binding.inc"
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *pixels){(void)a;(void)pixels;lv_display_flush_ready(d);}
static view_frame_t *prepared(uint32_t *pixels){
    view_frame_t *f=calloc(1,sizeof(*f));CHECK(f);f->references=1;f->owner=state;
    f->layout=view.layout;f->generation=view.intent;f->intent=view.intent;
    f->complete=true;
    f->position.cursor=(document_cursor_t){7,13};f->position.source=(document_anchor_t){1,0,25,0};
    f->image.header.magic=LV_IMAGE_HEADER_MAGIC;f->image.header.cf=LV_COLOR_FORMAT_ARGB8888;
    f->image.header.w=2;f->image.header.h=2;f->image.header.stride=8;
    f->image.data=(const uint8_t *)pixels;f->image.data_size=16;strcpy(f->title,"book");return f;
}
int main(void){
    lv_init();lv_display_t *display=lv_display_create(80,100);
    static uint32_t screen_pixels[80*100],old_pixels[4]={0xffffffff,0xffffffff,0xffffffff,0xffffffff};
    static uint32_t new_pixels[3][4]={{0xff000000},{0xff555555},{0xff999999}};
    lv_display_set_buffers(display,screen_pixels,NULL,sizeof(screen_pixels),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *body=lv_obj_create(lv_screen_active());
    lv_obj_t *menu=lv_obj_create(lv_screen_active());lv_obj_set_pos(menu,0,50);lv_obj_set_size(menu,80,50);
    view_state_t owner={0};state=&owner;owner.session=ui.id=mailbox.session=1;
    owner.progress.source_total=100;owner.status=(reader_view_t){.ready=true,.margin=8,.top=12};
    ui.session=&owner;ui.paused=true;ui.info=owner.status;
    owner.layout=1;ui.intent=mailbox.intent=owner.intent=1;
    view_frame_t *cached=prepared(old_pixels);frame_publish(cached);frame_release(cached);
    epd_reader_step(8,true,true);CHECK(epd_reader_bind(body));
    CHECK(ui.bitmap && lv_image_get_src(ui.bitmap)==&ui.bound->image);
    CHECK(epd_reader_bound_image(body)==&ui.bound->image && !epd_reader_bound_image(menu));
    for(unsigned change=0;change<3;++change){
        if(change==0)++requested_font;else if(change==1)requested_size+=4;else requested_line+=6;
        view_frame_t *old=ui.bound;unsigned saved=saves;settings_changed=true;
        unsigned before=font_prepares;
        CHECK(epd_reader_reflow());CHECK(ui.pending && ui.bound==old && !mailbox.configuration);
        CHECK(font_prepares==before && font_request.context);
        font_capacity=false;
        CHECK(!font_request_process() && !ui.info.error_code && font_prepares==before);
        font_capacity=true;
        CHECK(font_request_process() && font_prepares==before+1 && !font_request.context);
        CHECK(mailbox.command.kind==INTENT_REFLOW && mailbox.command.origin==old);
        /* The worker's configuration and rendered bitmap are supplied at the
         * asynchronous boundary; request, publication, UI intake and bind are production code. */
        view_state_t *configuration=mailbox.configuration;mailbox.configuration=NULL;
        owner.layout=configuration->layout;owner.status=configuration->status;
        free(configuration);owner.intent=ui.intent;owner.frame_published=false;
        frame_release(mailbox.command.origin);mailbox.command.origin=NULL;
        cached=prepared(new_pixels[change]);frame_publish(cached);frame_release(cached);
        unsigned revision=ui.info.revision;
        epd_reader_step(2,true,false);
        CHECK(mailbox.ready && !ui.ready && ui.bound==old && ui.pending && ui.info.revision==revision);
        epd_reader_step(8,false,true);CHECK(mailbox.ready && ui.bound==old && ui.pending);
        epd_reader_step(8,true,true);CHECK(!mailbox.ready && ui.ready && ui.pending);
        CHECK(epd_reader_bind(body));
        CHECK(ui.bound!=old && !ui.pending && !ui.ready && ui.paused);
        CHECK(ui.bound->layout==owner.layout && ui.info.line_space==(int)requested_line);
        CHECK(lv_image_get_src(ui.bitmap)==&ui.bound->image && ui.bound->image.data==(const uint8_t *)new_pixels[change]);
        CHECK(lv_obj_is_valid(menu) && !lv_obj_has_flag(menu,LV_OBJ_FLAG_HIDDEN));
        CHECK(saves==saved+1 && mailbox.ack==ui.bound);
        lv_obj_update_layout(lv_screen_active());CHECK(lv_obj_get_x(body)==8 && lv_obj_get_y(body)==12);
        frame_release(mailbox.ack);mailbox.ack=NULL;
    }
    CHECK(cancelled==3 && !locks);
    /* Replaced requests and stale sessions release their origin exactly once. */
    unsigned refs=ui.bound->references,was_disposed=disposed;
    CHECK(epd_reader_reflow());CHECK(ui.bound->references==refs+1);
    epd_reader_turn(1);epd_reader_turn(1);
    CHECK(font_request.command.kind==INTENT_TURN && font_request.command.value==2);
    CHECK(epd_reader_reflow());CHECK(ui.bound->references==refs+1 && disposed==was_disposed+1);
    CHECK(font_request.command.kind==INTENT_TURN && font_request.command.value==2);
    epd_reader_cancel_pending();CHECK(font_request.command.kind==INTENT_CANCEL && !ui.turn_delta);
    ++ui.id;CHECK(!font_request_process());CHECK(!font_request.context && ui.bound->references==refs);
    CHECK(epd_reader_reflow());font_prepare=false;
    CHECK(!font_request_process() && !ui.pending && ui.configuration_failed && ui.info.error_code==READER_ERROR_FONT_MEMORY);
    CHECK(ui.bound->references==refs);font_prepare=true;
    ui.info.error_code=READER_ERROR_NONE;
    /* An unopened document is not published while the font service is busy. */
    view_state_t *opening=calloc(1,sizeof(*opening));CHECK(opening);
    ui.session=opening;font_request.context=opening;font_request.opening=true;font_request.session=ui.id;
    font_begin=false;CHECK(!font_request_process() && !mailbox.incoming && !ui.info.error_code);
    font_request_cancel();CHECK(!ui.session && !font_request.context && !mailbox.incoming);font_begin=1;
    opening=calloc(1,sizeof(*opening));CHECK(opening);
    ui.session=opening;font_request.context=opening;font_request.opening=true;font_request.session=ui.id;
    font_begin=-1;
    CHECK(!font_request_process() && !ui.session && !font_request.context && !mailbox.incoming);
    CHECK(ui.info.error_code==READER_ERROR_FONT_MEMORY);font_begin=1;
    ui.session=&owner;
    epd_reader_detach();CHECK(!epd_reader_bound_image(body));frame_release(ui.bound);ui.bound=NULL;
    CHECK(!owner.references);
    printf("Reader reflow/publish/bind: %u checks passed (font, size, spacing; busy/foreground gates; open overlay)\n",checks);
    return 0;
}
