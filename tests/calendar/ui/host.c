#include "lvgl.h"
#include "runtime.h"
#include "platform/epd_keyboard.h"
#include "epd_file_picker.h"
#include "epd_date_picker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <stdarg.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static char last_log[256];
static void rt_kprintf(const char *format,...){va_list args;va_start(args,format);vsnprintf(last_log,sizeof(last_log),format,args);va_end(args);}
enum { W=684,H=1216 };
typedef enum { EPD_FONT_BODY,EPD_FONT_SMALL,EPD_FONT_TITLE,EPD_FONT_CAPTION,EPD_FONT_TEMPERATURE } epd_font_role_t;
typedef struct {int16_t timezone_minutes;bool automatic,hour24;} system_time_settings_t;
static lv_font_t *fonts[5];static lv_style_t epd_style;static unsigned scale=100,scale_revision=1;static bool english,nav;
static bool automatic=false,hour24=true;
static uint8_t frame[W*H*4];static cal_book_t sample;static cal_status_t backend;static unsigned back_count;static lv_obj_t *back_button;
static unsigned query_calls,query_sequence;static bool defer_query;
static int epd_ui_px(int n){return (n*(int)scale+(n<0?-50:50))/100;}
static int epd_ui_touch(int n){return LV_MAX(56,epd_ui_px(n));}
static unsigned epd_ui_scale_revision(void){return scale_revision;}
static unsigned epd_locale_revision(void){return english?2:1;}
static const char *epd_locale_get(void){return english?"en_us":"zh_cn";}
static void epd_ui_content_area(lv_obj_t *s,lv_area_t *a){*a=(lv_area_t){0,epd_ui_px(174),W-1,H-epd_ui_px(nav?84:32)-1};}
static int64_t system_time_get_utc(void){return (int64_t)epd_date_day((epd_date_t){2026,10,30})*86400+2*3600;}
static int32_t system_time_local_day(int64_t t){return epd_date_utc_day(t,480);}
static void system_time_get_settings(system_time_settings_t *s){*s=(system_time_settings_t){480,automatic,hour24};}
static bool system_time_format(int64_t t,const char *locale,bool with_date,char *out,size_t n){int32_t d=epd_date_utc_day(t,480);epd_date_t date=epd_date_from_day(d);int min=(int)((t+28800-(int64_t)d*86400)/60);if(with_date)snprintf(out,n,"%d/%02u/%02u %02d:%02d",date.year,date.month,date.day,min/60,min%60);else snprintf(out,n,"%02d:%02d",min/60,min%60);return true;}
static void gui_app_goback(void){++back_count;}
static const lv_font_t *epd_app_font_role(epd_font_role_t r){return fonts[r];}
static const lv_font_t *ui_font_body(void){return fonts[0];}
static lv_group_t *input_group;
static void epd_input_add_object(lv_obj_t *o){if(!input_group)input_group=lv_group_create();lv_group_add_obj(input_group,o);}
static void epd_app_input_group(lv_obj_t *o){}
static void epd_app_input_back(lv_obj_t *o,lv_obj_t *b){back_button=b;}
static bool text_update(lv_obj_t *o,const char *s){lv_label_set_text(o,s);return true;}
#include "../../notes/ui/platform.inc"
static void epd_app_object_init(lv_obj_t *o){epd_obj_init(o);}
static lv_obj_t *epd_app_label(lv_obj_t *p,const char *t,int x,int y,int w,const lv_font_t *f){return label_create(p,t,x,y,w,f);}
static bool epd_app_text(lv_obj_t *label,const char *value){if(strcmp(lv_label_get_text(label),value))lv_label_set_text(label,value);return true;}
static void epd_app_header_refresh(lv_obj_t *s){}
static bool epd_reminder_available(void){return true;}
lv_obj_t *epd_keyboard_create(lv_obj_t *p,lv_obj_t *t,const epd_keyboard_config_t *c){lv_obj_t*k=lv_keyboard_create(p);lv_keyboard_set_textarea(k,t);return k;}
void epd_keyboard_set_textarea(lv_obj_t*k,lv_obj_t*t){lv_keyboard_set_textarea(k,t);}
int32_t epd_keyboard_preferred_height(lv_obj_t*k){return epd_ui_px(360);}
void epd_keyboard_refresh(lv_obj_t*k){}
bool epd_file_picker_open(lv_obj_t *o,const char *e,epd_file_selected_t f,void*c){return true;}
void epd_file_picker_cancel(lv_obj_t *o){}
bool cal_runtime_start(void){return true;}
bool cal_runtime_draft_saved(void){return true;}
void cal_runtime_status(cal_status_t *s){*s=backend;}
bool cal_runtime_event(uint32_t id,cal_event_t *e){cal_event_t*f=cal_find(&sample,id);return f&&cal_event_copy(e,f);}
uint32_t cal_runtime_query(int32_t first,int32_t last,int tz,const char *q,int kind,size_t offset,unsigned fields){++query_calls;uint32_t revision=++query_sequence;if(defer_query)return revision;cal_occurrence_t rows[48];backend.count=cal_query(&sample,first,last,tz,q,kind,rows,48,offset);for(size_t i=0;i<backend.count;++i){cal_event_t*e=cal_find(&sample,rows[i].id);backend.rows[i].occurrence=rows[i];strcpy(backend.rows[i].title,e->title);backend.rows[i].duration=e->duration_minutes;backend.rows[i].kind=e->kind;}backend.query_revision=revision;++backend.data_revision;++backend.revision;return revision;}
bool cal_runtime_draft(cal_event_t*e,int32_t*d,bool*i){return false;}
static cal_command_t submitted;
bool cal_runtime_submit(cal_command_t c,const cal_event_t *e,uint32_t id,int32_t d,bool i,const char *p,const cal_book_t *b){submitted=c;return true;}
#include "picker.inc"
static bool fail_ui_alloc;
static unsigned failed_ui_allocations;
static void *ui_alloc(size_t size){if(fail_ui_alloc){++failed_ui_allocations;return NULL;}return cal_alloc(size);}
#define cal_alloc ui_alloc
#include "ui.inc"
#undef cal_alloc

static void draft_input_failures(void)
{
 page=cal_alloc(sizeof(*page));CHECK(page);page->screen=lv_screen_active();page->width=W;page->height=H;
 page->mode=EDIT;page->draft.notes=cal_alloc(4);CHECK(page->draft.notes);strcpy(page->draft.notes,"old");
 char *old=page->draft.notes;fail_ui_alloc=true;text_editor(2);
 CHECK(failed_ui_allocations==1&&!page->input&&!page->notes_input&&page->draft.notes==old&&!strcmp(old,"old"));
 fail_ui_alloc=false;text_editor(2);CHECK(page->input&&page->notes_input);
 char *reserved=page->notes_input;lv_textarea_set_text(page->input,"最新备注，关闭时保留");
 fail_ui_alloc=true;CHECK(!field_apply()&&page->draft.notes==old);field_close();
 CHECK(page->draft.notes==reserved&&!page->notes_input);modal_close();
 CHECK(!page->input&&!strcmp(page->draft.notes,"最新备注，关闭时保留"));
 fail_ui_alloc=false;
 cal_event_t restored={0};CHECK(cal_event_copy(&restored,&page->draft));
 CHECK(restored.notes!=page->draft.notes&&!strcmp(restored.notes,page->draft.notes));cal_event_clear(&restored);
 /* The reserve also covers the textarea's maximum four-byte UTF-8 input. */
 text_editor(2);CHECK(page->input&&page->notes_input);reserved=page->notes_input;
 char maximum[NOTES_INPUT_BYTES+5];for(unsigned i=0;i<NOTES_INPUT_CHARS+1;++i)memcpy(maximum+i*4,"\xf0\x9f\x99\x82",4);
 maximum[NOTES_INPUT_BYTES+4]=0;lv_textarea_set_text(page->input,maximum);
 CHECK(strlen(lv_textarea_get_text(page->input))==NOTES_INPUT_BYTES);
 fail_ui_alloc=true;field_close();modal_close();fail_ui_alloc=false;
 CHECK(page->draft.notes==reserved&&strlen(page->draft.notes)==NOTES_INPUT_BYTES);
 CHECK(!memcmp(page->draft.notes,maximum,NOTES_INPUT_BYTES));
 cal_event_clear(&page->draft);cal_free(page);page=NULL;lv_obj_clean(lv_screen_active());
 puts("PASS calendar native textarea: reserve rejection, persistent OOM, detached draft ownership and 2000 four-byte characters");
}

#define MARGIN epd_ui_px(32)
#define PAGE_CONTENT_Y epd_ui_px(174)
#define SETTINGS_TITLE_X epd_ui_px(91)
#define SETTINGS_TITLE_Y epd_ui_px(76)
#define LV_EXT_STR_ID(x) #x
#define UI_PAGE_TIME_EDIT 1
#define NAV_IDLE 0
#define NAV_BACK 2
static struct {bool foreground;int id;lv_obj_t *popup,*screen;void *feature;} setting_page;
static int pending_page;
#define current (&setting_page)
static int ui_shell_content_bottom(void){return H-32;}
static const lv_font_t *ui_font_settings_title(void){return fonts[2];}
static const lv_font_t *ui_font_settings_body(void){return fonts[0];}
static const lv_font_t *ui_font_settings_value(void){return fonts[1];}
static lv_obj_t *page_title_create(lv_obj_t*s,const char*id,const lv_font_t*f){return label_create(s,"日期与时间",91,76,500,f);}
static lv_obj_t *button_create_i18n(lv_obj_t*p,int x,int y,int w,int h,const char*id,int v){return epd_app_button(p,x,y,w,h,v==NAV_BACK?"取消":"保存",NULL,NULL);}
static unsigned clock_writes,setting_errors;static struct tm written;
static bool system_time_get_local(struct tm*t){*t=(struct tm){.tm_year=126,.tm_mon=9,.tm_mday=30,.tm_hour=3,.tm_min=44};return true;}
static bool system_time_set_local(const struct tm*t){if(automatic)return false;written=*t;++clock_writes;return true;}
static void popup_open_i18n(const char*id,void*p){++setting_errors;}
#include "time_settings.inc"
#undef current

static void click(action_t a,int v){binding_t b={a,v};lv_obj_t*o=lv_obj_create(page->screen);lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,&b);lv_obj_send_event(o,LV_EVENT_CLICKED,NULL);lv_obj_delete(o);if(page->need_build&&!page->modal)rebuild();}
static void flush(lv_display_t*d,const lv_area_t*a,uint8_t*p)
{
 unsigned stride=lv_display_get_buf_active(d)->header.stride;
 for(int y=a->y1;y<=a->y2;++y){uint8_t *src=p+(y-a->y1)*stride,*dst=frame+(y*W+a->x1)*4;
  if(lv_display_get_color_format(d)==LV_COLOR_FORMAT_RGB565){for(int x=0;x<=a->x2-a->x1;++x){uint16_t v;memcpy(&v,src+x*2,2);dst[x*4]=(v&31)*255/31;dst[x*4+1]=((v>>5)&63)*255/63;dst[x*4+2]=(v>>11)*255/31;dst[x*4+3]=255;}}
  else memcpy(dst,src,(a->x2-a->x1+1)*4);
 }lv_display_flush_ready(d);
}
static void capture(const char *dir,const char *name){lv_obj_update_layout(page->screen);lv_refr_now(NULL);char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n%d %d\n255\n",W,H);for(int i=0;i<W*H;++i){fputc(frame[i*4+2],f);fputc(frame[i*4+1],f);fputc(frame[i*4],f);}fclose(f);}
static lv_indev_data_t pointer_data;
static lv_indev_data_t key_data;
static void key_read(lv_indev_t *indev,lv_indev_data_t *data){(void)indev;*data=key_data;}
static void send_key(lv_indev_t *indev,uint32_t key){key_data.key=key;key_data.state=LV_INDEV_STATE_PRESSED;lv_tick_inc(30);lv_indev_read(indev);key_data.state=LV_INDEV_STATE_RELEASED;lv_tick_inc(30);lv_indev_read(indev);}
static void pointer_read(lv_indev_t *indev,lv_indev_data_t *data){(void)indev;*data=pointer_data;}
static void drag_up(lv_indev_t *indev,lv_obj_t *obj)
{lv_obj_update_layout(page->screen);lv_area_t a;lv_obj_get_coords(obj,&a);pointer_data.point=(lv_point_t){(a.x1+a.x2)/2,(a.y1+a.y2)/2};pointer_data.state=LV_INDEV_STATE_PRESSED;lv_tick_inc(30);lv_indev_read(indev);
 for(int i=0;i<4;++i){pointer_data.point.y-=18;lv_tick_inc(30);lv_indev_read(indev);}pointer_data.state=LV_INDEV_STATE_RELEASED;lv_tick_inc(30);lv_indev_read(indev);}
static bool label_has(lv_obj_t*o,const char*t){if(lv_obj_check_type(o,&lv_label_class)&&strstr(lv_label_get_text(o),t))return true;for(unsigned i=0;i<lv_obj_get_child_count(o);++i)if(label_has(lv_obj_get_child(o,i),t))return true;return false;}
static void check_home_bounds(void)
{
 lv_obj_update_layout(page->screen);lv_area_t root,body,tabs,area;lv_obj_get_coords(page->root,&root);lv_obj_get_coords(page->body,&body);lv_obj_get_coords(lv_obj_get_child(page->root,-1),&tabs);epd_ui_content_area(page->screen,&area);
 CHECK(root.y1==px(64)&&root.y2==area.y2);CHECK(tabs.y2==area.y2);CHECK(body.y2+px(8)+1==tabs.y1);
 CHECK(body.y1==root.y1+epd_ui_touch(56)+px(16));CHECK(lv_obj_get_height(back_button)>=56);
}
static void *draw_buffer(uint32_t size,lv_color_format_t format){void *raw=calloc(1,size+LV_DRAW_BUF_ALIGN);CHECK(raw);return lv_draw_buf_align(raw,format);}
static unsigned shell_deletes;
static void shell_deleted(lv_event_t *e){++shell_deletes;}
static void check_view_switches(void)
{
 lv_obj_t *root=page->root,*back=back_button,*tabs[4];
 lv_obj_add_event_cb(root,shell_deleted,LV_EVENT_DELETE,NULL);
 for(int i=0;i<4;++i){tabs[i]=page->view_tabs[i];lv_obj_add_event_cb(tabs[i],shell_deleted,LV_EVENT_DELETE,NULL);}
 page->minute=system_time_get_utc()/60;page->last_day=system_time_local_day(system_time_get_utc());page->last_timezone=480;page->hour24=true;
 page->timer=lv_timer_create(poll,200,NULL);lv_tick_inc(1);
 for(int i=0;i<4;++i){unsigned before=query_calls;
  lv_timer_reset(page->timer);lv_obj_send_event(tabs[i],LV_EVENT_CLICKED,NULL);
  CHECK(page->need_build);lv_timer_handler();
  CHECK(page->built_view==i&&!page->need_build);CHECK(page->root==root&&back_button==back&&!shell_deletes);
  CHECK(query_calls==before+(i!=0));
  for(int j=0;j<4;++j){CHECK(page->view_tabs[j]==tabs[j]);CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(tabs[j],0))==lv_color_to_u32(lv_color_hex(j==i?0x242424:0xffffff)));}
  before=query_calls;lv_obj_send_event(tabs[i],LV_EVENT_CLICKED,NULL);CHECK(!page->need_build&&query_calls==before);
  check_home_bounds();
 }
 defer_query=true;lv_obj_send_event(tabs[1],LV_EVENT_CLICKED,NULL);lv_timer_handler();
 CHECK(page->built_view==1&&!rows_ready()&&!label_has(page->list,"项目评审"));
 lv_obj_t *cell=page->grid_cells[0];uint32_t requested=page->query_revision;
 ++backend.data_revision;++backend.revision;poll(NULL);
 CHECK(page->grid_cells[0]==cell&&!rows_ready()&&!label_has(page->list,"项目评审"));
 backend.query_revision=requested;++backend.data_revision;++backend.revision;poll(NULL);
 CHECK(page->grid_cells[0]==cell&&label_has(page->list,"项目评审")&&!shell_deletes);
 defer_query=false;
 lv_timer_delete(page->timer);page->timer=NULL;
 lv_obj_remove_event_cb(root,shell_deleted);for(int i=0;i<4;++i)lv_obj_remove_event_cb(tabs[i],shell_deleted);
 puts("calendar view switches: next-loop rendering, persistent header/tabs, unchanged-tab no-op and stale results passed");
}
int main(int argc,char**argv){CHECK(argc==3||argc==4);bool partial=argc==4;FILE*f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);void*data=malloc(size);CHECK(fread(data,1,size,f)==size);fclose(f);
 lv_init();int sizes[]={28,24,36,20,64};for(int i=0;i<5;++i){fonts[i]=lv_tiny_ttf_create_data_ex(data,size,sizes[i],LV_FONT_KERNING_NONE,256);CHECK(fonts[i]);}
 lv_style_init(&epd_style);lv_style_set_anim_duration(&epd_style,0);lv_display_t*d=lv_display_create(W,H);lv_color_format_t format=partial?LV_COLOR_FORMAT_RGB565:LV_COLOR_FORMAT_XRGB8888;lv_display_set_color_format(d,format);uint32_t buffer_size=partial?W*40*2:W*H*4;void*buffer=draw_buffer(buffer_size,format),*buffer2=partial?draw_buffer(buffer_size,format):NULL;lv_display_set_buffers(d,buffer,buffer2,buffer_size,partial?LV_DISPLAY_RENDER_MODE_PARTIAL:LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(d,flush);lv_indev_t *touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);lv_indev_set_display(touch,d);lv_indev_set_read_cb(touch,pointer_read);
 if(argc==4&&!strcmp(argv[3],"draft-failures")){draft_input_failures();return 0;}
 cal_book_init(&sample);sample.first_weekday=1;cal_event_t e={0};e.rule.start_day=epd_date_day((epd_date_t){2026,10,30});e.rule.until_day=epd_date_day((epd_date_t){2099,12,31});e.rule.timezone_minutes=480;e.rule.minute=9*60;e.duration_minutes=60;e.reminder_minutes=10;strcpy(e.title,"项目评审 · 十月版本");strcpy(e.uid,"fixture@epd");e.notes="核对本轮功能、交互与验收结果。\n确定下一阶段安排。";CHECK(cal_put(&sample,&e));
 backend.ready=true;backend.preferences=sample;backend.preferences.events=NULL;backend.preferences.count=0;
 page=cal_alloc(sizeof(*page));page->screen=lv_screen_active();screen_init(page->screen);page->selected=(epd_date_t){2026,10,30};page->filter=-1;page->view=1;page->prefs_loaded=true;page->status=backend;request_rows();page->status=backend;rebuild();capture(argv[2],"month");CHECK(label_has(page->body,e.title));
 check_view_switches();
 for(int i=0;i<4;++i){click(VIEW,i);page->status=backend;rebuild();const char*names[]={"year","month","week","day"};capture(argv[2],names[i]);check_home_bounds();}
 if(partial){
  /* Both draw buffers can contain a previously rendered footer chunk. */
  lv_area_t footer;lv_obj_get_coords(lv_obj_get_child(page->root,-1),&footer);
  for(int y=0;y<40;++y)for(int x=0;x<W;++x){uint8_t *p=frame+((footer.y1+y)*W+x)*4;uint16_t v=((p[2]>>3)<<11)|((p[1]>>2)<<5)|(p[0]>>3);((uint16_t*)buffer)[y*W+x]=v;((uint16_t*)buffer2)[y*W+x]=v;}
  lv_area_t top_gap={0,48,W-1,63};lv_obj_invalidate_area(page->screen,&top_gap);capture(argv[2],"partial-top-reuse");
  unsigned stale=0;for(int y=top_gap.y1;y<=top_gap.y2;++y)for(int x=0;x<W;++x){uint8_t *p=frame+(y*W+x)*4;if(p[0]!=255||p[1]!=255||p[2]!=255)++stale;}
  printf("partial RGB565 reused-buffer top band: %u stale pixels\n",stale);CHECK(stale==0);
  CHECK(lv_obj_get_style_bg_opa(page->screen,0)==LV_OPA_COVER);
 }
 nav=true;poll(NULL);check_home_bounds();capture(argv[2],"day-navigation");int nav_height=lv_obj_get_height(page->body);
 nav=false;poll(NULL);check_home_bounds();CHECK(lv_obj_get_height(page->body)==nav_height+52);
 click(OPEN,0);capture(argv[2],"detail");CHECK(page->mode==DETAIL&&label_has(page->body,"09:00"));click(BACK,0);CHECK(page->mode==HOME);
 page->status.ready=false;click(NEW,0);CHECK(page->mode==HOME&&strstr(last_log,"new blocked ready=0 busy=0"));
 page->status.busy=true;click(NEW,0);CHECK(page->mode==HOME&&strstr(last_log,"new blocked ready=0 busy=1"));
 page->status.busy=false;page->status.ready=true;
 click(NEW,0);CHECK(strstr(last_log,"new editor open"));click(FIELD,0);lv_textarea_set_text(page->input,"编辑中的标题");click(BACK,0);CHECK(!strcmp(page->draft.title,"编辑中的标题")&&page->dirty);capture(argv[2],"editor");click(BACK,0);CHECK(page->modal);click(DISCARD,0);CHECK(page->mode==EDIT);click(FIELD,0);page->draft_saved=true;lv_textarea_set_text(page->input,"已修改的标题");CHECK(!page->draft_saved);CHECK(!cal_ui_flush());CHECK(page->draft_waiting);
 lv_textarea_set_text(page->input,"保存期间继续输入");++backend.completed;CHECK(!cal_ui_flush());CHECK(page->draft_waiting&&!page->draft_saved);++backend.completed;CHECK(cal_ui_flush());CHECK(page->draft_saved&&!strcmp(page->draft.title,"保存期间继续输入"));lv_textarea_set_text(page->input,"项目评审");click(BACK,0);
 click(DATE,1);CHECK(page->inline_picker&&!page->modal);capture(argv[2],"date-picker");
 date_picker_t *dp=lv_obj_get_user_data(page->inline_picker);lv_obj_send_event(dp->next,LV_EVENT_CLICKED,NULL);CHECK(dp->shown_month==11&&page->draft.rule.start_day==epd_date_day((epd_date_t){2026,10,30}));
 lv_obj_send_event(dp->cells[6],LV_EVENT_CLICKED,NULL);CHECK(page->draft.rule.start_day==epd_date_day((epd_date_t){2026,11,1}));
 click(TIME,1);CHECK(page->inline_picker);dp=lv_obj_get_user_data(page->inline_picker);CHECK(lv_obj_get_style_text_font(dp->wheels[0],LV_PART_SELECTED)->line_height>lv_obj_get_style_text_font(dp->wheels[0],0)->line_height);
 CHECK(lv_obj_get_style_bg_opa(dp->wheels[0],LV_PART_SELECTED)==0);CHECK(lv_obj_get_group(dp->wheels[0])==input_group);lv_indev_t *keypad=lv_indev_create();lv_indev_set_type(keypad,LV_INDEV_TYPE_KEYPAD);lv_indev_set_read_cb(keypad,key_read);lv_indev_set_group(keypad,input_group);lv_group_focus_obj(dp->wheels[0]);send_key(keypad,LV_KEY_DOWN);send_key(keypad,LV_KEY_ENTER);CHECK(page->draft.rule.minute==10*60);
 capture(argv[2],"time-picker");drag_up(touch,dp->wheels[1]);CHECK(page->draft.rule.minute>600&&page->draft.rule.minute==lv_roller_get_selected(dp->wheels[0])*60+lv_roller_get_selected(dp->wheels[1]));click(BACK,0);click(BACK,0);click(DISCARD,1);CHECK(page->mode==HOME);
 page->mode=SEARCH;rebuild();capture(argv[2],"search");page->mode=SETTINGS;rebuild();capture(argv[2],"settings");page->mode=CALCULATE;page->calc_days=30;page->calc_date=page->selected;rebuild();capture(argv[2],"calculate");
 page->mode=HOME;page->view=1;page->waiting=true;click(NEW,0);CHECK(page->mode==HOME);page->waiting=false;rebuild();click(MORE,0);capture(argv[2],"menu");click(BACK,0);click(BACK,0);CHECK(back_count==1);
 const int scales[]={90,95,100,110,120};for(unsigned s=0;s<sizeof(scales)/sizeof(scales[0]);++s){int percent=scales[s];scale=percent;for(int i=0;i<5;++i)lv_tiny_ttf_set_size(fonts[i],epd_ui_px(sizes[i]));++scale_revision;nav=true;english=percent==120;page->mode=HOME;page->view=1;rebuild();char n[32];snprintf(n,sizeof(n),"month-%d",percent);capture(argv[2],n);check_home_bounds();CHECK(lv_obj_is_valid(back_button));nav=false;poll(NULL);check_home_bounds();}
 scale=100;for(int i=0;i<5;++i)lv_tiny_ttf_set_size(fonts[i],sizes[i]);++scale_revision;english=false;nav=false;page->mode=EDIT;new_event();page->prefs_loaded=true;page->status=backend;page->hour24=false;hour24=false;click(TIME,1);dp=lv_obj_get_user_data(page->inline_picker);CHECK(dp->wheels[2]);
 lv_roller_set_selected(dp->wheels[0],11,LV_ANIM_OFF);lv_roller_set_selected(dp->wheels[2],0,LV_ANIM_OFF);lv_obj_send_event(dp->wheels[0],LV_EVENT_VALUE_CHANGED,NULL);CHECK(page->draft.rule.minute==0);
 lv_roller_set_selected(dp->wheels[2],1,LV_ANIM_OFF);lv_obj_send_event(dp->wheels[2],LV_EVENT_VALUE_CHANGED,NULL);CHECK(page->draft.rule.minute==720);capture(argv[2],"time-picker-12h");hour24=true;
 lv_obj_delete(page->root);page->root=NULL;lv_obj_clean(page->screen);setting_page.foreground=true;setting_page.id=UI_PAGE_TIME_EDIT;setting_page.screen=page->screen;
 settings_time_edit_create(page->screen);time_edit_view_t *sv=setting_page.feature;CHECK(sv->picker&&sv->value.minute==224);capture(argv[2],"system-date");
 dp=lv_obj_get_user_data(sv->picker);lv_obj_send_event(dp->month,LV_EVENT_CLICKED,NULL);CHECK(dp->choosing_month);
 lv_roller_set_selected(dp->wheels[0],2028-2024,LV_ANIM_OFF);lv_roller_set_selected(dp->wheels[1],1,LV_ANIM_OFF);lv_obj_send_event(dp->wheels[0],LV_EVENT_VALUE_CHANGED,NULL);lv_obj_send_event(dp->month,LV_EVENT_CLICKED,NULL);
 CHECK(dp->shown_year==2028&&dp->shown_month==2);lv_obj_send_event(dp->cells[29],LV_EVENT_CLICKED,NULL);CHECK(sv->value.date.day==29&&sv->value.date.month==2);
 CHECK(clock_writes==0);sv->mode=EPD_PICK_TIME;sv->rebuild=true;settings_time_edit_process();dp=lv_obj_get_user_data(sv->picker);
 lv_roller_set_selected(dp->wheels[0],7,LV_ANIM_OFF);lv_roller_set_selected(dp->wheels[1],0,LV_ANIM_OFF);lv_obj_send_event(dp->wheels[1],LV_EVENT_VALUE_CHANGED,NULL);CHECK(sv->value.minute==420);capture(argv[2],"system-time");
 int target=TIME_EDIT_SAVE;CHECK(!settings_time_edit_action(&target)&&target==NAV_BACK&&clock_writes==1&&written.tm_mday==29&&written.tm_hour==7);
 automatic=true;settings_time_edit_process();CHECK(lv_obj_has_state(sv->save,LV_STATE_DISABLED));target=TIME_EDIT_SAVE;CHECK(settings_time_edit_action(&target)&&clock_writes==1&&setting_errors==1);
 automatic=false;scale=120;english=true;++scale_revision;for(int i=0;i<5;++i)lv_tiny_ttf_set_size(fonts[i],sizes[i]*120/100);sv->mode=EPD_PICK_DATE;sv->rebuild=true;settings_time_edit_process();capture(argv[2],"system-date-120");
 lv_obj_update_layout(sv->body);CHECK(lv_obj_get_width(sv->picker)<=lv_obj_get_width(lv_obj_get_parent(sv->picker)));
 lv_obj_scroll_to_view(sv->save,LV_ANIM_OFF);lv_obj_update_layout(sv->body);lv_area_t save_area,body_area;lv_obj_get_coords(sv->save,&save_area);lv_obj_get_coords(sv->body,&body_area);CHECK(save_area.y1>=body_area.y1&&save_area.y2<=body_area.y2);settings_time_edit_stop();lv_obj_clean(page->screen);
 puts("calendar UI and shared date/time picker: views, back, input preservation, leap-day selection, 12h conversion and manual clock save passed");return 0;}
