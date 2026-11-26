#define WIN32_LEAN_AND_MEAN
#include "lvgl.h"
#include "runtime_actual.h"
#include "epd_input.h"
#include "epd_keyboard_internal.h"
#include "ui_font_style.h"
#include "epd_memory_manager.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <crtdbg.h>
#undef small
#define bind notes_bind

enum { W=684, H=1216 };
typedef enum { EPD_FONT_BODY, EPD_FONT_SMALL, EPD_FONT_TITLE, EPD_FONT_CAPTION, EPD_FONT_TEMPERATURE } epd_font_role_t;
static lv_font_t *fonts[5], styled[5][3];
typedef struct { lv_font_t *normal; unsigned flags; } font_wrap_t;
static font_wrap_t wraps[5][3];
static lv_style_t epd_style;
static uint8_t frame[W*H*4];
static note_t sample, ai_result;
static int scene;
static const char *epd_locale_get(void){return "zh_cn";}
static int epd_ui_px(int x){return x;}
static int epd_ui_touch(int x){return LV_MAX(56,x);}
static unsigned epd_ui_scale_revision(void){return 1;}
static void epd_ui_content_area(lv_obj_t *screen,lv_area_t *a){(void)screen;*a=(lv_area_t){32,174,W-33,H-33};}
static unsigned back_requests;
static void gui_app_goback(void){++back_requests;}
void epd_input_add_object(lv_obj_t *o){(void)o;}
typedef struct input_scope {
    struct input_scope *next;
    lv_obj_t *owner,*screen,*back;
    lv_group_t *group;
    const epd_input_ops_t *ops;
    epd_memory_owner_t ops_owner;
    unsigned identity;
} input_scope_t;
static input_scope_t input_scope,*scopes=&input_scope;
static input_scope_t *press_scope;
static unsigned next_identity,published_identity;
/* This fixture has one resident input scope. Callback guards and publication
 * compile from epd_input.c; only their memory/interrupt dependencies are ports. */
static unsigned owner_context_depth,owner_context_entries;
epd_memory_owner_t epd_memory_owner_current(void){return EPD_MEMORY_OWNER_SYSTEM;}
bool epd_memory_owner_retain(epd_memory_owner_t owner){return owner==EPD_MEMORY_OWNER_SYSTEM;}
void epd_memory_owner_release(epd_memory_owner_t owner){assert(owner==EPD_MEMORY_OWNER_SYSTEM);}
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner){
    if(owner!=EPD_MEMORY_OWNER_SYSTEM)return EPD_MEMORY_OWNER_INVALID;
    ++owner_context_depth;++owner_context_entries;return EPD_MEMORY_OWNER_SYSTEM;
}
void epd_memory_owner_leave(epd_memory_owner_t previous){assert(previous==EPD_MEMORY_OWNER_SYSTEM&&owner_context_depth);--owner_context_depth;}
bool epd_memory_owner_get(epd_memory_owner_t owner,epd_memory_owner_snapshot_t *out){
    if(owner!=EPD_MEMORY_OWNER_SYSTEM)return false;
    *out=(epd_memory_owner_snapshot_t){0};return true;
}
bool epd_memory_owner_retain_trusted(epd_memory_owner_t owner)
{ return epd_memory_owner_retain(owner); }
void epd_memory_owner_release_trusted(epd_memory_owner_t owner)
{ epd_memory_owner_release(owner); }
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{ return epd_memory_owner_enter(owner); }
void epd_memory_owner_leave_trusted(epd_memory_owner_t previous)
{ epd_memory_owner_leave(previous); }
void epd_input_cleanup_closed(void){assert(input_scope.ops_owner==EPD_MEMORY_OWNER_SYSTEM);}
static unsigned rt_hw_interrupt_disable(void){return 0;}
static void rt_hw_interrupt_enable(unsigned level){assert(!level);}
static input_scope_t *active_scope(void){return &input_scope;}
static lv_group_t *epd_app_input_group(lv_obj_t *owner){input_scope.owner=input_scope.screen=owner;return input_scope.group;}
static void epd_refresh_request_interaction(void){}
static void epd_refresh_input_objects(lv_obj_t *kbd,lv_obj_t *editor,bool enabled) {(void)kbd;(void)editor;(void)enabled;}
static void gui_app_exec_now(void){}
static bool ui_navigation_cancel(void){return false;}
#include "navigation_actual.inc"
static bool text_update(lv_obj_t *o,const char *t){lv_label_set_text(o,t);return true;}
static bool epd_app_text(lv_obj_t *o,const char *t){return text_update(o,t);}
static const lv_font_t *ui_font_body(void){return fonts[0];}
static const lv_font_t *epd_app_font_role(epd_font_role_t r){return fonts[r];}
static bool styled_dsc(const lv_font_t *f,lv_font_glyph_dsc_t *d,uint32_t c,uint32_t n){
    font_wrap_t *w=f->user_data;bool ok=w->normal->get_glyph_dsc(w->normal,d,c,n);
    if(ok)ui_font_style_metrics(d,f->line_height,w->flags);return ok;
}
static const void *styled_bitmap(lv_font_glyph_dsc_t *d,lv_draw_buf_t *scratch){
    font_wrap_t *w=d->resolved_font->user_data;lv_font_glyph_dsc_t original=*d;original.resolved_font=w->normal;original.entry=NULL;
    const lv_draw_buf_t *raw=w->normal->get_glyph_bitmap(&original,scratch);
    lv_draw_buf_t *out=ui_font_style_bitmap(lv_draw_buf_get_font_handlers(),raw,w->normal->line_height,w->flags);
    if(original.entry&&w->normal->release_glyph)w->normal->release_glyph(w->normal,&original);
    d->entry=(lv_cache_entry_t*)out;return out;
}
static void styled_release(const lv_font_t *f,lv_font_glyph_dsc_t *d){(void)f;lv_draw_buf_destroy((lv_draw_buf_t*)d->entry);d->entry=NULL;}
static size_t composer_font_queries;
static const lv_font_t *epd_app_font_style(epd_font_role_t r,unsigned flags){++composer_font_queries;return flags?&styled[r][(flags-1)&3]:fonts[r];}
static void epd_app_clean_draw_buffer(lv_draw_buf_t *b){(void)b;}
#include "platform.inc"
static void epd_app_object_init(lv_obj_t *o){epd_obj_init(o);}
static lv_obj_t *epd_app_label(lv_obj_t *p,const char *t,int x,int y,int w,const lv_font_t *f){return label_create(p,t,x,y,w,f);}

#include <assert.h>
#include <time.h>
#include "notes_composer.h"
static void *font_data;static long font_size;
static lv_font_t *epd_app_font_create(unsigned family,unsigned size,unsigned weight,uint32_t *id,lv_font_t **fallback){(void)family;(void)weight;*id=1;*fallback=NULL;return lv_tiny_ttf_create_data_ex(font_data,font_size,size,LV_FONT_KERNING_NONE,256);}
static void epd_app_font_destroy(lv_font_t *font){lv_tiny_ttf_destroy(font);}
#include "epd_keyboard.h"
#include "epd_pinyin.c"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_class_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#define EPD_FONT_STYLE_BOLD UI_FONT_STYLE_BOLD
#define RTM_EXPORT(name)
#define epd_get_str(name) host_str_##name
static const char *host_str_keyboard_voice="语音",*host_str_keyboard_hide="收起",*host_str_keyboard_pinyin="输入拼音";
static const char *host_str_keyboard_numbers="数字",*host_str_keyboard_chinese="中文拼音";
void epd_keyboard_open_voice(lv_obj_t *o);
static void ui_voice_input_open_for(lv_obj_t *o){(void)o;}
#include "keyboard_actual.inc"
static bool system_time_local_from_utc(int64_t utc,struct tm *t){(void)utc;memset(t,0,sizeof(*t));t->tm_mon=9;t->tm_mday=29;t->tm_hour=14;t->tm_min=5;return true;}

#include "epd_file_picker.h"
static epd_file_selected_t picked_callback;static void *picked_context;static char picker_extensions[96];
bool epd_file_picker_open(lv_obj_t *owner,const char *extensions,epd_file_selected_t selected,void *context){assert(owner==lv_screen_active());strcpy(picker_extensions,extensions);picked_callback=selected;picked_context=context;return true;}
void epd_file_picker_cancel(lv_obj_t *owner){(void)owner;picked_callback=NULL;picked_context=NULL;}
static const char *epd_speech_error_name(int e){(void)e;return "Audio error";}
static bool grant_done;static unsigned grant_calls;
epd_ai_error_t epd_ai_authorize_service(epd_service_t *owner,uint32_t *ticket){(void)owner;++grant_calls;*ticket=1;return EPD_AI_OK;}
epd_ai_error_t epd_ai_authorization_poll(uint32_t ticket){(void)ticket;grant_done=true;return EPD_AI_OK;}
epd_ai_error_t notes_ai_profile(epd_service_t *owner,epd_ai_profile_t *p,size_t *m){(void)owner;(void)p;(void)m;return grant_done?EPD_AI_OK:EPD_AI_UNAUTHORIZED;}
typedef int epd_app_key_t;typedef int epd_app_key_event_t;
#define EPD_APP_KEY_CLICKED 1
#define EPD_APP_KEY2 2
#include "notes_composer.c"
void epd_service_ui_request(epd_service_t *owner, uint32_t delay) { (void)owner; (void)delay; }
#include "actual_ui.inc"
static void poll(lv_timer_t *timer) { (void)timer; notes_ui_update(); }
static bool fixture_moved_note;static unsigned rows_calls;
size_t notes_rows(notes_row_t *out,size_t offset,size_t capacity,const char *search,const char *book,unsigned filter,unsigned sort){
    ++rows_calls;
    (void)search;(void)sort;
    const char *titles[]={"产品讨论 · 十月例会","《人类简史》读书摘记","周末出行准备","关于新项目的几个想法","采访记录 · 设计师","值得再读的一句话"};
    const char *summaries[]={"本次讨论确定：先完成三个关键页面，再安排下一轮评审。整理结论，确认分工。","真正重要的是，我们如何理解彼此的故事。——《人类简史》","□ 充电器和数据线\n□ 相机和备用电池\n□ 雨伞\n□ 路上读的书","把复杂的问题拆成几件可以行动的小事。","设计应当让人专注于正在做的事。从用户每天遇到的小事开始，找到真正需要解决的问题。","把时间花在重要的人和重要的事情上。"};
    const char *books[]={"工作","阅读","生活","灵感","工作",""};
    if(fixture_moved_note)books[5]="新笔记本";
    size_t total=0,written=0,limit=filter==2?1:filter==3?2:6;
    for(size_t n=0;n<limit;n++){
        if(book&&*book&&((book[0]=='\1'&&*books[n])||(book[0]!='\1'&&strcmp(book,books[n]))))continue;
        if(search&&*search&&!strstr(titles[n],search)&&!strstr(summaries[n],search))continue;
        if(total++<offset||written>=capacity)continue;
        notes_row_t *row=&out[written++];memset(row,0,sizeof(*row));row->id=(uint32_t)n+1;row->pinned=n<2;
        strcpy(row->title,titles[n]);strcpy(row->summary,summaries[n]);strcpy(row->notebook,books[n]);row->tasks=n==0?2:0;
    }return total;
}
size_t notes_tasks(notes_task_t *out,size_t offset,size_t capacity,bool completed){
    const char *texts[]={"整理会议结论，发给项目成员","周五前确认下一版页面布局","把需要带的东西提前装好","补充两条读书笔记"};
    size_t total=completed?1:4;for(size_t i=0;i<capacity&&offset+i<total;i++){memset(out+i,0,sizeof(*out));out[i].note=i<2?1:2;out[i].block=(uint32_t)i+1;out[i].checked=completed;strcpy(out[i].text,texts[offset+i]);strcpy(out[i].title,i<2?"产品讨论 · 十月例会":"周末出行准备");}return total;
}
static size_t fixture_book_count=4;
static char fixture_custom_name[96]="新笔记本";
size_t notes_books(char (*out)[96],size_t offset,size_t count){const char *names[]={"工作","阅读","生活","灵感",fixture_custom_name};for(size_t i=0;i<count&&offset+i<fixture_book_count;i++)strcpy(out[i],names[offset+i]);return fixture_book_count;}
size_t notes_files(notes_file_t *f,size_t offset,size_t count,char *path,size_t size){(void)f;(void)offset;(void)count;snprintf(path,size,"/sdcard");return 0;}
static notes_ai_mode_t preview_mode=AI_CLEAR;
bool notes_preview_original(note_t *out){return note_clone(out,&sample);}
bool notes_preview(note_t *out,notes_ai_mode_t *mode){*mode=preview_mode;return note_clone(out,&ai_result);}
bool notes_take_image(uint8_t **p,unsigned *w,unsigned *h){(void)p;(void)w;(void)h;return false;}


size_t notes_tasks_query(notes_task_t *out,size_t offset,size_t capacity,bool done,const char *search){(void)search;return notes_tasks(out,offset,capacity,done);}
static note_t undo_doc,redo_doc;
bool notes_mutate(notes_edit_fn fn,void *arg,bool coalesce){(void)coalesce;note_clone(&undo_doc,&sample);return fn(&sample,arg);}
bool notes_edit_text(uint32_t id,const char *text,note_t *mirror){note_clone(&undo_doc,&sample);if(!note_edit(&sample,id,text))return false;return !mirror||note_clone(mirror,&sample);}
bool notes_edit_metadata(const char *title,const char *notebook,note_t *mirror){note_clone(&undo_doc,&sample);strcpy(sample.title,title);strcpy(sample.notebook,notebook);++sample.revision;return !mirror||note_clone(mirror,&sample);}
bool notes_document(note_t *out){return note_clone(out,&sample);}
bool notes_undo(bool redo){note_t *from=redo?&redo_doc:&undo_doc;note_clone(redo?&undo_doc:&redo_doc,&sample);return from->id&&note_clone(&sample,from);}
static bool accept_command=true;static uint32_t command_id;static notes_command_t command_kind;
static uint32_t command_arg;static char command_text[192];
bool notes_command(notes_command_t kind,uint32_t id,uint32_t arg,const char *text){command_id=id;command_kind=kind;command_arg=arg;snprintf(command_text,sizeof(command_text),"%s",text?text:"");if(accept_command&&kind==N_BOOK_NEW&&!strcmp(command_text,"新笔记本"))fixture_book_count=5;return accept_command;}
static bool report_status;static notes_status_t reported_status;
bool notes_runtime_start(void){return true;}
void notes_runtime_ui_closed(void){}
void notes_cancel(void){} void notes_save_now(void){} bool notes_status(notes_status_t *s){if(report_status)*s=reported_status;return report_status;}
unsigned notes_preferences(void){return 8;}epd_service_t *notes_owner(void){return NULL;}
bool notes_preview_select(uint32_t id,bool selected){note_block_t *b=note_find(&ai_result,id);if(!b)return false;b->checked=selected;return true;}
static void flush(lv_display_t *display,const lv_area_t *a,uint8_t *pixels){
    int width=a->x2-a->x1+1;for(int y=a->y1;y<=a->y2;y++)memcpy(frame+(y*W+a->x1)*4,pixels+(y-a->y1)*width*4,width*4);lv_display_flush_ready(display);
}
static void line(lv_obj_t *parent,int x,int y,int width,int height){lv_obj_t *o=lv_obj_create(parent);epd_obj_init(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,height);lv_obj_set_style_bg_color(o,lv_color_black(),0);lv_obj_set_style_bg_opa(o,255,0);}
static void shell(lv_obj_t *screen){
    label_create(screen,"14:30",5,15,110,fonts[0]);
    lv_obj_t *battery=panel_create(screen,601,12,72,40);lv_obj_set_style_border_color(battery,lv_color_black(),0);lv_obj_set_style_border_width(battery,2,0);lv_obj_set_style_radius(battery,5,0);centered_label(battery,"86",3,5,63,fonts[1]);line(screen,674,25,4,14);
    line(screen,558,37,5,12);line(screen,566,30,5,19);line(screen,574,22,5,27);

    lv_obj_t *handle=lv_obj_create(screen);epd_obj_init(handle);lv_obj_set_pos(handle,222,H-16);lv_obj_set_size(handle,240,5);lv_obj_set_style_bg_color(handle,lv_color_black(),0);lv_obj_set_style_bg_opa(handle,255,0);lv_obj_set_style_radius(handle,3,0);
}
static void init_note(void){
    note_init(&sample,1,0);strcpy(sample.title,"产品讨论 · 十月例会");strcpy(sample.notebook,"工作");sample.pinned=true;

    note_block_t *p=note_insert(&sample,sample.count,NOTE_TEXT,"今天主要讨论了下一版的页面布局。先把记录做好，再把需要行动的事情单独列出来。");
    const char *strong=strstr(p->text,"先把记录做好");note_style(&sample,p->id,(uint32_t)(strong-p->text),(uint32_t)(strong-p->text+strlen("先把记录做好")),NOTE_BOLD);
    note_insert(&sample,sample.count,NOTE_HEADING,"这次确定的事情");
    note_insert(&sample,sample.count,NOTE_TEXT,"先完成三个关键页面，再安排下一轮评审。设计需要让人专注于内容，常用操作应当随手可用。");
    note_insert(&sample,sample.count,NOTE_QUOTE,"好的工具，应当让人专注于正在做的事。");
    note_insert(&sample,sample.count,NOTE_TASK,"整理会议结论，发给项目成员");
    note_block_t *done=note_insert(&sample,sample.count,NOTE_TASK,"确认这一版的主要功能范围");done->checked=true;
    note_insert(&sample,sample.count,NOTE_IMAGE,"白板讨论.jpg");
    note_block_t *audio=note_insert(&sample,sample.count,NOTE_AUDIO,"十月例会录音");audio->milliseconds=12*60000+34000;
    note_mark(&sample,audio->id,95000,"页面布局");note_mark(&sample,audio->id,362000,"下一步安排");
    note_init(&ai_result,2,0);note_insert(&ai_result,0,NOTE_TEXT,"本次会议围绕下一版的页面布局展开讨论，确定先完成三个关键页面，再安排下一轮评审。");
    note_insert(&ai_result,1,NOTE_TEXT,"接下来需要整理会议结论，并在周五前确认页面布局。已经确认的功能范围保持不变。");
}
static void check_shared_keyboard(void)
{
    assert(page->keyboard && page->input);
    lv_obj_t *reference = epd_keyboard_create(page->screen, page->input, NULL);
    assert(reference);
    lv_obj_t *actual_keys = lv_obj_get_child(page->keyboard, 0);
    lv_obj_t *reference_keys = lv_obj_get_child(reference, 0);
    assert(lv_obj_check_type(actual_keys, &lv_keyboard_class));
    assert(epd_keyboard_get_textarea(page->keyboard) == page->input);
    assert(lv_obj_get_height(page->keyboard) == epd_keyboard_preferred_height(reference));
    assert(lv_obj_get_y(page->keyboard) + lv_obj_get_height(page->keyboard) == lv_obj_get_height(page->screen));
    assert(lv_obj_get_style_bg_opa(page->keyboard, 0) == LV_OPA_TRANSP);
    assert(lv_color_eq(lv_obj_get_style_bg_color(page->keyboard, 0), lv_obj_get_style_bg_color(reference, 0)));
    assert(lv_obj_get_style_pad_top(actual_keys, 0) == lv_obj_get_style_pad_top(reference_keys, 0));
    assert(lv_obj_get_style_pad_bottom(actual_keys, 0) == lv_obj_get_style_pad_bottom(reference_keys, 0));
    assert(lv_obj_get_style_pad_left(actual_keys, 0) == lv_obj_get_style_pad_left(reference_keys, 0));
    assert(lv_obj_get_style_pad_right(actual_keys, 0) == lv_obj_get_style_pad_right(reference_keys, 0));
    assert(lv_obj_get_style_pad_row(actual_keys, 0) == lv_obj_get_style_pad_row(reference_keys, 0));
    assert(lv_obj_get_style_pad_column(actual_keys, 0) == lv_obj_get_style_pad_column(reference_keys, 0));
    assert(lv_obj_get_style_radius(actual_keys, LV_PART_ITEMS) == lv_obj_get_style_radius(reference_keys, LV_PART_ITEMS));
    assert(lv_color_eq(lv_obj_get_style_border_color(actual_keys, LV_PART_ITEMS),
                       lv_obj_get_style_border_color(reference_keys, LV_PART_ITEMS)));
    if (page->toolbar) {
        lv_area_t toolbar, keyboard;
        lv_obj_get_coords(page->toolbar, &toolbar);
        lv_obj_get_coords(page->keyboard, &keyboard);
        assert(toolbar.y2 < keyboard.y1);
    }
    lv_obj_delete(reference);
}

static void capture(const char *directory,const char *name){
    lv_obj_update_layout(page->screen);lv_obj_invalidate(page->screen);lv_refr_now(NULL);
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);FILE *f=fopen(path,"wb");if(!f){perror(path);exit(2);}fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(size_t i=0;i<W*H;i++){uint8_t rgb[3]={frame[i*4+2],frame[i*4+1],frame[i*4]};fwrite(rgb,1,3,f);}fclose(f);printf("%s\n",path);
}

static lv_point_t pointer_point;static bool pointer_down;static lv_indev_t *pointer_device;
static void read_pointer(lv_indev_t *device,lv_indev_data_t *data){(void)device;data->point=pointer_point;data->state=pointer_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void pointer_step(int x,int y,bool down,unsigned ms){pointer_point=(lv_point_t){x,y};pointer_down=down;for(unsigned i=0;i<ms;i+=20){lv_tick_inc(20);lv_timer_handler();}}
static void click_at(int x,int y){pointer_step(x,y,true,80);pointer_step(x,y,false,80);}
static void system_back(void){page->action=0;epd_input_back();assert(page->action==A_BACK||page->action==A_HIDE);poll(NULL);}
static void check_redesign(const char *directory)
{
    unsigned failures=0;
#define AUDIT_CHECK(condition, message) do { if(!(condition)){fprintf(stderr,"FAIL: %s\n",message);++failures;} } while(0)
    uint32_t first=sample.blocks[0].id, other=sample.blocks[2].id;
    keyboard_close();page->mode=READ;page->caret=(notes_caret_t){.block=first};page->caret_override=true;rebuild();
    do_action(A_BLOCK,other);rebuild();do_action(A_EDIT,other);rebuild();
    AUDIT_CHECK(page->composer->caret.block==other,"editing a selected paragraph must target that paragraph");
    AUDIT_CHECK(!strcmp(lv_textarea_get_text(page->input),note_find(&sample,other)->text),"keyboard must contain the selected paragraph");

    do_action(A_INSERT,NOTE_TEXT);rebuild();uint32_t inserted=page->block;
    do_action(A_UNDO,0);rebuild();
    note_block_t *active=note_find(&sample,page->composer->caret.block);
    AUDIT_CHECK(!note_find(&sample,inserted),"undo must remove the inserted paragraph");
    AUDIT_CHECK(active!=NULL,"undoing an insertion must restore an existing editing target");
    if(active){uint32_t id=active->id;lv_textarea_add_text(page->input,"撤销后输入");AUDIT_CHECK(strstr(note_find(&sample,id)->text,"撤销后输入")!=NULL,"typing after undo must reach the document");}

    page->status.recording=true;page->mode=EDIT;page->keyboard_wanted=true;rebuild();lv_obj_update_layout(page->screen);
    lv_area_t recorder,keyboard,toolbar;
    lv_obj_get_coords(lv_obj_get_parent(page->progress),&recorder);lv_obj_get_coords(page->keyboard,&keyboard);lv_obj_get_coords(page->toolbar,&toolbar);
    AUDIT_CHECK(recorder.y2<keyboard.y1,"recording controls must remain above the keyboard");
    AUDIT_CHECK(toolbar.y2<recorder.y1,"editing toolbar must not overlap recording controls");
    page->action=0;click_at(recorder.x1+page->width-105,recorder.y1+46);AUDIT_CHECK(page->action==A_STOP,"stop recording must remain touchable while typing");page->action=0;
    capture(directory,"11-recording-editor");
    char original_title[sizeof(page->note.title)];strcpy(original_title,page->note.title);page->note.title[0]=0;
    for(unsigned i=0;i<17;i++)strcat(page->note.title,"长标题");
    rebuild();lv_obj_update_layout(page->screen);lv_area_t editor;lv_obj_get_coords(page->editor_host,&editor);lv_obj_get_coords(page->toolbar,&toolbar);
    AUDIT_CHECK(editor.y2<toolbar.y1,"long note titles must not push the editor underneath the toolbar");
    strcpy(page->note.title,original_title);
    page->status.recording=false;keyboard_close();page->mode=LIST;rebuild();lv_obj_update_layout(page->screen);
    page->action=0;click_at(285,1138);AUDIT_CHECK(page->action==0,"floating toolbar gaps must not open a note underneath");page->action=0;

    strcpy(page->notebook,"工作");page->mode=NOTEBOOKS;rebuild();do_action(A_FILTER,2);rebuild();
    AUDIT_CHECK(!page->notebook[0],"drawer trash view must show notes from every notebook");
    page->notebook[0]=0;page->filter=0;page->mode=READ;rebuild();
    assert(!failures);
#undef AUDIT_CHECK
}
static void check_navigation(void)
{
    unsigned exits=back_requests;
    if(scene==0){system_back();assert(back_requests==exits+1);return;}
    if(scene==1||scene==2){system_back();assert(page->mode==LIST);}
    if(scene==3){system_back();assert(page->mode==MENU);system_back();assert(page->mode==LIST);}
    if(scene==4){
        page->action=0;click_at(42,102);assert(page->action==A_BACK);poll(NULL);assert(page->mode==LIST);
        page->mode=READ;rebuild();system_back();assert(page->mode==LIST);
    }
    if(scene==5){
        page->mode=EDIT;page->block=sample.blocks[0].id;page->caret=(notes_caret_t){.block=page->block};
        page->caret_override=true;page->keyboard_wanted=true;rebuild();assert(page->keyboard);
        lv_textarea_add_text(page->input,"返回保留");
        system_back();assert(!page->keyboard&&page->mode==EDIT);assert(strstr(sample.blocks[0].text,"返回保留"));
        system_back();assert(page->mode==READ);system_back();assert(page->mode==LIST);
    }
    if(scene==6){
        if(page->keyboard){system_back();assert(!page->keyboard&&page->mode==FORMAT);}
        system_back();assert(page->mode==EDIT);system_back();assert(page->mode==READ);
    }
    if(scene==7){system_back();assert(page->mode==LIST&&page->filter==0);}
    if (scene == 8) {
        system_back();
        assert(page->mode == LIST && page->status.recording && page->progress);
        lv_area_t bar;
        lv_obj_update_layout(page->screen);
        lv_obj_get_coords(lv_obj_get_parent(page->progress), &bar);
        page->action = 0;
        click_at(bar.x1 + page->width - 105, bar.y1 + 46);
        assert(page->action == A_STOP);
        notes_ui_update();
        assert(command_kind == N_REC_STOP);
    }
    if(scene==9){
        system_back();assert(page->mode==READ);
        page->confirm_action=A_DELETE;rebuild();system_back();assert(!page->confirm_action&&page->mode==READ);
        assert(back_requests==exits);notes_ui_retire();epd_input_back();assert(back_requests==exits+1);return;
    }
    assert(back_requests==exits);
}
static bool has_label(lv_obj_t *object,const char *text)
{
    if(lv_obj_check_type(object,&lv_label_class)&&!strcmp(lv_label_get_text(object),text))return true;
    for(unsigned i=0;i<lv_obj_get_child_count(object);i++)if(has_label(lv_obj_get_child(object,i),text))return true;
    return false;
}
#include "notebook_checks.inc"
static void check_ai_preview(void)
{
    note_clear(&ai_result);note_init(&ai_result,sample.id,0);
    note_block_t *result=note_insert(&ai_result,0,NOTE_TEXT,"这一段的润色结果");result->source=sample.blocks[2].id;
    page->mode=PREVIEW;page->show_original=true;page->ai_target=0;rebuild();
    lv_obj_t *preview_panel=lv_obj_get_child(page->body,-1);
    bool original_scoped=has_label(preview_panel,sample.blocks[0].text)&&has_label(preview_panel,sample.blocks[2].text);
    if(!original_scoped)fprintf(stderr,"FAIL: paragraph preview must compare only its source paragraph\n");
    do_action(A_AI,AI_CONCISE);bool rerun_scoped=command_id==0;
    if(!rerun_scoped)fprintf(stderr,"FAIL: changing preview style must retain the result's source scope\n");
    result=note_insert(&ai_result,1,NOTE_TEXT,"另一段的润色结果");result->source=sample.blocks[0].id;
    page->mode=PREVIEW;page->show_original=false;page->ai_target=sample.blocks[2].id;rebuild();do_action(A_AI,AI_FORMAL);
    bool whole_scope=command_id==0;if(!whole_scope)fprintf(stderr,"FAIL: whole-note preview must not reuse a previous paragraph target\n");
    note_clear(&ai_result);note_init(&ai_result,sample.id,0);preview_mode=AI_TASKS;
    for(unsigned i=0;i<300;i++){char title[64];snprintf(title,sizeof(title),"Task %u",i+1);note_block_t *b=note_insert(&ai_result,i,NOTE_TASK,title);b->source=sample.blocks[0].id;b->checked=true;}
    page->mode=PREVIEW;page->offset=0;rebuild();lv_obj_update_layout(page->screen);
    bool controls=page->binding_count<256;page->action=0;click_at(500,1122);controls=controls&&page->action==A_ADOPT;page->action=0;
    if(!controls)fprintf(stderr,"FAIL: a long AI task result must retain working apply/discard controls\n");
    assert(original_scoped&&rerun_scoped&&whole_scope&&controls);
    do_action(A_NEXT,0);rebuild();assert(page->offset==8&&has_label(page->body,"Task 9"));assert(!has_label(page->body,"Task 1"));
    do_action(A_TASK_SELECT,ai_result.blocks[8].id);rebuild();assert(!ai_result.blocks[8].checked);
    do_action(A_PREV,0);rebuild();assert(page->offset==0&&has_label(page->body,"Task 1"));
    preview_mode=AI_CLEAR;page->show_original=false;page->mode=PREVIEW;
}
static void check_forms(const char *directory)
{
    page->mode=READ;rebuild();do_action(A_META,0);rebuild();
    assert(page->mode==META&&has_label(page->body,"所在笔记本"));capture(directory,"11-title");
    lv_textarea_set_text(page->input,"新的标题");do_action(A_PICK_BOOK,0);rebuild();
    assert(!strcmp(sample.title,"新的标题")&&page->mode==BOOK_PICK);capture(directory,"12-choose-notebook");
    do_action(A_CHOOSE_BOOK,2);rebuild();assert(!strcmp(sample.notebook,"阅读")&&page->mode==META);
    do_action(A_PICK_BOOK,0);rebuild();do_action(A_BOOK_NEW,0);rebuild();assert(page->mode==BOOK_EDIT);capture(directory,"13-new-notebook");
    do_action(A_BACK,0);rebuild();assert(page->mode==BOOK_PICK);do_action(A_BACK,0);rebuild();assert(page->mode==META);
    page->selected_count=1;page->selected[0]=sample.id;do_action(A_BATCH_MOVE,0);rebuild();do_action(A_BOOK_NEW,0);rebuild();
    lv_textarea_set_text(page->input,"项目记录");do_action(A_SAVE,0);rebuild();assert(page->mode==BOOK_PICK&&page->batch_move&&command_kind==N_BOOK_NEW);
    do_action(A_CHOOSE_BOOK,2);rebuild();assert(page->mode==LIST&&!strcmp(page->destination,"阅读"));page->selected_count=0;page->batch_move=false;
    page->mode=LIST;rebuild();do_action(A_SEARCH,0);rebuild();lv_textarea_set_text(page->input,"讨论");
    do_action(A_SAVE,0);rebuild();assert(page->mode==SEARCH&&command_kind==N_QUERY);do_action(A_HIDE,0);lv_obj_update_layout(page->screen);assert(has_label(page->body,"产品讨论 · 十月例会"));capture(directory,"14-search-results");
    lv_textarea_set_text(page->input,"不存在的内容");accept_command=false;do_action(A_SAVE,0);assert(page->pending&&!page->search_due&&page->mode==SEARCH);accept_command=true;
    do_action(A_BACK,0);rebuild();assert(page->mode==LIST);
    page->mode=READ;rebuild();do_action(A_AI_MENU,0);rebuild();grant_done=false;page->ai_authorized=false;page->ai_checked=0;
    lv_obj_update_layout(page->screen);assert(has_label(page->body,"授权服务"));do_action(A_GRANT,0);assert(page->grant_ticket);ai_authorization_refresh();rebuild();lv_obj_update_layout(page->screen);
    assert(page->ai_authorized&&has_label(page->body,"已授权服务"));unsigned calls=grant_calls;do_action(A_GRANT,0);assert(grant_calls==calls);capture(directory,"15-ai-authorized");
    accept_command=false;do_action(A_AI,AI_CLEAR);assert(page->mode==AI_MENU);
    page->mode=PREVIEW;do_action(A_ADOPT,0);assert(page->mode==PREVIEW&&!page->adopt_pending);accept_command=true;
    grant_done=false;page->ai_checked=0;page->mode=AI_MENU;ai_authorization_refresh();rebuild();lv_obj_update_layout(page->screen);assert(!page->ai_authorized&&has_label(page->body,"授权服务"));
    page->mode=READ;page->status.online=true;page->status.transcribing=false;rebuild();assert(!page->progress);
    for(size_t i=0;i<page->binding_count;++i)assert(page->bindings[i].action!=A_CANCEL_TRANSCRIBE);
    page->status.transcribing=true;rebuild();assert(page->progress);page->status.online=page->status.transcribing=false;
    page->pending=false;page->mode=READ;rebuild();
}

static void check_audio(const char *directory){
    page->feedback[0]=0;page->feedback_until=0;notice_text("");note_t saved={0};assert(note_clone(&saved,&sample));note_clear(&sample);note_init(&sample,88,0);strcpy(sample.title,"产品讨论 · 录音记录");
    note_block_t *audio=note_insert(&sample,0,NOTE_AUDIO,"十月例会录音");uint32_t id=audio->id;audio->milliseconds=754000;strcpy(audio->attachment,"attachments/00000001.wav");
    note_mark(&sample,id,95000,"页面布局");note_mark(&sample,id,362000,"下一步安排");
    note_clone(&page->note,&sample);page->status=(notes_status_t){.ready=true};page->mode=READ;page->caret=(notes_caret_t){0};page->caret_override=true;page->pending=false;rebuild();lv_obj_update_layout(page->screen);
    assert(note_word_count(&page->note)==0);capture(directory,"16-audio-note");
    lv_area_t area;lv_obj_get_coords(page->composer->canvas,&area);
    page->action=0;click_at(area.x1+120,area.y1+30);assert(page->action==A_INSERT_AT&&page->value==0);page->action=0;
    click_at(area.x1+120,area.y1+100);assert(page->action==A_BLOCK&&page->value==id);page->action=0;
    click_at(area.x1+120,area.y1+350);assert(page->action==A_INSERT_AT&&page->value==1);page->action=0;
    do_action(A_INSERT_AT,1);rebuild();assert(page->mode==EDIT&&sample.count==2&&sample.blocks[0].id==id);lv_textarea_add_text(page->input,"会后补充：周五确认页面布局。");do_action(A_HIDE,0);
    do_action(A_INSERT_AT,0);rebuild();assert(sample.count==3&&sample.blocks[1].id==id);lv_textarea_add_text(page->input,"今天的讨论录音，重点已做标记。");do_action(A_HIDE,0);do_action(A_SAVE,0);rebuild();capture(directory,"17-audio-with-text");
    assert(note_word_count(&sample)==(strlen(sample.blocks[0].text)+strlen(sample.blocks[2].text))/3);
    do_action(A_BLOCK,id);rebuild();assert(page->mode==AUDIO_PLAYER&&has_label(page->body,"转写"));capture(directory,"18-audio-player");
    page->status.playback_block=id;page->status.playback_note=sample.id;page->status.playback_ms=95000;page->status.playing=true;rebuild();assert(has_label(page->body,"00:01:35"));capture(directory,"19-audio-playing");
    page->action=0;click_at(W/2,780);assert(page->action==A_PLAY_STOP);page->action=0;page->status.playing=false;
    do_action(A_AUDIO_MENU,0);rebuild();capture(directory,"20-audio-menu");do_action(A_BACK,0);rebuild();assert(page->mode==AUDIO_PLAYER);
    do_action(A_MARKS,0);rebuild();capture(directory,"21-audio-markers");assert(has_label(page->body,"页面布局"));do_action(A_BACK,0);rebuild();assert(page->mode==AUDIO_PLAYER);
    do_action(A_EDIT,id);rebuild();lv_textarea_set_text(page->input,"新的录音名");do_action(A_SAVE,0);rebuild();assert(page->mode==AUDIO_PLAYER&&!strcmp(note_find(&sample,id)->text,"新的录音名"));
    do_action(A_BACK,0);rebuild();assert(page->mode==READ);do_action(A_BACK,0);rebuild();assert(page->mode==LIST);
    do_action(A_IMPORT,0);assert(page->mode==LIST&&picked_callback&&!strcmp(picker_extensions,".mp3;.wav;.md;.markdown;.txt"));
    assert(picked_callback("/sdcard/example.mp3",picked_context)&&command_kind==N_IMPORT&&page->pending_document==1);page->pending_document=0;
    note_clear(&sample);sample=saved;notes_document(&page->note);page->status=(notes_status_t){.ready=true};page->mode=READ;page->pending=false;rebuild();
}

#include "composer_checks.inc"

int main(int argc,char **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);_CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    if(argc!=3)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);void *data=malloc(bytes);if(fread(data,1,bytes,f)!=(size_t)bytes)return 2;fclose(f);
    font_data=data;font_size=bytes;lv_init();const int sizes[]={28,24,36,20,64};for(int r=0;r<5;r++){fonts[r]=lv_tiny_ttf_create_data_ex(data,bytes,sizes[r],LV_FONT_KERNING_NONE,256);if(!fonts[r]){fprintf(stderr,"tiny_ttf font initialization failed: role=%d size=%d path=%s\n",r,sizes[r],argv[1]);return 3;}fonts[r]->fallback=&lv_font_montserrat_14;for(int s=0;s<3;s++){wraps[r][s]=(font_wrap_t){fonts[r],(unsigned)s+1};styled[r][s]=*fonts[r];styled[r][s].get_glyph_dsc=styled_dsc;styled[r][s].get_glyph_bitmap=styled_bitmap;styled[r][s].release_glyph=styled_release;styled[r][s].user_data=&wraps[r][s];}}
    lv_style_init(&epd_style);lv_style_set_anim_duration(&epd_style,0);lv_style_set_bg_grad_dir(&epd_style,LV_GRAD_DIR_NONE);lv_style_set_shadow_width(&epd_style,0);
    lv_display_t *d=lv_display_create(W,H);lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);void *buffer_memory=malloc(W*H*4+LV_DRAW_BUF_ALIGN);assert(buffer_memory);void *buffer=lv_draw_buf_align(buffer_memory,LV_COLOR_FORMAT_XRGB8888);lv_display_set_buffers(d,buffer,NULL,W*H*4,LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(d,flush);

    pointer_device=lv_indev_create();lv_indev_set_type(pointer_device,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer_device,read_pointer);
    check_composer_edits(d,buffer);
    init_note();const char *names[]={"01-home","02-notebooks","03-menu","04-sort","05-note","06-editor","07-format","08-tasks","09-recording","10-ai-preview"};
    for(scene=0;scene<10;scene++){
        lv_obj_t *screen=lv_screen_active();lv_obj_clean(screen);page=calloc(1,sizeof(*page));page->screen=screen;epd_obj_init(screen);lv_obj_set_style_bg_color(screen,lv_color_white(),0);lv_obj_set_style_bg_opa(screen,255,0);lv_obj_set_style_text_color(screen,lv_color_black(),0);lv_obj_set_style_text_font(screen,fonts[0],0);shell(screen);
        page->title=label(screen,"笔记",104,81,520,false);page->body=lv_obj_create(screen);epd_obj_init(page->body);page->notice=label(screen,"",0,0,100,true);page->status.ready=true;note_clone(&page->note,&sample);page->binding_count=0;page->grid=true;page->completed=true;
        switch(scene){case 0:page->mode=LIST;break;case 1:page->mode=NOTEBOOKS;break;case 2:page->mode=MENU;page->return_mode=LIST;break;case 3:page->mode=SORT_MENU;break;case 4:page->mode=READ;break;case 5:page->mode=EDIT;page->block=sample.blocks[0].id;page->caret=(notes_caret_t){.block=page->block};page->keyboard_wanted=true;break;case 6:page->mode=FORMAT;page->block=sample.blocks[0].id;page->caret=(notes_caret_t){.block=page->block,.start=51,.end=69};break;case 7:page->mode=LIST;page->filter=1;break;case 8:page->mode=READ;page->status.recording=true;page->status.recording_ms=125000;break;case 9:page->mode=PREVIEW;break;}
        input_scope=(input_scope_t){.owner=screen,.screen=screen};navigation_init();

        rebuild();lv_obj_update_layout(screen);
        if(page->keyboard)check_shared_keyboard();
        if(page->composer){assert(page->composer->count>0);assert(lv_obj_get_height(page->composer->viewport)>100);}
        capture(argv[2],names[scene]);
        if(scene==0){click_at(447,1138);assert(page->action==A_NEW);page->action=0;click_at(70,380);assert(page->action==A_OPEN);page->action=0;
            assert(!back_requests);check_notebook_switching(argv[2]);check_notebook_background_updates();}
        if(scene==4){lv_area_t area;lv_obj_get_coords(page->composer->canvas,&area);click_at(area.x1+35,area.y1+15);assert(page->action==A_FOCUS);page->action=0;}
        if(scene==5){
            lv_area_t area;lv_obj_get_coords(page->composer->canvas,&area);pointer_step(area.x1+35,area.y1+15,true,700);pointer_step(area.x1+185,area.y1+15,true,80);pointer_step(area.x1+185,area.y1+15,false,80);
            assert(page->composer->caret.start!=page->composer->caret.end);
            notes_caret_t cursor={.block=page->block};notes_composer_caret(page->composer,&cursor,true);
            uint32_t id=page->block;lv_textarea_set_cursor_pos(page->input,LV_TEXTAREA_CURSOR_LAST);lv_textarea_add_text(page->input,"新增文字");
            assert(strstr(note_find(&sample,id)->text,"新增文字"));do_action(A_HIDE,0);assert(!page->keyboard);assert(strstr(note_find(&sample,id)->text,"新增文字"));
            notes_caret_t state={.block=id,.start=0,.end=6};notes_composer_caret(page->composer,&state,true);do_action(A_FORMAT,0);rebuild();do_action(A_BOLD,0);rebuild();assert(note_find(&sample,id)->span_count>0);
            do_action(A_UNDO,0);rebuild();do_action(A_REDO,0);rebuild();assert(note_find(&sample,id)->span_count>0);
            notes_caret_t replace={.block=id,.start=0,.end=6,.cursor=6};notes_composer_caret(page->composer,&replace,true);
            char tail[1024];strcpy(tail,note_find(&sample,id)->text+6);lv_textarea_add_text(page->input,"测试");assert(!strncmp(note_find(&sample,id)->text,"测试",6));assert(!strcmp(note_find(&sample,id)->text+6,tail));
            notes_composer_caret(page->composer,&replace,true);lv_textarea_delete_char(page->input);assert(!strcmp(note_find(&sample,id)->text,tail));
            do_action(A_INSERT,NOTE_TASK);rebuild();assert(page->composer->caret.block==sample.blocks[sample.count-1].id);
            lv_textarea_add_text(page->input,"新待办");assert(!strcmp(sample.blocks[sample.count-1].text,"新待办"));
            do_action(A_HIDE,0);lv_obj_update_layout(screen);assert(lv_obj_get_height(page->editor_host)==lv_obj_get_height(page->composer->viewport));
            lv_textarea_cursor_left(page->input);notes_composer_poll(page->composer);assert(page->composer->caret.cursor==strlen("新待"));
            do_action(A_SEARCH,0);rebuild();assert(page->keyboard&&page->input);
            lv_obj_update_layout(screen);check_shared_keyboard();
            lv_textarea_add_text(page->input,"会议");do_action(A_HIDE,0);assert(!strcmp(lv_textarea_get_text(page->input),"会议"));
            do_action(A_BACK,0);rebuild();page->mode=READ;rebuild();do_action(A_AI_MENU,0);rebuild();do_action(A_AI,AI_CLEAR);assert(command_kind==N_AI&&command_id==0);
            page->mode=READ;rebuild();do_action(A_AI_MENU,id);rebuild();do_action(A_AI,AI_CONCISE);assert(command_id==0);
            uint32_t audio_id=0;for(size_t a=0;a<sample.count;a++)if(sample.blocks[a].kind==NOTE_AUDIO)audio_id=sample.blocks[a].id;
            do_action(A_BLOCK,audio_id);rebuild();do_action(A_EDIT,audio_id);rebuild();assert(page->mode==BLOCK_TITLE);lv_textarea_set_text(page->input,"录音新名称");do_action(A_SAVE,0);rebuild();assert(!strcmp(note_find(&sample,audio_id)->text,"录音新名称"));
            fprintf(stdout,"editor interaction checks passed\n");
        }
        if(scene==4){check_forms(argv[2]);check_audio(argv[2]);}
        if(scene==5)check_redesign(argv[2]);
        if(scene==9)check_ai_preview();
        check_navigation();
        keyboard_close();lv_obj_clean(page->body);page->composer=NULL;if(page->heading)epd_app_font_destroy(page->heading);if(page->heading_fallback)epd_app_font_destroy(page->heading_fallback);if(page->clock_font)epd_app_font_destroy(page->clock_font);if(page->clock_fallback)epd_app_font_destroy(page->clock_fallback);note_clear(&page->note);note_clear(&page->preview);note_clear(&page->original);free(page);page=NULL;note_clear(&sample);note_clear(&ai_result);init_note();
    }assert(!owner_context_depth&&owner_context_entries);return 0;
}
