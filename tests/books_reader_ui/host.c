#include "host_api.h"
#include "ui_system_font_cache.h"
#include <windows.h>
#include <crtdbg.h>
#define rt_kprintf(...) ((void)0)
void epd_refresh_input_objects(lv_obj_t *keyboard, lv_obj_t *editor, bool enabled)
{ (void)keyboard; (void)editor; (void)enabled; }
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FATAL %d: %s\n",__LINE__,#x);exit(2);}}while(0)
static lv_font_t *fonts[5];
static lv_style_t epd_style;
static uint32_t host_tick;
static unsigned settings[UI_SETTING_COUNT],seek_calls,last_seek,installs,cancels,manages;
static bool sd_available=true;
static unsigned font_count=1,selected_font;
static bool display_idle=true,background_paused,settings_changed,font_busy,font_fail,navigation_fail;
static bool document_active=true;
static unsigned reflows,toc_count=24,chapter_calls,toc_reads,root_deletes,sheet_deletes,keyboard_deletes;
static const char *chapter_title="第一部 · 惶惑";
static unsigned chapter_index;
static bool realistic_toc;
static const struct { const char *title;unsigned level; } toc_fixture[]={
    {"版权页",0},{"献辞",0},
    {"推荐序 凡是过往，皆为序章——小米十年的成长与选择",0},
    {"前言 小米10年，生而逢时",0},
    {"第一部分 小米的诞生与崛起",0},
    {"第一章 雷军的重新出发",1},{"第二章 为发烧而生",1},
    {"第三章 从一个小团队开始",1},{"第四章 用产品赢得信任",1},
    {"第二部分 在变化中寻找方向",0},
    {"第五章 互联网思维",1},{"第六章 新的挑战",1},
    {"第七章 坚持长期的选择",1},{"第八章 走向世界",1},
    {"第九章 逆转向上",1},{"全面屏手机的发布与探索",2},
    {"第三部分 继续向前",0},{"第十章 下一段旅程",1},
    {"第十一章 保持热爱",1},{"后记",0},{"致谢",0},{"附录",0}
};
static unsigned chapter_updates,invalidations;
static bool is_menu_chapter(lv_obj_t *object);
static unsigned toc_seeks,chapter_moves;
static bool can_seek_progress;
static unsigned progress_seeks,last_progress_seek;
static bool navigation_active;
static int navigation_status = 1;
int epd_reader_toc_prepare(bool active) { navigation_active = active; return navigation_status; }
static int last_chapter;
static unsigned turn_calls,step_calls,last_step_budget,events,reflow_event,navigation_event;
static int last_turn;
static bool target_pending,last_step_foreground,last_step_commit;
static unsigned ui_requests;
static bool last_runnable;
static bool reader_page_live=true,reader_resume_pending;
static struct { bool active; unsigned last_step_ms,step_ms,max_step_ms; } submit_trace;
#define UI_PAGE_HOME 0
#define UI_PAGE_COUNT 16
#define UI_READER_WORK_IDLE READER_WORK_IDLE
typedef reader_work_t ui_reader_work_t;
static lv_obj_t *voice_target;
static reader_view_t reading={.title="四世同堂.epub",.text="瑞宣立在门口，望着熟悉的胡同。",.page=18,.pages=428,.ready=true};
static book_ai_view_t backend={.revision=1,.generation=1,.authorized=true};
static char draft[BOOK_AI_DRAFT];
bool host_locale_en;
books_page_t state;
books_page_t *current=&state;
int pending_page=NAV_IDLE;
epd_service_t *books_service=(epd_service_t *)1;
const lv_font_t *epd_app_font_role(epd_font_role_t r){return fonts[r];}
const lv_font_t *epd_app_font_style(epd_font_role_t r,unsigned flags){(void)flags;return fonts[r];}
const lv_font_t *ui_font_body(void){return fonts[0];}
const lv_font_t *ui_font_small(void){return fonts[1];}
const lv_font_t *ui_font_title(void){return fonts[2];}
const lv_font_t *ui_font_caption(void){return fonts[3];}
uint32_t rt_tick_get(void){return host_tick;}
uint32_t epd_locale_revision(void){return 1;}
const char *epd_locale_get(void){return "zh_cn";}
unsigned epd_ui_scale_percent(void){return 100;}
uint32_t epd_ui_scale_revision(void){return 1;}
static int32_t ui_shell_content_bottom(void){return 1184;}
#include "scale.inc"
void *epd_app_alloc(size_t n,epd_app_memory_t m){(void)m;return malloc(n);}
void epd_app_free(void *p){free(p);}
size_t epd_app_psram_available(void){return 4*1024*1024;}
/* UI fixtures run in the system context; ownership transitions are covered by
 * unified_lv_context using these same production input functions. */
epd_memory_owner_t epd_memory_owner_current(void){return EPD_MEMORY_OWNER_SYSTEM;}
bool epd_memory_owner_retain(epd_memory_owner_t owner){return owner==EPD_MEMORY_OWNER_SYSTEM;}
void epd_memory_owner_release(epd_memory_owner_t owner){REQUIRE(owner==EPD_MEMORY_OWNER_SYSTEM);}
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{return owner==EPD_MEMORY_OWNER_SYSTEM?EPD_MEMORY_OWNER_SYSTEM:EPD_MEMORY_OWNER_INVALID;}
void epd_memory_owner_leave(epd_memory_owner_t previous){REQUIRE(previous==EPD_MEMORY_OWNER_SYSTEM);}
bool epd_memory_owner_get(epd_memory_owner_t owner,epd_memory_owner_snapshot_t *out)
{if(owner!=EPD_MEMORY_OWNER_SYSTEM)return false;*out=(epd_memory_owner_snapshot_t){0};return true;}
#include "input.inc"
bool epd_app_refresh_done(void){return display_idle;}
bool epd_app_reading_font_can_create(unsigned count){(void)count;return true;}
uint32_t epd_render_profile_begin(unsigned stage){(void)stage;return 0;}
void epd_render_profile_end(unsigned stage,uint32_t generation){(void)stage;(void)generation;}
void epd_service_ui_request(epd_service_t *service,uint32_t delay){CHECK(service==books_service && delay==16);++ui_requests;}
void epd_app_request_full_refresh(void){}
void epd_app_clean_draw_buffer(lv_draw_buf_t *b){(void)b;}
void ui_voice_input_open_for(lv_obj_t *o){voice_target=o;}
#include "view.inc"
void epd_app_object_init(lv_obj_t *o){epd_obj_init(o);}
lv_obj_t *epd_app_label(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *f){return label_create(p,s,x,y,w,f);}
bool text_update(lv_obj_t *o,const char *s){if(is_menu_chapter(o))++chapter_updates;lv_label_set_text(o,s);return true;}
bool epd_app_text(lv_obj_t *o,const char *s){return text_update(o,s);}
static bool storage_changing(void){return false;}
#include "clicked.inc"
lv_obj_t *button_create(lv_obj_t *p,int x,int y,int w,int h,const char *s,int target){
    lv_obj_t *b=epd_app_button(p,x,y,w,h,s,clicked,(void *)(intptr_t)target);
    if(state.focus_count<64)state.focus_items[state.focus_count++]=b;return b;
}
unsigned ui_settings_index(ui_setting_id_t s){return settings[s];}
bool ui_settings_set(ui_setting_id_t s,unsigned i){settings_changed|=s>=UI_SETTING_FONT_SIZE && s<=UI_SETTING_ENCODING && settings[s]!=i;settings[s]=i;return true;}
const char *ui_settings_value(ui_setting_id_t s){static char size[16];if(s==UI_SETTING_FONT_SIZE){snprintf(size,sizeof(size),"%upx",36+settings[s]*4);return size;}return s==UI_SETTING_FONT?"默认字体":s==UI_SETTING_FONT_WEIGHT?"标准":"10 次";}
bool storage_available(storage_volume_t v){return v!=STORAGE_SD || sd_available;}
size_t reading_font_list(reading_font_info_t *items,size_t cap){for(unsigned i=0;i<font_count && i<cap;++i){items[i]=(reading_font_info_t){.id=i,.available=true,.selected=i==0};snprintf(items[i].name,sizeof(items[i].name),i?"字体 %u":"默认字体",i);}return font_count;}
rt_err_t reading_font_select(uint32_t id,uint32_t *ticket){selected_font=id;*ticket=1;return RT_EOK;}
bool reading_font_job_status(uint32_t t,reading_font_status_t *s){*s=(reading_font_status_t){.ticket=t,.busy=font_busy,.phase=font_fail?READING_FONT_FAILED:READING_FONT_DONE};return true;}
bool epd_app_open_font_install(void){++installs;return true;}
bool epd_app_open_reading_fonts(void){++manages;return true;}
void frontlight_get(frontlight_snapshot_t *s){*s=(frontlight_snapshot_t){.available=true,.committed_percent=45};}
bool frontlight_commit(uint8_t p){return p<=100;}
const ui_reader_view_t *ui_reader_view(void){return &reading;}
bool ui_reader_waiting(void){return target_pending;}
const char *ui_reader_identity(void){return "/sdcard/books/四世同堂.epub";}
void ui_reader_turn(int d){++turn_calls;last_turn=d;navigation_event=++events;}
void ui_reader_seek(unsigned p){if(!document_active && !reading.ready)return;++seek_calls;last_seek=p;reading.page=p;navigation_event=++events;}
void ui_reader_background_pause(bool paused){background_paused=paused;}
bool ui_reader_settings_changed(void){return settings_changed;}
bool ui_reader_reflow(void){++reflows;reflow_event=++events;settings_changed=false;target_pending=true;reading.indexing=true;toc_count=0;last_seek=0;if(!document_active)reading.ready=false;return true;}
static ui_reader_work_t ui_reader_step(unsigned budget,bool foreground,bool commit){
    ++step_calls;last_step_budget=budget;last_step_foreground=foreground;last_step_commit=commit;
    return target_pending?READER_WORK_TARGET:READER_WORK_IDLE;
}
static int launcher_current_page(void){return current->id;}
static void reader_submit_poll(bool idle){(void)idle;}
static void reader_render(void){current->reader_revision=reading.revision;}
static void reader_update(void){reader_render();}
static void reader_status_render(void){}
static void reader_option_render(void){books_menu_process();}
bool document_view_active(void){return document_active;}
unsigned document_view_toc_count(void){return toc_count;}
bool document_view_toc_title(unsigned i,char *s,size_t n,unsigned *l){++toc_reads;
    if(realistic_toc){if(i>=sizeof(toc_fixture)/sizeof(toc_fixture[0]))return false;snprintf(s,n,"%s",toc_fixture[i].title);*l=toc_fixture[i].level;}
    else {snprintf(s,n,"第 %u 章",i+1);*l=i%4==1?1:0;}return true;}
unsigned epd_reader_toc_level(unsigned i){return realistic_toc?toc_fixture[i].level:i%4==1?1:0;}
bool document_view_toc_seek(unsigned i){if(navigation_fail || i>=toc_count)return false;++toc_seeks;last_seek=i;navigation_event=++events;return true;}
bool epd_reader_page_text(char *s,size_t n){snprintf(s,n,"%s",reading.text);return true;}
bool epd_reader_chapter_title(char *s,size_t n,unsigned *i){++chapter_calls;if(!chapter_title)return false;snprintf(s,n,"%s",chapter_title);if(i)*i=chapter_index;return true;}
bool epd_reader_chapter_move(int d){if(navigation_fail)return false;++chapter_moves;last_chapter=d;navigation_event=++events;return true;}
bool epd_reader_can_seek_progress(void){return can_seek_progress;}
bool epd_reader_seek_progress(unsigned percent){if(!can_seek_progress)return false;++progress_seeks;last_progress_seek=percent;return true;}
bool books_ai_init(epd_service_t *owner){(void)owner;strcpy(backend.model,"deepseek-v4-pro");return true;}
bool books_ai_bind(const char *id,const char *t,unsigned p,const char *s){(void)id;(void)t;(void)s;backend.page=p;return true;}
void books_ai_refresh_profiles(void){}
void books_ai_view(book_ai_view_t *v){*v=backend;}
void books_ai_cancel(void){++cancels;backend.busy=false;++backend.revision;}
void books_ai_auto_read(bool e){backend.auto_read=e;++backend.revision;}
bool books_ai_send(const char *s){if(!*s || backend.busy)return false;backend.busy=true;++backend.revision;return true;}
bool books_ai_retry(void){backend.busy=true;backend.status=BOOK_AI_WAITING;++backend.revision;return true;}
bool books_ai_message(unsigned i,bool a,char *s,size_t n){snprintf(s,n,"%s%u",a?"这是结合本页内容给出的回答。人物在家庭责任与时代变化之间犹豫。":"为什么人物会做出这样的选择？",i);return true;}
void books_ai_draft(const char *s){snprintf(draft,sizeof(draft),"%s",s);}
void books_ai_get_draft(char *s,size_t n){snprintf(s,n,"%s",draft);}
bool books_ai_select_service(void){return true;}
const char *epd_ai_error_name(epd_ai_error_t e){(void)e;return "network";}
const char *epd_speech_error_name(epd_speech_error_t e){(void)e;return "speech";}
#include "reader_controls.c"
static bool is_menu_chapter(lv_obj_t *object){return object && object==menu.chapter;}
#include "reader_ai_ui.c"
#include "resume.inc"
static void pump(void){
    host_tick+=400;lv_tick_inc(400);
    if(pending_page!=NAV_IDLE){int t=pending_page;pending_page=NAV_IDLE;books_menu_action(t);}
    books_menu_process();
    bool idle=display_idle,can_render=display_idle,runnable=false;
    bool modal=books_menu_active() || (current->foreground && current->popup && lv_obj_is_visible(current->popup));
    ui_reader_background_pause(modal || books_menu_font_pending());
#include "reader_process.inc"
    last_runnable=runnable;
    lv_obj_update_layout(state.screen);
}
static void choose(int target){books_menu_action(target);pump();}
static lv_indev_data_t pointer_data;
static void pointer_read(lv_indev_t *device,lv_indev_data_t *data){(void)device;*data=pointer_data;}
static void direct_display_touch(void){
    lv_indev_t *pointer=lv_indev_create();
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,pointer_read);
    lv_timer_pause(lv_indev_get_read_timer(pointer));
    for(unsigned point=0;point<3;++point){
        books_menu_close();books_menu_open();pump();
        CHECK(menu.tab==READER_MENU);
        lv_area_t area;lv_obj_get_coords(menu.tabs[3],&area);
        pointer_data.point=(lv_point_t){(area.x1+area.x2)/2,area.y1+(point==0?34:point==1?82:54)};
        pointer_data.state=LV_INDEV_STATE_PRESSED;lv_tick_inc(30);lv_indev_read(pointer);
        pointer_data.state=LV_INDEV_STATE_RELEASED;lv_tick_inc(30);lv_indev_read(pointer);
        CHECK(pending_page==DISPLAY);pump();
        CHECK(menu.tab==DISPLAY && menu.light && current->reader_panel);
    }
    lv_indev_delete(pointer);books_menu_close();
}
static void deferred_menu(void){
    books_menu_close();display_idle=false;unsigned requests=ui_requests;
    books_menu_open();
    CHECK(books_menu_active() && !current->reader_panel && ui_requests>requests);
    CHECK(books_menu_action(DISPLAY));
    display_idle=true;pump();
    CHECK(menu.tab==DISPLAY && menu.light && current->reader_panel);
    books_menu_close();books_menu_open();pump();
    display_idle=false;requests=ui_requests;choose(DISPLAY);
    CHECK(menu.dirty && !menu.light && ui_requests>requests);
    display_idle=true;pump();
    CHECK(menu.tab==DISPLAY && menu.light && !menu.dirty);
    books_menu_close();
}
static void deleted(lv_event_t *event){
    if(lv_event_get_target_obj(event)==lv_event_get_current_target_obj(event))++*(unsigned *)lv_event_get_user_data(event);
}
static void check_focus(void){
    input_scope_t *scope=scope_for_owner(current->reader_panel);CHECK(scope);
    if(!scope)return;
    CHECK(lv_obj_is_valid(lv_group_get_focused(scope->group)));
    CHECK(scope->back && lv_obj_is_valid(scope->back));
    for(unsigned i=0;i<current->focus_count;++i)CHECK(lv_obj_is_valid(current->focus_items[i]));
}
static void finish_layout(void){current->reader_reflow_pending=false;target_pending=false;reading.ready=true;toc_count=24;reading.indexing=false;}
static void unknown_pagination(void){
    reader_view_t saved=reading;
    finish_layout();books_menu_close();
    reading.page=reading.pages=0;reading.percent=60;reading.indexing=true;
    books_menu_open();choose(PROGRESS);
    CHECK(!strcmp(lv_label_get_text(menu.progress),"60%"));
    CHECK(lv_obj_has_state(menu.slider,LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(menu.jump_input,LV_STATE_DISABLED));
    CHECK(!lv_textarea_get_text(menu.jump_input)[0]);
    unsigned seeks=seek_calls,turns=turn_calls;
    lv_textarea_set_text(menu.jump_input,"80");choose(SEEK);
    CHECK(seek_calls==seeks && current->reader_panel);
    choose(PAGE_PREV);pump();
    CHECK(turn_calls==turns+1 && last_turn==-1 && !current->reader_panel);
    finish_layout();reading.percent=73;can_seek_progress=true;
    books_menu_open();pump();
    CHECK(!strcmp(lv_label_get_text(menu.progress),"73%"));
    CHECK(lv_slider_get_value(menu.slider)==73 && lv_slider_get_min_value(menu.slider)==0 &&
          lv_slider_get_max_value(menu.slider)==100 && !lv_obj_has_state(menu.slider,LV_STATE_DISABLED));
    choose(PROGRESS);
    unsigned progress_calls=progress_seeks;
    lv_obj_send_event(menu.slider,LV_EVENT_RELEASED,NULL);pump();
    CHECK(progress_seeks==progress_calls && current->reader_panel);
    lv_slider_set_value(menu.slider,41,LV_ANIM_OFF);
    lv_obj_send_event(menu.slider,LV_EVENT_VALUE_CHANGED,NULL);
    CHECK(!strcmp(lv_label_get_text(menu.progress),"41%") && progress_seeks==progress_calls);
    CHECK(!lv_textarea_get_text(menu.jump_input)[0]);
    lv_obj_send_event(menu.slider,LV_EVENT_RELEASED,NULL);pump();
    CHECK(progress_seeks==progress_calls+1 && last_progress_seek==41 && seek_calls==seeks && !current->reader_panel);
    can_seek_progress=false;
    reading=saved;finish_layout();
}
static void publish_target(void){reading.ready=true;target_pending=false;++reading.revision;pump();}
static void invalidated(lv_event_t *event){(void)event;++invalidations;}
static void chapter_refresh(void){
    unsigned initial_checks=checks,initial_failures=failures;
    reader_view_t saved=reading;
    unsigned size=settings[UI_SETTING_FONT_SIZE];
    const char *saved_title=chapter_title;
    finish_layout();books_menu_close();chapter_title=NULL;reading.page=18;
    books_menu_open();choose(TYPE);
    lv_obj_t *root=current->reader_panel,*head=menu.head,*dock=menu.dock,*chapter=menu.chapter,*sheet=menu.sheet;
    lv_group_t *group=epd_app_input_group(root);
    char fallback[64];snprintf(fallback,sizeof(fallback),"%s %u",BR(page),reading.page);
    CHECK(!strcmp(lv_label_get_text(chapter),fallback));
    lv_obj_update_layout(current->screen);lv_refr_now(NULL);
    unsigned updates=chapter_updates,requests=ui_requests,invalid=invalidations;
    for(unsigned i=0;i<32;++i){books_menu_process();lv_obj_update_layout(current->screen);lv_refr_now(NULL);}
    CHECK(chapter_updates==updates && invalidations==invalid && ui_requests==requests);
    CHECK(root==current->reader_panel && head==menu.head && dock==menu.dock && chapter==menu.chapter && sheet==menu.sheet);

    /* Metadata may arrive before the reader status revision is advanced. */
    unsigned revision=reading.revision;
    display_idle=false;chapter_title="迟到的章节";books_menu_process();
    CHECK(chapter_updates==updates && !strcmp(lv_label_get_text(chapter),fallback));
    CHECK(ui_requests==requests+1 && invalidations==invalid);
    chapter_title="最新章节";books_menu_process();
    CHECK(chapter_updates==updates && ui_requests==requests+2);
    display_idle=true;books_menu_process();
    CHECK(reading.revision==revision && !strcmp(lv_label_get_text(chapter),chapter_title));
    CHECK(chapter_updates==updates+1 && menu.tab==TYPE && menu.sheet==sheet && current->reader_panel==root);
    CHECK(group==epd_app_input_group(root));
    lv_obj_update_layout(current->screen);lv_refr_now(NULL);
    updates=chapter_updates;invalid=invalidations;requests=ui_requests;
    for(unsigned i=0;i<32;++i){++reading.revision;books_menu_process();lv_obj_update_layout(current->screen);lv_refr_now(NULL);}
    CHECK(chapter_updates==updates && invalidations==invalid && ui_requests==requests);

    current->foreground=false;chapter_title="恢复后的章节";books_menu_process();
    CHECK(chapter_updates==updates && ui_requests==requests);
    current->foreground=true;books_menu_process();
    CHECK(chapter_updates==updates+1 && !strcmp(lv_label_get_text(chapter),chapter_title));
    settings[UI_SETTING_FONT_SIZE]=3;choose(SIZE_UP);sheet=menu.sheet;
    CHECK(current->reader_reflow_pending && menu.tab==TYPE);
    chapter_title="重排后的章节";reading.page=37;publish_target();
    CHECK(!current->reader_reflow_pending && !strcmp(lv_label_get_text(chapter),chapter_title));
    CHECK(current->reader_panel==root && menu.sheet==sheet && menu.tab==TYPE && background_paused);
    chapter_title=NULL;reading.page=38;publish_target();
    snprintf(fallback,sizeof(fallback),"%s %u",BR(page),reading.page);
    CHECK(!strcmp(lv_label_get_text(chapter),fallback) && menu.sheet==sheet && menu.tab==TYPE);

    choose(PROGRESS);lv_textarea_set_text(menu.jump_input,"123");
    lv_obj_send_event(menu.jump_input,LV_EVENT_CLICKED,NULL);pump();
    lv_obj_t *keyboard=menu.keyboard,*input=menu.jump_input,*focus=lv_group_get_focused(group);
    sheet=menu.sheet;chapter_title="编辑页码时更新章节";books_menu_process();
    CHECK(!strcmp(lv_label_get_text(chapter),chapter_title));
    CHECK(menu.tab==PROGRESS && menu.sheet==sheet && menu.keyboard==keyboard && menu.jump_input==input);
    CHECK(!strcmp(lv_textarea_get_text(input),"123") && lv_group_get_focused(group)==focus);
    choose(AI);CHECK(!menu.chapter);
    unsigned calls=chapter_calls;books_menu_process();CHECK(chapter_calls==calls);
    books_menu_close();books_menu_process();CHECK(chapter_calls==calls);
    reading=saved;chapter_title=saved_title;settings[UI_SETTING_FONT_SIZE]=size;settings_changed=false;finish_layout();
    printf("Menu chapter refresh: %u checks, %u failures\n",checks-initial_checks,failures-initial_failures);
}
static void capture(const char *folder,const char *name);
static void realtime_gates(void){
    finish_layout();settings[UI_SETTING_FONT_SIZE]=3;settings_changed=false;
    books_menu_open();choose(TYPE);
    unsigned before=reflows,steps=step_calls;
    lv_obj_t *root=current->reader_panel;
    lv_obj_set_style_text_font(state.reader_body,fonts[2],0);
    display_idle=false;choose(SIZE_UP);
    CHECK(reflows==before && settings_changed && current->reader_panel==root);
    CHECK(last_runnable);
    CHECK(lv_obj_get_style_text_font(state.reader_body,0)==fonts[2]);
    CHECK(step_calls==steps+1 && last_step_budget==2 && last_step_foreground && !last_step_commit);
    display_idle=true;pump();
    CHECK(reflows==before+1 && !settings_changed && current->reader_reflow_pending);
    CHECK(last_step_foreground && last_step_commit && background_paused);
    publish_target();CHECK(reading.indexing && !current->reader_reflow_pending && current->reader_panel==root);
    current->popup=panel_create(current->screen,0,0,100,100);
    lv_obj_update_layout(state.screen);
    CHECK(ui_settings_set(UI_SETTING_FONT_SIZE,5));before=reflows;pump();
    CHECK(reflows==before && settings_changed && !last_step_foreground && background_paused);
    lv_obj_delete(current->popup);current->popup=NULL;pump();
    CHECK(reflows==before+1 && !settings_changed && last_step_foreground);
    publish_target();before=reflows;choose(CLOSE);
    CHECK(reflows==before && !current->reader_panel && !background_paused);
    finish_layout();
}

static void font_menu_regression(const char *folder){
    finish_layout();settings_changed=false;font_fail=false;font_busy=false;
    books_menu_open();choose(TYPE);choose(FONTS);CHECK(menu.tab==FONTS);
    capture(folder,"03-fonts");check_focus();
    unsigned installed=installs,managed=manages;
    choose(INSTALL);CHECK(installs==installed+1);
    sd_available=false;choose(MANAGE_FONTS);CHECK(manages==managed+1);
    choose(INSTALL);CHECK(installs==installed+1);sd_available=true;
    font_count=READING_FONT_LIMIT+1;menu.dirty=true;pump();
    CHECK(menu.font_count==READING_FONT_LIMIT+1);
    for(unsigned i=0;i<4;++i)choose(FONT_NEXT);
    CHECK(menu.font_first==20);font_busy=true;choose(FONT_BASE-4);
    CHECK(selected_font==24 && menu.font_ticket && !last_step_foreground);check_focus();
    unsigned before=reflows;
    font_busy=false;font_fail=true;pump();CHECK(!menu.font_ticket && menu.font_failed);
    CHECK(reflows==before && !strcmp(lv_label_get_text(menu.error),BR(font_failed)));
    font_count=1;font_fail=false;books_menu_close();finish_layout();
}

static void regression(const char *folder){
    finish_layout();settings[UI_SETTING_FONT_SIZE]=3;settings_changed=false;books_menu_open();pump();
    lv_obj_t *root=current->reader_panel,*head=menu.head,*dock=menu.dock;
    lv_group_t *group=epd_app_input_group(root);
    unsigned identities=next_identity,heads=chapter_updates,deleted_before=root_deletes;
    lv_obj_add_event_cb(root,deleted,LV_EVENT_DELETE,&root_deletes);
    lv_obj_add_event_cb(menu.sheet,deleted,LV_EVENT_DELETE,&sheet_deletes);
    lv_group_focus_obj(current->focus_items[current->focus_count-1]);
    choose(TYPE);CHECK(sheet_deletes==1);check_focus();
    CHECK(root==current->reader_panel && head==menu.head && dock==menu.dock);
    CHECK(group==epd_app_input_group(root) && identities==next_identity && chapter_updates==heads);
    unsigned before=reflows;
    choose(SIZE_UP);CHECK(reflows==before+1 && !settings_changed);
    choose(SIZE_UP);CHECK(reflows==before+2 && !settings_changed);
    choose(OPTION_BASE-UI_SETTING_MARGIN*16-2);
    CHECK(reflows==before+3 && background_paused && !settings_changed);
    CHECK(current->reader_panel==root && current->reader_reflow_pending && target_pending);
    CHECK(last_step_foreground && last_step_commit && last_step_budget==8);
    unsigned steps=step_calls;pump();CHECK(step_calls==steps+1 && reflows==before+3);
    publish_target();CHECK(!current->reader_reflow_pending && reading.indexing && books_menu_active());
    CHECK(background_paused && current->reader_panel==root);
    choose(PROGRESS);lv_obj_send_event(menu.jump_input,LV_EVENT_CLICKED,NULL);pump();
    lv_obj_add_event_cb(menu.keyboard,deleted,LV_EVENT_DELETE,&keyboard_deletes);
    toc_count=24;
    choose(TOC);CHECK(keyboard_deletes==1 && !menu.keyboard);check_focus();
    lv_obj_t *sheet=menu.sheet;unsigned reads=toc_reads,updates=chapter_updates;
    toc_count=40;pump();toc_count=48;pump();
    CHECK(menu.sheet==sheet && toc_reads==reads && chapter_updates==updates);
    CHECK(strstr(lv_label_get_text(menu.error),"48"));
    capture(folder,"11-toc-updated");
    choose(TOC_NEXT);CHECK(menu.toc_count==48 && menu.visible[0]==7);check_focus();
    CHECK(root_deletes==deleted_before && root==current->reader_panel && head==menu.head && dock==menu.dock);
    unsigned toc_before=toc_seeks;
    before=reflows;display_idle=false;
    CHECK(ui_settings_set(UI_SETTING_FONT_SIZE,ui_settings_index(UI_SETTING_FONT_SIZE)-1));
    choose(TOC_BASE-2);
    CHECK(!current->reader_panel && !background_paused && menu.navigation==TOC && menu.target==9);
    CHECK(reflows==before && toc_seeks==toc_before);
    display_idle=true;pump();CHECK(reflows==before+1 && menu.navigation==TOC && toc_seeks==toc_before);
    toc_count=5;pump();CHECK(menu.navigation==TOC && toc_seeks==toc_before);
    toc_count=10;reading.ready=false;pump();CHECK(!menu.navigation && toc_seeks==toc_before+1 && last_seek==9);
    reading.ready=true;
    finish_layout();
    /* Absolute jumps retain their number. Relative turns retain direction even
     * when reflow changes the displayed page number before dispatch. */
    for(unsigned i=0;i<3;++i){
        books_menu_open();choose(TYPE);before=reflows;choose(SIZE_DOWN);choose(PROGRESS);
        CHECK(reflows==before+1 && current->reader_reflow_pending && reading.indexing);
        reading.page=18;unsigned seeks=seek_calls,turns=turn_calls;
        display_idle=false;
        if(i==0){lv_textarea_set_text(menu.jump_input,"80");choose(SEEK);}
        else choose(i==1?PAGE_PREV:PAGE_NEXT);
        CHECK(!current->reader_panel && menu.navigation==(i==0?SEEK:i==1?PAGE_PREV:PAGE_NEXT));
        CHECK(seek_calls==seeks && turn_calls==turns);
        reading.page=37;display_idle=true;pump();
        CHECK(reflows==before+1 && navigation_event>reflow_event);
        if(i==0)CHECK(seek_calls==seeks+1 && last_seek==80 && turn_calls==turns);
        else {
            CHECK(turn_calls==turns+1 && last_turn==(i==1?-1:1));
            CHECK(seek_calls==seeks && reading.page==37);
        }
        pump();CHECK(turn_calls==turns+(i!=0) && seek_calls==seeks+(i==0));
        CHECK(!menu.navigation && !background_paused);finish_layout();
    }
    for(int direction=-1;direction<=1;direction+=2){
        books_menu_open();choose(TYPE);choose(SIZE_UP);choose(PROGRESS);
        unsigned moves=chapter_moves;choose(direction<0?CHAPTER_PREV:CHAPTER_NEXT);
        CHECK(menu.navigation && chapter_moves==moves);
        pump();CHECK(menu.navigation && chapter_moves==moves);
        publish_target();CHECK(!current->reader_reflow_pending && reading.indexing);
        pump();CHECK(!menu.navigation && chapter_moves==moves+1 && last_chapter==direction);
        CHECK(reading.indexing);finish_layout();
    }
    document_active=false;books_menu_open();choose(TYPE);choose(SIZE_DOWN);choose(PROGRESS);
    lv_textarea_set_text(menu.jump_input,"63");unsigned seeks=seek_calls;before=reflows;
    choose(SEEK);CHECK(reflows==before && !reading.ready && !settings_changed);
    CHECK(menu.navigation==SEEK && menu.target==63 && seek_calls==seeks);
    pump();CHECK(menu.navigation==SEEK && seek_calls==seeks);
    CHECK(last_step_foreground && last_step_commit);
    publish_target();CHECK(!menu.navigation && seek_calls==seeks+1 && last_seek==63);
    CHECK(reading.indexing && navigation_event>reflow_event && !current->reader_reflow_pending);
    for(int direction=-1;direction<=1;direction+=2){
        finish_layout();reading.page=18;books_menu_open();choose(TYPE);
        choose(direction<0?SIZE_UP:SIZE_DOWN);choose(PROGRESS);
        unsigned turns=turn_calls,seeks_before=seek_calls;
        choose(direction<0?PAGE_PREV:PAGE_NEXT);
        CHECK(menu.navigation==(direction<0?PAGE_PREV:PAGE_NEXT) && !reading.ready);
        pump();CHECK(turn_calls==turns && seek_calls==seeks_before);
        reading.page=37;publish_target();
        CHECK(!menu.navigation && turn_calls==turns+1 && last_turn==direction);
        CHECK(seek_calls==seeks_before && reading.page==37 && reading.indexing);
        pump();CHECK(turn_calls==turns+1);
    }
    document_active=true;finish_layout();
    books_menu_open();choose(FONTS);font_busy=true;choose(FONT_BASE);
    CHECK(menu.font_ticket && !strcmp(lv_label_get_text(menu.error),BR(font_wait)));
    CHECK(!last_step_foreground && background_paused);
    capture(folder,"12-font-wait");
    root=current->reader_panel;font_busy=false;font_fail=true;pump();
    CHECK(!menu.font_ticket && menu.tab==FONTS && root==current->reader_panel);
    CHECK(!strcmp(lv_label_get_text(menu.error),BR(font_failed)));check_focus();
    capture(folder,"13-font-failed");
    font_fail=false;font_busy=true;choose(FONT_BASE);before=reflows;
    font_busy=false;settings_changed=true;pump();
    CHECK(menu.tab==TYPE && root==current->reader_panel && reflows==before+1 && background_paused);
    CHECK(current->reader_reflow_pending && last_step_foreground && last_step_commit);
    choose(CLOSE);CHECK(reflows==before+1 && !background_paused);finish_layout();
    /* Closing while an asynchronous font selection runs retains the old
     * body font and prevents the shared reader from initiating its own reflow. */
    books_menu_open();choose(FONTS);font_busy=true;choose(FONT_BASE);
    settings_changed=true;before=reflows;
    lv_obj_set_style_text_font(state.reader_body,fonts[2],0);
    books_menu_close();CHECK(background_paused && books_menu_font_pending());
    pump();pump();CHECK(reflows==before && background_paused);
    CHECK(!last_step_foreground);
    CHECK(lv_obj_get_style_text_font(state.reader_body,0)==fonts[2]);
    font_busy=false;pump();CHECK(reflows==before+1 && !background_paused && !books_menu_font_pending());
    CHECK(lv_obj_get_style_text_font(state.reader_body,0)==fonts[0]);finish_layout();
    books_menu_open();choose(FONTS);font_busy=true;choose(FONT_BASE);
    books_menu_pause();books_menu_close();
    current->foreground=false;current->id=0;display_idle=false;
    font_busy=false;font_fail=true;before=reflows;
    books_menu_process();CHECK(!books_menu_font_pending() && menu.font_failed);
    pump();CHECK(!background_paused && !current->reader_panel && reflows==before);
    current->foreground=true;current->id=UI_PAGE_READER;display_idle=true;font_fail=false;
    /* A missing anchor after completed indexing is surfaced in the menu. */
    books_menu_open();choose(TOC);
    CHECK(ui_settings_set(UI_SETTING_FONT_SIZE,ui_settings_index(UI_SETTING_FONT_SIZE)-1));
    choose(TOC_BASE-2);
    CHECK(menu.navigation==TOC);reading.indexing=false;pump();
    CHECK(!menu.navigation && current->reader_panel && background_paused);
    CHECK(!strcmp(lv_label_get_text(menu.error),BR(open_failed)));books_menu_close();finish_layout();
    /* Empty TOC publication updates once, then keeps the displayed snapshot. */
    toc_count=0;books_menu_open();choose(TOC);CHECK(menu.toc_count==0);
    toc_count=4;pump();CHECK(menu.toc_count==4 && menu.visible[0]==0);
    finish_layout();books_menu_close();
    /* A pending open pauses immediately even while a refresh delays drawing. */
    display_idle=false;books_menu_open();CHECK(background_paused && books_menu_active() && !current->reader_panel);
    display_idle=true;pump();CHECK(current->reader_panel);choose(CLOSE);CHECK(!background_paused);
    current->popup=panel_create(current->screen,0,0,100,100);
    lv_obj_update_layout(state.screen);
    books_menu_open();books_menu_close();CHECK(background_paused);
    lv_obj_delete(current->popup);current->popup=NULL;books_menu_close();CHECK(!background_paused);
}
static void capture(const char *folder,const char *name){
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);lv_obj_update_layout(state.screen);
    lv_draw_buf_t *b=lv_snapshot_take(state.screen,LV_COLOR_FORMAT_RGB888);REQUIRE(b);
    FILE *f=fopen(path,"wb");REQUIRE(f);fprintf(f,"P6\n684 1216\n255\n");
    for(unsigned y=0;y<1216;++y)for(unsigned x=0;x<684;++x){const uint8_t *p=b->data+y*b->header.stride+x*3;uint8_t rgb[]={p[2],p[1],p[0]};fwrite(rgb,1,3,f);}
    fclose(f);lv_draw_buf_destroy(b);
}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){(void)a;(void)p;lv_display_flush_ready(d);}
static void toc_regression(const char *folder){
    reader_view_t saved=reading;const char *saved_chapter=chapter_title;
    finish_layout();books_menu_close();realistic_toc=true;
    toc_count=sizeof(toc_fixture)/sizeof(toc_fixture[0]);chapter_index=14;
    chapter_title=toc_fixture[14].title;reading.title="一往无前";
    books_menu_open();choose(TOC);
    CHECK(menu.first==14 && menu.visible[0]==14 && menu.visible_count==22);
    lv_obj_t *entry=lv_obj_get_child(menu.content,0);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(entry,0),lv_color_black()));
    check_focus();capture(folder,"20-toc-current");
    choose(TOC_PREV);choose(TOC_PREV);CHECK(menu.first==0);
    entry=lv_obj_get_child(menu.content,2);
    lv_obj_t *caption=lv_obj_get_child(lv_obj_get_child(entry,0),0);
    lv_area_t text_bounds,row_bounds;
    lv_obj_get_coords(caption,&text_bounds);lv_obj_get_coords(entry,&row_bounds);
    CHECK(lv_obj_get_height(caption)>ui_font_body()->line_height);
    CHECK(text_bounds.x1>=row_bounds.x1+16 && text_bounds.x2<row_bounds.x2);
    CHECK(text_bounds.y1>=row_bounds.y1 && text_bounds.y2<=row_bounds.y2);
    capture(folder,"21-toc-frontmatter");
    choose(TOC_FOLD-4);CHECK(toc_folded(4) && menu.visible_count==18 && menu.visible[5]==9);
    choose(TOC_FOLD-5);CHECK(toc_folded(9) && menu.visible_count==12);
    choose(TOC_CURRENT);CHECK(toc_folded(4) && !toc_folded(9));
    CHECK(menu.first==7 && menu.visible[3]==14 && menu.visible_count==18);
    capture(folder,"22-toc-folds");
    choose(TOC_NEXT);CHECK(menu.first==14 && menu.visible[4]==UINT_MAX);
    unsigned seeks=toc_seeks;choose(TOC_BASE-6);CHECK(toc_seeks==seeks && books_menu_active());
    choose(TOC_NEXT);CHECK(menu.first==14);
    lv_area_t footer,error;lv_obj_get_coords(lv_obj_get_child(menu.content,5),&footer);lv_obj_get_coords(menu.error,&error);
    CHECK(footer.y2<error.y1);
    choose(TOC_PREV);choose(TOC_BASE-3);
    CHECK(toc_seeks==seeks+1 && last_seek==14 && !books_menu_active());
    books_menu_open();choose(TOC);CHECK(menu.first==14);
    display_idle=false;lv_obj_t *sheet=menu.sheet;choose(TOC_PREV);
    CHECK(menu.dirty && menu.sheet==sheet);display_idle=true;pump();CHECK(menu.first==7 && !menu.dirty);
    books_menu_close();toc_count=0;navigation_status=0;
    books_menu_open();choose(TOC);CHECK(menu.toc_locate && !menu.toc_count);
    toc_count=6;pump();CHECK(menu.first==0 && menu.toc_locate);
    toc_count=22;navigation_status=1;pump();CHECK(menu.first==14 && !menu.toc_locate);
    books_menu_close();realistic_toc=false;chapter_index=0;chapter_title=saved_chapter;reading=saved;finish_layout();
}
int main(int argc,char **argv){
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);_set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    for(int i=_CRT_WARN;i<=_CRT_ASSERT;++i){_CrtSetReportMode(i,_CRTDBG_MODE_FILE);_CrtSetReportFile(i,_CRTDBG_FILE_STDERR);}
    REQUIRE(argc==3 || argc==4);lv_init();FILE *f=fopen(argv[2],"rb");REQUIRE(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);void *data=malloc(size);REQUIRE(fread(data,1,size,f)==(size_t)size);fclose(f);
    int sizes[]={28,24,36,20,64};for(unsigned i=0;i<5;++i){fonts[i]=lv_tiny_ttf_create_data_ex(data,size,sizes[i],LV_FONT_KERNING_NONE,0);REQUIRE(fonts[i]);}
    lv_style_init(&epd_style);lv_style_set_anim_duration(&epd_style,0);
    lv_display_t *display=lv_display_create(684,1216);_Alignas(32) static uint32_t pixels[684*1216];lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(display,flush);
    lv_display_add_event_cb(display,invalidated,LV_EVENT_INVALIDATE_AREA,NULL);
    state.screen=lv_screen_active();state.id=UI_PAGE_READER;state.foreground=true;epd_obj_init(state.screen);lv_obj_set_style_bg_color(state.screen,lv_color_white(),0);lv_obj_set_style_bg_opa(state.screen,LV_OPA_COVER,0);
    label_create(state.screen,"四世同堂",24,12,310,fonts[1]);label_create(state.screen,"第一部 · 惶惑",364,12,296,fonts[1]);
    label_create(state.screen,"瑞宣立在门口，望着熟悉的胡同。\n\n他想起祖父，也想起孩子们。\n\n生活仍在继续，可一切又都不同了。",32,76,620,fonts[2]);
    state.reader_body=label_create(state.screen,"",0,0,1,fonts[0]);
    if(argc==4 && !strcmp(argv[3],"--navigation-only")) {
        toc_count=0;navigation_status=0;books_menu_open();pump();
        CHECK(!navigation_active);
        choose(TOC);CHECK(navigation_active);
        toc_count=2;pump();CHECK(menu.toc_count==2 && menu.visible[1]==1);
        toc_count=6;pump();CHECK(menu.visible[1]==1);
        navigation_status=1;pump();CHECK(menu.toc_count==6);
        choose(DISPLAY);CHECK(!navigation_active);
        toc_count=0;navigation_status=0;choose(TOC);CHECK(navigation_active);
        navigation_status=-1;pump();CHECK(menu.toc_count==0);
        books_menu_close();CHECK(!navigation_active);
        printf("Navigation menu: %u checks, %u failures\n",checks,failures);return failures?1:0;
    }
    if(argc==4 && !strcmp(argv[3],"--font-only")) {
        font_menu_regression(argv[1]);
        printf("Font menu: %u checks, %u failures\n",checks,failures);return failures?1:0;
    }
    direct_display_touch();deferred_menu();
    books_menu_open();pump();CHECK(current->reader_panel);capture(argv[1],"01-menu");
    choose(TYPE);capture(argv[1],"02-type");choose(OPTION_BASE-UI_SETTING_LINE_SPACING*16-3);CHECK(settings[UI_SETTING_LINE_SPACING]==3);
    choose(OPTION_BASE-UI_SETTING_MARGIN*16-4);CHECK(settings[UI_SETTING_MARGIN]==4);
    choose(FONTS);CHECK(menu.tab==FONTS);capture(argv[1],"03-fonts");choose(INSTALL);CHECK(installs==1);choose(BOOKS_MENU_BACK);CHECK(menu.tab==TYPE);
    choose(PROGRESS);lv_slider_set_value(menu.slider,80,LV_ANIM_OFF);lv_obj_send_event(menu.slider,LV_EVENT_VALUE_CHANGED,NULL);CHECK(seek_calls==0);lv_obj_send_event(menu.slider,LV_EVENT_RELEASED,NULL);pump();CHECK(seek_calls==1 && last_seek==80 && !current->reader_panel);
    finish_layout();
    books_menu_open();choose(PROGRESS);lv_textarea_set_text(menu.jump_input,"429");choose(SEEK);CHECK(current->reader_panel && seek_calls==1);lv_obj_send_event(menu.jump_input,LV_EVENT_CLICKED,NULL);pump();capture(argv[1],"04-page-keyboard");
    lv_area_t input,key;lv_obj_get_coords(menu.jump_input,&input);lv_obj_get_coords(menu.keyboard,&key);CHECK(input.y2<key.y1);choose(BOOKS_MENU_BACK);CHECK(!lv_obj_is_visible(menu.keyboard));CHECK(!strcmp(lv_textarea_get_text(menu.jump_input),"429"));
    choose(TOC);capture(argv[1],"05-toc");CHECK(menu.visible[1]==1);choose(TOC_FOLD);CHECK(menu.visible[1]==2);choose(TOC_CURRENT);CHECK(menu.visible[1]==2);choose(TOC_FOLD);CHECK(menu.visible[1]==1);
    choose(DISPLAY);capture(argv[1],"06-display");choose(SLEEP);capture(argv[1],"07-sleep");choose(BOOKS_MENU_BACK);CHECK(menu.tab==DISPLAY);
    choose(REFRESH);choose(OPTION_BASE-UI_SETTING_FULL_REFRESH*16-2);CHECK(menu.tab==DISPLAY && settings[UI_SETTING_FULL_REFRESH]==2);
    choose(TYPE);choose(WEIGHT);choose(OPTION_BASE-UI_SETTING_FONT_WEIGHT*16-1);CHECK(menu.tab==TYPE && settings[UI_SETTING_FONT_WEIGHT]==1);
    publish_target();CHECK(!ui_reader_waiting() && reading.indexing);
    choose(AI);CHECK(ui.input);capture(argv[1],"08-ai");lv_textarea_set_text(ui.input,"为什么他会这样想？");lv_obj_send_event(ui.input,LV_EVENT_CLICKED,NULL);pump();capture(argv[1],"09-ai-keyboard");
    lv_obj_get_coords(ui.input,&input);lv_obj_get_coords(ui.keyboard,&key);CHECK(input.y2<key.y1);lv_obj_send_event(ui.keyboard,LV_EVENT_CANCEL,NULL);CHECK(!strcmp(draft,"为什么他会这样想？"));CHECK(!ui.keyboard_visible);
    choose(AI_VOICE);CHECK(voice_target==ui.input);choose(BOOKS_MENU_BACK);CHECK(ui.root && !ui.keyboard_visible);choose(AI_SEND);CHECK(backend.busy && !lv_textarea_get_text(ui.input)[0]);choose(AI_READ);CHECK(backend.auto_read);
    backend.count=3;backend.busy=false;++backend.revision;pump();CHECK(ui.count==3);CHECK(lv_label_get_text(ui.answers[0])[0]);capture(argv[1],"10-ai-chat");
    backend.count=2;++backend.generation;++backend.revision;pump();CHECK(ui.count==2 && lv_label_get_text(ui.answers[0])[0]);
    lv_textarea_set_text(ui.input,"未发送的草稿");choose(BOOKS_MENU_BACK);CHECK(!current->reader_panel && !strcmp(draft,"未发送的草稿"));CHECK(cancels>0);
    choose(BOOKS_MENU_AI);CHECK(!strcmp(lv_textarea_get_text(ui.input),"未发送的草稿"));books_menu_pause();pump();CHECK(ui.input && !strcmp(lv_textarea_get_text(ui.input),"未发送的草稿"));books_menu_close();
    puts("regression");regression(argv[1]);
    puts("realtime_gates");realtime_gates();
    puts("font_menu_regression");font_menu_regression(argv[1]);
    puts("unknown_pagination");unknown_pagination();
    puts("chapter_refresh");chapter_refresh();
    puts("toc_regression");toc_regression(argv[1]);
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
