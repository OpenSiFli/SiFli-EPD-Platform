#include "calendar_runtime.h"
#include <mod_installer.h>
#include "epd_reminder.h"
#include "epd_date_picker.h"
#include "epd_file_picker.h"
#include "system_time.h"
#include "storage_file.h"
#include "platform/epd_app.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_locale.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

EPD_APP_DEFINE("calendr");
MODULE_INIT_DEF(cal_runtime_init)
MODULE_CLEANUP_DEF(cal_runtime_cleanup)
typedef enum { HOME,DETAIL,EDIT,SEARCH,SETTINGS,CALCULATE } screen_t;
typedef enum { BACK,VIEW,PREV,NEXT,TODAY,DAY,MONTH,NEW,OPEN,MORE,MENU,EDIT_EVENT,COPY_EVENT,REMOVE_EVENT,
    SAVE,FIELD,FIELD_OK,DATE,TIME,DATE_OK,KIND,ALL_DAY,REPEAT,REPEAT_SET,WEEKDAY,REMIND,REMIND_SET,
    LUNAR,INCLUSIVE,FILTER,LIST_PREV,LIST_NEXT,PREF,PREF_SET,EXPORT,IMPORT,CONFIRM,DISCARD,EDIT_SCOPE,DELETE_SCOPE,CALC_MODE } action_t;
typedef struct {action_t action;int32_t value;} binding_t;
enum { NOTES_INPUT_CHARS=2000, NOTES_INPUT_BYTES=NOTES_INPUT_CHARS*4 };
typedef struct {
    lv_obj_t *screen,*root,*body,*modal,*input,*keyboard,*picker,*inline_picker,*date_error,*date_labels[2],*time_labels[2],*modal_error,*shade;
    lv_obj_t *grid_cells[42],*grid_date[42],*grid_lunar[42],*grid_dots[42],*list,*lunar_title;
    binding_t *grid_bindings[42];size_t list_bindings;bool list_only;int32_t grid_start;
    lv_obj_t *view_tabs[4];size_t home_bindings;
    char built_message[192];uint32_t query_revision,list_revision;
    int list_y;int scroll[6];screen_t built_mode;int built_view;size_t modal_bindings;
    lv_timer_t *timer;
    cal_status_t status;
    cal_event_t draft,original;
    char *notes_input;
    screen_t mode,source;
    epd_date_t selected,calc_date;
    uint32_t revision,scale,locale,completed,pending_id;
    int64_t minute;
    int32_t last_day;int last_timezone;bool hour24;
    int view,filter,field,date_field,calc_days,calc_mode,expanded;bool time_picker,date_invalid;
    int32_t occurrence;
    int width,height,content_bottom;
    size_t offset;
    bool dirty,instance,waiting,retired,need_build,prefs_loaded,draft_saved,draft_waiting,draft_failed;uint32_t draft_completed,edit_revision,draft_revision;
    char query[241],message[192];
    binding_t bindings[420];size_t bindings_used;
} page_t;
static page_t *page;
static uint32_t intent_id;
static int32_t intent_day;
static void rebuild(void);
static void request_rows(void);
static void action(lv_event_t *event);
static bool field_apply(void);
static const char *tr(const char *zh,const char *en){return !strcmp(epd_locale_get(),"en_us")?en:zh;}
static const char *kind_name(int k){static const char *zh[]={"日程","生日","纪念日","倒数日"},*en[]={"Event","Birthday","Anniversary","Countdown"};return tr(zh[k],en[k]);}
static int px(int n){return epd_ui_px(n);}
static const lv_font_t *font(epd_font_role_t role){return epd_app_font_role(role);}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,int radius,unsigned color)
{
    lv_obj_t *o=lv_obj_create(parent);epd_app_object_init(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_pad_all(o,0,0);lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_radius(o,px(radius),0);lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,255,0);return o;
}
static lv_obj_t *text(lv_obj_t *p,const char *s,int x,int y,int w,epd_font_role_t role,unsigned color)
{
    lv_obj_t *o=epd_app_label(p,s,x,y,w,font(role));lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);lv_obj_set_height(o,LV_SIZE_CONTENT);return o;
}
static void bind(lv_obj_t *o,action_t a,int32_t value)
{
    if(page->bindings_used>=sizeof(page->bindings)/sizeof(page->bindings[0]))return;
    binding_t *b=&page->bindings[page->bindings_used++];*b=(binding_t){a,value};lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,b);
}
static lv_obj_t *button(lv_obj_t *p,const char *s,int x,int y,int w,int h,action_t a,int32_t v,bool selected)
{
    lv_obj_t *o=epd_app_button(p,x,y,w,h,s,NULL,NULL);lv_obj_set_style_radius(o,px(24),0);lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(selected?0x242424:0xffffff),0);lv_obj_set_style_bg_opa(o,255,0);
    if(s){lv_obj_t *label=lv_obj_get_child(o,0);lv_obj_set_style_text_font(label,font(EPD_FONT_SMALL),0);lv_obj_set_style_text_color(label,lv_color_hex(selected?0xffffff:0x222222),0);lv_obj_set_width(label,w-px(16));lv_obj_center(label);}
    bind(o,a,v);return o;
}
static void points_free(lv_event_t *e){cal_free(lv_event_get_user_data(e));}
static void line(lv_obj_t *p,const int *xy,size_t n,int x,int y,unsigned color)
{
    lv_point_precise_t *points=cal_alloc(n*sizeof(*points));if(!points)return;
    for(size_t i=0;i<n;++i){points[i].x=px(xy[i*2]);points[i].y=px(xy[i*2+1]);}
    lv_obj_t *o=lv_line_create(p);lv_obj_set_pos(o,x,y);lv_line_set_points(o,points,(uint32_t)n);lv_obj_set_style_line_width(o,px(3),0);
    lv_obj_set_style_line_color(o,lv_color_hex(color),0);lv_obj_set_style_line_rounded(o,true,0);lv_obj_add_event_cb(o,points_free,LV_EVENT_DELETE,points);
}
static void icon(lv_obj_t *p,const char *name,int x,int y,unsigned color)
{
    if(!strcmp(name,"plus")){int a[]={20,4,20,36},b[]={4,20,36,20};line(p,a,2,x,y,color);line(p,b,2,x,y,color);}
    else if(!strcmp(name,"back")||!strcmp(name,"next")){int a[]={26,5,11,20,26,35};if(!strcmp(name,"next"))for(int i=0;i<6;i+=2)a[i]=40-a[i];line(p,a,3,x,y,color);}
    else if(!strcmp(name,"check")){int a[]={5,21,16,32,36,8};line(p,a,3,x,y,color);}
    else if(!strcmp(name,"more")){for(int i=0;i<3;++i)box(p,x+px(17),y+px(6+i*12),px(5),px(5),3,color);}
    else if(!strcmp(name,"search")){lv_obj_t *o=box(p,x+px(4),y+px(3),px(26),px(26),13,0xffffff);lv_obj_set_style_border_width(o,px(3),0);lv_obj_set_style_border_color(o,lv_color_hex(color),0);int a[]={27,27,38,38};line(p,a,2,x,y,color);}
    else {lv_obj_t *o=box(p,x+px(3),y+px(6),px(34),px(31),5,0xffffff);lv_obj_set_style_border_width(o,px(2),0);lv_obj_set_style_border_color(o,lv_color_hex(color),0);int a[]={3,15,37,15};line(p,a,2,x,y,color);if(!strcmp(name,"year"))for(int r=0;r<2;++r)for(int c=0;c<3;++c)box(p,x+px(9+c*9),y+px(20+r*8),px(4),px(4),1,color);else{int b[]={10,24,30,24};line(p,b,2,x,y,color);}}
}
static lv_obj_t *icon_button(lv_obj_t *p,const char *name,int x,int y,action_t a,int32_t value)
{int size=epd_ui_touch(56);lv_obj_t *o=button(p,NULL,x,y,size,size,a,value,false);lv_obj_set_style_bg_opa(o,0,0);icon(o,name,(size-px(40))/2,(size-px(40))/2,0x222222);return o;}
static void format_date(epd_date_t d,char *out,size_t n){snprintf(out,n,"%04d/%02u/%02u",d.year,d.month,d.day);}
static int timezone(void){system_time_settings_t s;system_time_get_settings(&s);return s.timezone_minutes;}
static void format_time(int64_t utc,bool with_date,char *out,size_t capacity)
{
    system_time_settings_t settings;system_time_get_settings(&settings);int32_t day=epd_date_utc_day(utc,settings.timezone_minutes);epd_date_t d=epd_date_from_day(day);
    if(!epd_date_valid(d)){snprintf(out,capacity,"--");return;}
    int minute=(int)((utc+settings.timezone_minutes*60-(int64_t)day*86400)/60),hour=minute/60;char clock[40],date[32]={0};
    if(settings.hour24)snprintf(clock,sizeof(clock),"%02d:%02d",hour,minute%60);
    else snprintf(clock,sizeof(clock),"%s %d:%02d",hour<12?tr("上午","AM"):tr("下午","PM"),hour%12?hour%12:12,minute%60);
    if(with_date){format_date(d,date,sizeof(date));snprintf(out,capacity,"%s %s",date,clock);}else snprintf(out,capacity,"%s",clock);
}
static epd_date_t today(void){return epd_date_from_day(system_time_local_day(system_time_get_utc()));}
static void modal_close(void)
{
    if(page->keyboard){epd_keyboard_set_textarea(page->keyboard,NULL);lv_obj_delete(page->keyboard);page->keyboard=NULL;}
    if(page->modal){lv_obj_delete(page->modal);page->modal=NULL;page->bindings_used=page->modal_bindings;}if(page->shade){lv_obj_delete(page->shade);page->shade=NULL;}page->input=NULL;page->picker=NULL;page->modal_error=NULL;
    cal_free(page->notes_input);page->notes_input=NULL;
}
static lv_obj_t *modal(const char *title,int h)
{
    modal_close();page->modal_bindings=page->bindings_used;int w=page->width-px(40);h=LV_MIN(h,page->height-px(20));
    page->shade=box(page->screen,0,0,page->width,lv_obj_get_height(page->screen),0,0x000000);lv_obj_set_style_bg_opa(page->shade,60,0);lv_obj_add_flag(page->shade,LV_OBJ_FLAG_CLICKABLE);
    page->modal=box(page->shade,px(20),px(180),w,h,30,0xf2f2f2);lv_obj_set_style_border_width(page->modal,1,0);lv_obj_set_style_border_color(page->modal,lv_color_hex(0xbdbdbd),0);
    lv_obj_set_style_shadow_width(page->modal,px(16),0);lv_obj_set_style_shadow_opa(page->modal,32,0);epd_app_input_group(page->modal);
    lv_obj_t *back=icon_button(page->modal,"back",px(12),px(10),BACK,0);epd_app_input_back(page->modal,back);
    text(page->modal,title,px(78),px(22),w-px(100),EPD_FONT_BODY,0x111111);return page->modal;
}
static void choice(const char *title,const char *const *items,int count,action_t a,int selected)
{
    lv_obj_t *m=modal(title,px(90+72*count));int w=page->width-px(72);
    for(int i=0;i<count;++i)button(m,items[i],px(16),px(76+i*72),w,px(62),a,i,i==selected);
}
static void notify(const char *message)
{
    if(page->modal){if(!page->modal_error)page->modal_error=text(page->modal,message,px(20),px(66),page->width-px(80),EPD_FONT_CAPTION,0x222222);else lv_label_set_text(page->modal_error,message);return;}
    snprintf(page->message,sizeof(page->message),"%s",message);page->need_build=true;
}
static void ask_discard(void)
{const char *items[]={tr("继续编辑","Keep editing"),tr("放弃更改","Discard changes")};choice(tr("尚未保存","Unsaved changes"),items,2,DISCARD,0);}
static void keyboard_ready(lv_event_t *event)
{(void)event;if(page&&page->input&&field_apply()){modal_close();page->draft_saved=false;page->draft_failed=false;page->need_build=true;}}
static void input_changed(lv_event_t *event)
{(void)event;if(page&&page->mode==EDIT&&page->field<3){++page->edit_revision;page->dirty=true;page->draft_saved=false;page->draft_failed=false;}}
static void text_editor(int field)
{
    /* Reserve before accepting edits so closing never needs an allocation. */
    char *notes=field==2?cal_alloc(NOTES_INPUT_BYTES+1):NULL;
    if(field==2&&!notes){notify(tr("内存不足，暂时无法编辑备注","Not enough memory to edit notes"));return;}
    if(field<3){page->draft_saved=false;page->draft_failed=false;}
    page->field=field;const char *title=field==0?tr("标题","Title"):field==1?tr("地点","Location"):field==2?tr("备注","Notes"):field==3?tr("搜索日程","Search events"):tr("天数","Days");
    lv_obj_t *m=modal(title,page->height-px(20));page->notes_input=notes;int w=page->width-px(72);
    page->input=lv_textarea_create(m);epd_app_object_init(page->input);lv_obj_set_pos(page->input,px(16),px(80));lv_obj_set_size(page->input,w,field==2?px(260):px(100));
    lv_obj_set_style_radius(page->input,px(22),0);lv_obj_set_style_text_font(page->input,font(EPD_FONT_BODY),0);lv_obj_set_style_pad_all(page->input,px(18),0);
    lv_textarea_set_max_length(page->input,field==0?80:field==1?120:field==2?NOTES_INPUT_CHARS:field==3?80:6);lv_textarea_set_one_line(page->input,field!=2);
    char days[24];snprintf(days,sizeof(days),"%d",page->calc_days);
    const char *value=field==0?page->draft.title:field==1?page->draft.place:field==2?(page->draft.notes?page->draft.notes:""):field==3?page->query:days;
    lv_textarea_set_text(page->input,value);lv_obj_add_event_cb(page->input,input_changed,LV_EVENT_VALUE_CHANGED,NULL);
    icon_button(m,"check",w-px(42),px(10),FIELD_OK,0);
    epd_keyboard_config_t cfg={.mode=field==4?EPD_KEYBOARD_NUMBER:EPD_KEYBOARD_TEXT,.chinese=true,.voice=field!=4};
    page->keyboard=epd_keyboard_create(page->screen,page->input,&cfg);
    if(page->keyboard){lv_obj_add_event_cb(page->keyboard,keyboard_ready,LV_EVENT_READY,NULL);int kh=epd_keyboard_preferred_height(page->keyboard);lv_obj_set_pos(page->keyboard,0,lv_obj_get_height(page->screen)-kh-px(84));lv_obj_set_width(page->keyboard,lv_obj_get_width(page->screen));}
}
static epd_date_picker_value_t picker_value(int which)
{
    epd_date_picker_value_t v={.date=page->selected,.first_year=1901,.last_year=2099,.first_weekday=page->status.preferences.first_weekday};
    system_time_settings_t settings;system_time_get_settings(&settings);v.hour24=settings.hour24;
    if(which==1||which==2){int64_t t=(int64_t)page->draft.rule.start_day*1440+page->draft.rule.minute+(which==2?page->draft.duration_minutes:0);
        if(page->draft.all_day&&which==2)t-=1440;int32_t day=(int32_t)(t/1440);int minute=(int)(t%1440);if(minute<0){--day;minute+=1440;}v.date=epd_date_from_day(day);v.minute=minute;}
    if(which==3)v.date=epd_date_from_day(page->draft.rule.until_day);
    if(which==4)v.date=page->calc_date;
    return v;
}
static const char *apply_date(int which,epd_date_picker_value_t value)
{
    int32_t day=epd_date_day(value.date);int minute=page->draft.all_day?0:value.minute;
    if(which==0){page->selected=value.date;page->offset=0;request_rows();}
    else if(which==4)page->calc_date=value.date;
    else {
        if(which==1){page->draft.rule.start_day=day;if(page->draft.rule.until_day<day)page->draft.rule.until_day=day;page->draft.rule.minute=minute;
            if(page->draft.rule.lunar_month){epd_lunar_t l;if(epd_date_to_lunar(value.date,&l)){page->draft.rule.lunar_month=l.date.month;page->draft.rule.lunar_day=l.date.day;page->draft.rule.lunar_leap=l.leap;}}}
        else if(which==2){int64_t duration=(int64_t)(day-page->draft.rule.start_day)*1440+minute-page->draft.rule.minute+(page->draft.all_day?1440:0);
            if(duration<=0||duration>366*1440)return tr("结束时间应晚于开始时间，且不超过一年","End must follow start and be within one year");page->draft.duration_minutes=(uint32_t)duration;}
        else {if(day<page->draft.rule.start_day)return tr("重复结束不能早于开始日期","Repeat end precedes start");page->draft.rule.until_day=day;}
        ++page->edit_revision;page->dirty=true;page->draft_saved=false;page->draft_failed=false;
    }
    return NULL;
}
static void endpoint_labels(void)
{
    for(int i=0;i<2;++i){epd_date_picker_value_t v=picker_value(i+1);char text[64];format_date(v.date,text,sizeof(text));
        if(page->date_labels[i])lv_label_set_text(page->date_labels[i],text);
        if(page->time_labels[i]){int hour=v.minute/60;if(v.hour24)snprintf(text,sizeof(text),"%02d:%02d",hour,v.minute%60);
            else snprintf(text,sizeof(text),"%s %d:%02d",hour<12?tr("上午","AM"):tr("下午","PM"),hour%12?hour%12:12,v.minute%60);lv_label_set_text(page->time_labels[i],text);}}
}
static void inline_date_changed(lv_event_t *event)
{
    (void)event;if(!page||!page->inline_picker)return;
    epd_date_picker_value_t value;epd_date_picker_get(page->inline_picker,&value);
    const char *error=apply_date(page->expanded,value);page->date_invalid=error!=NULL;
    lv_label_set_text(page->date_error,error?error:"");if(!error)endpoint_labels();
}
static void date_picker(int which)
{
    page->date_field=which;epd_date_picker_value_t value=picker_value(which);
    lv_obj_t *m=modal(tr("选择日期","Choose date"),px(600));
    page->picker=epd_date_picker_create(m,EPD_PICK_DATE,&value,page->width-px(80));
    if(page->picker)lv_obj_set_pos(page->picker,px(20),px(86));
    icon_button(m,"check",page->width-px(112),px(10),DATE_OK,0);
}
static void new_event(void)
{
    page->expanded=0;page->date_invalid=false;cal_event_clear(&page->draft);page->draft.rule.start_day=epd_date_day(page->selected);page->draft.rule.until_day=epd_date_day((epd_date_t){2099,12,31});
    page->draft.rule.timezone_minutes=timezone();page->draft.rule.minute=9*60;page->draft.rule.weekdays=1u<<epd_date_weekday(page->draft.rule.start_day);
    page->draft.duration_minutes=page->status.preferences.default_duration;page->scroll[EDIT]=0;page->draft.reminder_minutes=-1;page->source=page->mode;page->mode=EDIT;page->instance=false;page->dirty=false;cal_event_copy(&page->original,&page->draft);
}
static void navigate_month(int delta)
{
    int n=page->selected.year*12+page->selected.month-1+delta;int y=n/12,m=n%12+1;if(y<1901||y>2099)return;
    page->selected.year=(int16_t)y;page->selected.month=(uint8_t)m;page->selected.day=(uint8_t)LV_MIN(page->selected.day,epd_date_month_days(y,m));
}
static void request_rows(void)
{
    if(page->mode!=HOME&&page->mode!=SEARCH)return;
    if(page->mode==HOME&&page->view==0){page->query_revision=0;return;}
    int32_t d=epd_date_day(page->selected),first=d,last=d;
    unsigned fields=CAL_QUERY_ROWS;
    if(page->mode==SEARCH){first=epd_date_day((epd_date_t){1901,1,1});last=epd_date_day((epd_date_t){2099,12,31});}

    else if(page->view==2){first=d-(epd_date_weekday(d)+7-page->status.preferences.first_weekday)%7;last=first+6;}
    if(page->mode==SEARCH)fields|=CAL_QUERY_SEARCH;
    else if(page->view==3)fields|=CAL_QUERY_UPCOMING;
    else fields|=CAL_QUERY_GRID;
    page->query_revision=cal_runtime_query(first,last,timezone(),page->mode==SEARCH?page->query:NULL,page->mode==SEARCH?page->filter:-1,page->offset,fields);
}
static bool rows_ready(void){return page->query_revision&&page->status.query_revision==page->query_revision;}
static void mini_month(int month,int x,int y,int w,int h)
{
    lv_obj_t *card=button(page->body,NULL,x,y,w,h,MONTH,month,false);char title[32];snprintf(title,sizeof(title),tr("%d月","%d"),month);
    text(card,title,px(18),px(10),w-px(36),EPD_FONT_BODY,0x111111);int cell=(w-px(20))/7,top=px(60),row=(h-top-px(12))/7;
    static const char *zh[]={"日","一","二","三","四","五","六"},*en[]={"S","M","T","W","T","F","S"};
    for(int i=0;i<7;++i)text(card,tr(zh[(i+page->status.preferences.first_weekday)%7],en[(i+page->status.preferences.first_weekday)%7]),px(10)+i*cell,top,cell,EPD_FONT_CAPTION,0x666666);
    int start=(epd_date_weekday(epd_date_day((epd_date_t){page->selected.year,(uint8_t)month,1}))+7-page->status.preferences.first_weekday)%7;
    for(int d=1;d<=epd_date_month_days(page->selected.year,month);++d){int n=start+d-1;char s[24];snprintf(s,sizeof(s),"%d",d);lv_obj_t *t=text(card,s,px(10)+(n%7)*cell,top+row*(1+n/7),cell,EPD_FONT_CAPTION,0x222222);if(month==page->selected.month&&d==page->selected.day)lv_obj_set_style_text_decor(t,LV_TEXT_DECOR_UNDERLINE,0);}
}
static int month_grid(int y,bool week)
{
    int w=page->width-px(40),cell=w/7;int rows=week?1:6;int rh=px(76);
    lv_obj_t *card=box(page->body,px(20),y,w,px(48)+rows*rh,28,0xffffff);
    static const char *zh[]={"日","一","二","三","四","五","六"},*en[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    int32_t start=epd_date_day(page->selected);if(!week)start-=page->selected.day-1;start-=(epd_date_weekday(start)+7-page->status.preferences.first_weekday)%7;
    for(int i=0;i<7;++i)text(card,tr(zh[(i+page->status.preferences.first_weekday)%7],en[(i+page->status.preferences.first_weekday)%7]),i*cell,px(12),cell,EPD_FONT_CAPTION,0x666666);
    epd_date_t now=today();page->grid_start=start;
    for(int i=0;i<rows*7;++i){int32_t day=start+i;epd_date_t d=epd_date_from_day(day);if(!epd_date_valid(d))continue;bool selected=day==epd_date_day(page->selected);lv_obj_t *b=button(card,NULL,(i%7)*cell+px(3),px(45)+(i/7)*rh,cell-px(6),rh-px(6),DAY,day,selected);page->grid_cells[i]=b;page->grid_bindings[i]=&page->bindings[page->bindings_used-1];
        char n[8];snprintf(n,sizeof(n),"%u",d.day);lv_obj_t *l=text(b,n,0,px(3),cell-px(6),EPD_FONT_BODY,selected?0xffffff:d.month==page->selected.month?0x111111:0x888888);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);page->grid_date[i]=l;
        if(epd_date_valid(now)&&day==epd_date_day(now)&&!selected){lv_obj_set_style_border_width(b,px(2),0);lv_obj_set_style_border_color(b,lv_color_hex(0x333333),0);}
        const char *label=epd_date_label(d,page->status.preferences.festivals,page->status.preferences.lunar);
        l=text(b,label,0,px(40),cell-px(6),EPD_FONT_CAPTION,selected?0xffffff:0x666666);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);page->grid_lunar[i]=l;
        page->grid_dots[i]=box(b,(cell-px(6))/2-px(2),rh-px(11),px(4),px(4),2,selected?0xffffff:0x222222);
        int bit=day-page->status.grid_first;if(!rows_ready()||bit<0||bit>=42||!(page->status.busy_days&(UINT64_C(1)<<bit)))lv_obj_add_flag(page->grid_dots[i],LV_OBJ_FLAG_HIDDEN);
    }
    return y+px(64)+rows*rh;
}
static int agenda(int y,bool all)
{
    page->list_revision=page->status.data_revision;
    if(!rows_ready())return y+px(80);
    bool any=false;int w=page->width-px(40);int32_t selected=epd_date_day(page->selected),group=INT32_MIN;
    for(size_t i=0;i<page->status.count;++i){cal_row_t *r=&page->status.rows[i];if(!all){int64_t begin=(int64_t)selected*86400-(r->occurrence.all_day?0:timezone()*60);int64_t start=r->occurrence.all_day?(int64_t)r->occurrence.day*86400:r->occurrence.utc;if(start>=begin+86400||start+r->duration*INT64_C(60)<=begin)continue;}
        if(all&&r->occurrence.day!=group){char date[32];format_date(epd_date_from_day(r->occurrence.day),date,sizeof(date));text(page->list,date,px(30),y,w,EPD_FONT_SMALL,0x666666);y+=px(44);group=r->occurrence.day;}
        lv_obj_t *b=button(page->list,NULL,px(20),y,w,px(112),OPEN,(int32_t)i,false);text(b,r->title,px(24),px(14),w-px(48),EPD_FONT_BODY,0x111111);
        char time[96];if(r->occurrence.all_day)snprintf(time,sizeof(time),"%s · %s",tr("全天","All day"),kind_name(r->kind));else format_time(r->occurrence.utc,false,time,sizeof(time));
        text(b,time,px(24),px(66),w-px(48),EPD_FONT_SMALL,0x666666);y+=px(128);any=true;
    }
    if(!any){text(page->list,page->mode==SEARCH?tr("没有匹配的日程","No matching events"):tr("暂无日程","No events"),px(38),y+px(12),w-px(36),EPD_FONT_BODY,0x777777);y+=px(80);}
    if(page->offset)button(page->list,tr("上一组","Previous"),px(20),y,px(180),px(64),LIST_PREV,0,false);
    if(page->status.count==CAL_ROWS)button(page->list,tr("下一组","Next"),page->width-px(200),y,px(180),px(64),LIST_NEXT,0,false);
    y+=px(80);
    if(page->mode==HOME&&page->view==3&&page->status.upcoming_count){text(page->list,tr("即将到来","Upcoming"),px(36),y,w-px(32),EPD_FONT_BODY,0x222222);y+=px(58);
        for(size_t i=0;i<page->status.upcoming_count;++i){cal_row_t*r=&page->status.upcoming[i];lv_obj_t*b=button(page->list,NULL,px(20),y,w,px(112),OPEN,CAL_ROWS+(int32_t)i,false);
            text(b,r->title,px(24),px(16),w-px(48),EPD_FONT_BODY,0x222222);char value[80];snprintf(value,sizeof(value),tr("还有 %ld 天 · %s","%ld days left · %s"),(long)(r->occurrence.day-selected),kind_name(r->kind));text(b,value,px(24),px(66),w-px(48),EPD_FONT_SMALL,0x666666);y+=px(128);}}
    return y;
}
static void render_list(int y,bool all)
{
    page->list_y=y;size_t outer=page->bindings_used;page->list_bindings=340;page->bindings_used=340;
    page->list=box(page->body,0,y,page->width,px(80),0,0xf2f2f2);
    lv_obj_set_height(page->list,agenda(0,all));page->bindings_used=outer;
}
static void update_grid(void)
{
    if(!page->grid_cells[0])return;epd_date_t now=today();int count=page->view==2?7:42;
    for(int i=0;i<count;++i){lv_obj_t*b=page->grid_cells[i];if(!b)continue;int32_t d=page->grid_start+i;bool sel=d==epd_date_day(page->selected);unsigned fg=sel?0xffffff:0x222222;
        lv_obj_set_style_bg_color(b,lv_color_hex(sel?0x242424:0xffffff),0);lv_obj_set_style_text_color(page->grid_date[i],lv_color_hex(fg),0);lv_obj_set_style_text_color(page->grid_lunar[i],lv_color_hex(sel?0xffffff:0x666666),0);
        lv_obj_set_style_border_width(b,!sel&&epd_date_valid(now)&&d==epd_date_day(now)?px(2):0,0);
        lv_obj_set_style_bg_color(page->grid_dots[i],lv_color_hex(fg),0);int bit=d-page->status.grid_first;
        if(rows_ready()&&bit>=0&&bit<42&&(page->status.busy_days&(UINT64_C(1)<<bit)))lv_obj_remove_flag(page->grid_dots[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(page->grid_dots[i],LV_OBJ_FLAG_HIDDEN);}
    if(page->lunar_title){char value[96]={0};if(page->status.preferences.lunar)epd_date_lunar_text(page->selected,value,sizeof(value));lv_label_set_text(page->lunar_title,value);}
}
static int property(const char *name,const char *value,int y,action_t a,int32_t v)
{
    int w=page->width-px(40);lv_obj_t *b=button(page->body,NULL,px(20),y,w,px(84),a,v,false);
    text(b,name,px(24),px(25),w*2/5,EPD_FONT_BODY,0x222222);lv_obj_t *t=text(b,value,w*2/5,px(25),w*3/5-px(32),EPD_FONT_SMALL,0x555555);lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);return y+px(100);
}
static void home(void)
{
    int y=px(4),row_height=epd_ui_touch(56);char caption[80];snprintf(caption,sizeof(caption),"%04d / %02u",page->selected.year,page->selected.month);
    icon_button(page->body,"back",px(20),y,PREV,0);button(page->body,caption,px(84),y,page->width-px(280),row_height,DATE,0,false);icon_button(page->body,"next",page->width-px(188),y,NEXT,0);button(page->body,tr("今天","Today"),page->width-px(116),y,px(96),row_height,TODAY,0,false);y+=row_height+px(12);
    if(page->view==0){int first=page->selected.month<=6?1:7,w=(page->width-px(60))/2;for(int i=0;i<6;++i)mini_month(first+i,px(20)+(i%2)*(w+px(20)),y+(i/2)*px(270),w,px(250));}
    else if(page->view==1){y=month_grid(y,false);char date[96]={0};epd_date_lunar_text(page->selected,date,sizeof(date));page->lunar_title=text(page->body,page->status.preferences.lunar?date:"",px(38),y,page->width-px(76),EPD_FONT_BODY,0x333333);render_list(y+px(60),false);}
    else if(page->view==2){y=month_grid(y,true);render_list(y,true);}
    else {char d[16];snprintf(d,sizeof(d),"%u",page->selected.day);lv_obj_t *card=box(page->body,px(20),y,page->width-px(40),px(210),30,0xffffff);text(card,d,px(28),px(16),px(220),EPD_FONT_TEMPERATURE,0x111111);char lunar[96]={0};if(page->status.preferences.lunar)epd_date_lunar_text(page->selected,lunar,sizeof(lunar));text(card,lunar,px(240),px(65),page->width-px(300),EPD_FONT_BODY,0x555555);render_list(y+px(230),false);}
}
static const char *repeat_name(unsigned r){static const char *zh[]={"不重复","每天","每周","每月","每年"},*en[]={"Never","Daily","Weekly","Monthly","Yearly"};return tr(zh[r],en[r]);}
static int endpoint_row(lv_obj_t *card,int which,int y,int width)
{
    int time_width=px(picker_value(which).hour24?100:150),date_width=px(190);
    text(card,which==1?tr("开始","Starts"):tr("结束","Ends"),px(22),y+px(25),width-date_width-time_width-px(65),EPD_FONT_BODY,0x222222);
    int date_x=width-px(22)-date_width-(page->draft.all_day?0:time_width+px(10));
    lv_obj_t *d=button(card,"",date_x,y+px(14),date_width,px(56),DATE,which,page->expanded==which&&!page->time_picker);
    if(!(page->expanded==which&&!page->time_picker))lv_obj_set_style_bg_color(d,lv_color_hex(0xf1f1f1),0);
    page->date_labels[which-1]=lv_obj_get_child(d,0);
    if(!page->draft.all_day){lv_obj_t *t=button(card,"",width-px(22)-time_width,y+px(14),time_width,px(56),TIME,which,page->expanded==which&&page->time_picker);
        if(!(page->expanded==which&&page->time_picker))lv_obj_set_style_bg_color(t,lv_color_hex(0xf1f1f1),0);page->time_labels[which-1]=lv_obj_get_child(t,0);}
    y+=px(88);
    if(page->expanded==which){box(card,px(22),y,width-px(44),1,0,0xe0e0e0);epd_date_picker_value_t value=picker_value(which);
        page->inline_picker=epd_date_picker_create(card,page->time_picker?EPD_PICK_TIME:EPD_PICK_DATE,&value,width-px(32));
        if(page->inline_picker){lv_obj_set_pos(page->inline_picker,px(16),y+px(12));lv_obj_add_event_cb(page->inline_picker,inline_date_changed,LV_EVENT_VALUE_CHANGED,NULL);}
        y+=epd_date_picker_height(page->time_picker?EPD_PICK_TIME:EPD_PICK_DATE)+px(12);
        page->date_error=text(card,"",px(24),y,width-px(48),EPD_FONT_CAPTION,0x444444);y+=px(36);}
    return y;
}
static void editor(void)
{
    int y=px(12),gap=px(12),w=(page->width-px(40)-3*gap)/4;
    for(int i=0;i<4;++i)button(page->body,kind_name(i),px(20)+i*(w+gap),y,w,px(62),KIND,i,page->draft.kind==i);y+=px(80);
    y=property(tr("标题","Title"),page->draft.title,y,FIELD,0);
    int width=page->width-px(40),inner=px(12);lv_obj_t *card=box(page->body,px(20),y,width,px(200),28,0xffffff);
    if(page->draft.kind==CAL_EVENT){lv_obj_t *all=button(card,NULL,px(12),inner,width-px(24),px(70),ALL_DAY,0,false);lv_obj_set_style_bg_opa(all,0,0);
        text(all,tr("全天事件","All day"),px(10),px(20),width-px(140),EPD_FONT_BODY,0x222222);
        lv_obj_t *track=box(all,width-px(106),px(18),px(68),px(36),18,page->draft.all_day?0x222222:0xe5e5e5);
        box(track,px(page->draft.all_day?36:4),px(4),px(28),px(28),14,0xffffff);inner+=px(80);}
    inner=endpoint_row(card,1,inner,width);if(page->draft.kind==CAL_EVENT)inner=endpoint_row(card,2,inner,width);
    lv_obj_set_height(card,inner+px(8));y+=inner+px(28);endpoint_labels();char date[80];
    if(page->draft.kind!=CAL_COUNTDOWN){y=property(tr("重复","Repeat"),repeat_name(page->draft.rule.repeat),y,REPEAT,0);
        if(page->draft.rule.repeat==EPD_REPEAT_WEEKLY){const char *days[]={tr("日","Su"),tr("一","Mo"),tr("二","Tu"),tr("三","We"),tr("四","Th"),tr("五","Fr"),tr("六","Sa")};int cw=(page->width-px(40))/7;for(int i=0;i<7;++i)button(page->body,days[i],px(20)+i*cw,y,cw-px(4),px(60),WEEKDAY,i,(page->draft.rule.weekdays&(1u<<i))!=0);y+=px(80);}
        if(page->draft.rule.repeat){format_date(epd_date_from_day(page->draft.rule.until_day),date,sizeof(date));y=property(tr("重复结束","Repeat until"),date,y,DATE,3);}}
    if(page->draft.kind==CAL_BIRTHDAY||page->draft.kind==CAL_ANNIVERSARY)y=property(tr("农历日期","Lunar date"),page->draft.rule.lunar_month?tr("开启","On"):tr("关闭","Off"),y,LUNAR,0);
    if(page->draft.kind==CAL_COUNTDOWN)y=property(tr("包含起始日","Include start date"),page->draft.inclusive?tr("开启","On"):tr("关闭","Off"),y,INCLUSIVE,0);
    if(epd_reminder_available()){if(page->draft.reminder_minutes<0)strcpy(date,tr("不提醒","None"));else if(!page->draft.reminder_minutes)strcpy(date,tr("准时","At time"));else snprintf(date,sizeof(date),tr("提前 %ld 分钟","%ld minutes before"),(long)page->draft.reminder_minutes);y=property(tr("提醒","Reminder"),date,y,REMIND,0);}
    y=property(tr("地点","Location"),page->draft.place,y,FIELD,1);property(tr("备注","Notes"),page->draft.notes?page->draft.notes:"",y,FIELD,2);
}
static void detail(void)
{
    cal_event_t *e=&page->draft;int w=page->width-px(40);lv_obj_t *card=box(page->body,px(20),px(12),w,px(280),28,0xffffff);
    text(card,kind_name(e->kind),px(24),px(20),w-px(48),EPD_FONT_SMALL,0x666666);lv_obj_t *title=text(card,e->title,px(24),px(65),w-px(48),EPD_FONT_TITLE,0x111111);lv_obj_set_height(title,px(100));lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    char date[128],end[80];
    if(e->all_day){format_date(epd_date_from_day(page->occurrence),date,sizeof(date));format_date(epd_date_from_day(page->occurrence+e->duration_minutes/1440-1),end,sizeof(end));}
    else {int64_t start=(int64_t)page->occurrence*86400+(e->rule.minute-e->rule.timezone_minutes)*60;format_time(start,true,date,sizeof(date));format_time(start+e->duration_minutes*INT64_C(60),true,end,sizeof(end));}
    text(card,date,px(24),px(170),w-px(48),EPD_FONT_SMALL,0x555555);text(card,end,px(24),px(216),w-px(48),EPD_FONT_SMALL,0x555555);
    int y=px(310);text(page->body,repeat_name(e->rule.repeat),px(36),y,w-px(32),EPD_FONT_SMALL,0x555555);y+=px(56);
    if(e->rule.month_end&&epd_date_from_day(e->rule.start_day).month==2&&epd_date_from_day(e->rule.start_day).day==29){text(page->body,tr("闰日生日：平年按二月末","Leap-day birthday: last day of February"),px(36),y,w-px(32),EPD_FONT_SMALL,0x555555);y+=px(84);}
    if(e->rule.lunar_month){text(page->body,tr("无对应闰月时按普通月，三十遇小月按月末","Missing leap month uses the regular month; day 30 uses month end"),px(36),y,w-px(32),EPD_FONT_SMALL,0x555555);y+=px(100);}
    if(e->kind==CAL_COUNTDOWN){int32_t now=epd_date_day(today());if(now!=INT32_MIN){int32_t delta=page->occurrence-now;if(delta>0)snprintf(date,sizeof(date),tr("还有 %ld 天","%ld days left"),(long)delta+(e->inclusive?1:0));else if(delta<0)snprintf(date,sizeof(date),tr("已过 %ld 天","%ld days ago"),(long)-delta+(e->inclusive?1:0));else strcpy(date,tr("就是今天","Today"));text(page->body,date,px(36),y,w-px(32),EPD_FONT_TITLE,0x222222);y+=px(100);}}
    if(*e->place)y=property(tr("地点","Location"),e->place,y,EDIT_EVENT,0);
    if(e->notes&&*e->notes){lv_obj_t *notes=text(page->body,e->notes,px(40),y,w-px(40),EPD_FONT_BODY,0x222222);lv_obj_update_layout(notes);y+=lv_obj_get_height(notes)+px(24);}
    button(page->body,tr("编辑","Edit"),px(20),y,w/2-px(8),px(76),EDIT_EVENT,0,true);button(page->body,tr("复制","Duplicate"),px(28)+w/2,y,w/2-px(8),px(76),COPY_EVENT,0,false);y+=px(96);
    button(page->body,tr("删除","Delete"),px(20),y,w,px(76),REMOVE_EVENT,0,false);
}
static void search(void)
{
    lv_obj_t *b=button(page->body,NULL,px(20),px(12),page->width-px(40),px(84),FIELD,3,false);icon(b,"search",px(20),px(22),0x555555);text(b,*page->query?page->query:tr("搜索日程","Search events"),px(80),px(23),page->width-px(150),EPD_FONT_BODY,0x555555);
    int cw=(page->width-px(40))/5;button(page->body,tr("全部","All"),px(20),px(116),cw-px(4),px(60),FILTER,-1,page->filter==-1);for(int i=0;i<4;++i)button(page->body,kind_name(i),px(20)+(i+1)*cw,px(116),cw-px(4),px(60),FILTER,i,page->filter==i);render_list(px(200),true);
}
static void settings(void)
{
    int y=px(12);cal_book_t *b=&page->status.preferences;
    y=property(tr("每周起始日","Week starts"),b->first_weekday?tr("星期一","Monday"):tr("星期日","Sunday"),y,PREF,0);
    const char *views[]={tr("年","Year"),tr("月","Month"),tr("周","Week"),tr("日","Day")};y=property(tr("默认视图","Default view"),views[b->default_view],y,PREF,1);
    y=property(tr("农历","Lunar calendar"),b->lunar?tr("开启","On"):tr("关闭","Off"),y,PREF,2);y=property(tr("节日与节气","Festivals & terms"),b->festivals?tr("开启","On"):tr("关闭","Off"),y,PREF,3);
    char duration[32];snprintf(duration,sizeof(duration),tr("%u 分钟","%u minutes"),b->default_duration);property(tr("默认时长","Default duration"),duration,y,PREF,4);
}
static void calculate(void)
{
    int y=px(12);char value[100];format_date(page->selected,value,sizeof(value));y=property(tr("起始日期","Start date"),value,y,DATE,0);
    const char *modes[]={tr("向后推算","Add days"),tr("向前推算","Subtract days"),tr("相隔天数","Days between")};y=property(tr("计算方式","Calculation"),modes[page->calc_mode],y,CALC_MODE,0);
    int32_t first=epd_date_day(page->selected),result;
    if(page->calc_mode==2){format_date(page->calc_date,value,sizeof(value));y=property(tr("结束日期","End date"),value,y,DATE,4);result=epd_date_day(page->calc_date)-first;snprintf(value,sizeof(value),tr("相隔 %ld 天","%ld days apart"),(long)(result<0?-result:result));}
    else {snprintf(value,sizeof(value),"%d",page->calc_days);y=property(tr("天数","Days"),value,y,FIELD,4);result=first+(page->calc_mode==1?-page->calc_days:page->calc_days);epd_date_t d=epd_date_from_day(result);if(epd_date_valid(d))format_date(d,value,sizeof(value));else strcpy(value,tr("日期超出范围","Date out of range"));}
    text(page->body,value,px(40),y+px(30),page->width-px(80),EPD_FONT_TITLE,0x222222);
    if(page->calc_mode!=2&&epd_date_valid(epd_date_from_day(result)))button(page->body,tr("查看日期","View date"),px(30),y+px(120),page->width-px(60),px(76),DAY,result,true);
}
static void rebuild(void)
{
    if(!page||page->retired||page->modal)return;page->need_build=false;
    lv_area_t area;epd_ui_content_area(page->screen,&area);
    bool reuse=page->root&&page->mode==HOME&&page->built_mode==HOME&&page->width==lv_obj_get_width(page->screen)&&
        page->scale==epd_ui_scale_revision()&&page->locale==epd_locale_revision()&&page->content_bottom==area.y2+1&&
        !strcmp(page->message,page->built_message);
    if(page->list_only&&reuse&&page->list&&page->built_view==page->view){page->list_only=false;update_grid();lv_obj_clean(page->list);size_t saved=page->bindings_used;page->bindings_used=page->list_bindings;lv_obj_set_height(page->list,agenda(0,page->view==2));page->bindings_used=saved;return;}
    page->list_only=false;page->inline_picker=NULL;page->date_error=NULL;memset(page->date_labels,0,sizeof(page->date_labels));memset(page->time_labels,0,sizeof(page->time_labels));page->list=NULL;page->lunar_title=NULL;memset(page->grid_cells,0,sizeof(page->grid_cells));
    if(reuse){
        lv_obj_clean(page->body);page->bindings_used=page->home_bindings;home();
        for(int i=0;i<4;++i)if(i==page->built_view||i==page->view){lv_obj_t *tab=page->view_tabs[i];
            lv_obj_set_style_bg_color(tab,lv_color_hex(i==page->view?0x242424:0xffffff),0);
            lv_obj_set_style_text_color(lv_obj_get_child(tab,0),lv_color_hex(i==page->view?0xffffff:0x222222),0);}
        lv_obj_update_layout(page->body);lv_obj_scroll_to_y(page->body,page->scroll[HOME],LV_ANIM_OFF);page->built_view=page->view;return;
    }
    page->bindings_used=0;memset(page->view_tabs,0,sizeof(page->view_tabs));
    if(page->root){page->scroll[page->built_mode]=lv_obj_get_scroll_y(page->body);lv_obj_delete(page->root);}
    page->width=lv_obj_get_width(page->screen);page->height=area.y2-area.y1+1;page->content_bottom=area.y2+1;
    int header_height=epd_ui_touch(56)+px(16),tab_height=epd_ui_touch(64)+px(8);
    int root_height=page->content_bottom-px(64);
    page->root=box(page->screen,0,px(64),page->width,root_height,0,0xf2f2f2);epd_app_input_group(page->root);
    lv_obj_t *back=icon_button(page->root,"back",px(18),px(8),BACK,0);epd_app_input_back(page->root,back);
    const char *title=page->mode==HOME?tr("日历","Calendar"):page->mode==EDIT?(page->draft.id?tr("编辑日程","Edit event"):tr("创建日程","New event")):page->mode==SEARCH?tr("搜索日程","Search events"):page->mode==SETTINGS?tr("设置","Settings"):page->mode==CALCULATE?tr("日期推算","Date calculation"):kind_name(page->draft.kind);
    text(page->root,title,px(86),(header_height-font(EPD_FONT_TITLE)->line_height)/2,page->width-px(230),EPD_FONT_TITLE,0x111111);
    if(page->mode==EDIT)icon_button(page->root,"check",page->width-px(80),px(8),SAVE,0);
    else if(page->mode==HOME){icon_button(page->root,"plus",page->width-px(150),px(8),NEW,0);icon_button(page->root,"more",page->width-px(78),px(8),MORE,0);}
    int top=header_height,bh=root_height-top-(page->mode==HOME?tab_height+px(8):0);
    if(*page->message){text(page->root,page->message,px(28),top,page->width-px(56),EPD_FONT_SMALL,0x333333);top+=px(66);bh-=px(66);}
    page->body=box(page->root,0,top,page->width,bh,0,0xf2f2f2);lv_obj_add_flag(page->body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(page->body,LV_DIR_VER);lv_obj_set_scrollbar_mode(page->body,LV_SCROLLBAR_MODE_AUTO);
    if(page->mode==HOME){int w=page->width-px(56);lv_obj_t *bar=box(page->root,px(28),root_height-tab_height,w,tab_height,36,0xffffff);lv_obj_set_style_shadow_width(bar,px(12),0);lv_obj_set_style_shadow_opa(bar,24,0);const char *names[]={tr("年","Year"),tr("月","Month"),tr("周","Week"),tr("日","Day")};for(int i=0;i<4;++i)page->view_tabs[i]=button(bar,names[i],px(6)+i*(w-px(12))/4,px(4),(w-px(12))/4-px(4),epd_ui_touch(64),VIEW,i,page->view==i);page->home_bindings=page->bindings_used;}
    if(page->mode==HOME)home();else if(page->mode==DETAIL)detail();else if(page->mode==EDIT)editor();else if(page->mode==SEARCH)search();else if(page->mode==SETTINGS)settings();else calculate();
    lv_obj_update_layout(page->body);lv_obj_scroll_to_y(page->body,page->scroll[page->mode],LV_ANIM_OFF);page->built_mode=page->mode;page->built_view=page->view;
    page->scale=epd_ui_scale_revision();page->locale=epd_locale_revision();
    strcpy(page->built_message,page->message);
}
static bool field_apply(void)
{const char *s=page->input?lv_textarea_get_text(page->input):"";size_t n=strlen(s);if(page->field==0&&n<sizeof(page->draft.title))strcpy(page->draft.title,s);else if(page->field==1&&n<sizeof(page->draft.place))strcpy(page->draft.place,s);else if(page->field==2&&n<=CAL_TEXT_BYTES){char *copy=cal_alloc(n+1);if(!copy)return false;strcpy(copy,s);cal_free(page->draft.notes);page->draft.notes=copy;}else if(page->field==3&&n<sizeof(page->query)){strcpy(page->query,s);page->offset=0;request_rows();}else if(page->field==4){char *end;long v=strtol(s,&end,10);if(*end||v<0||v>73000){notify(tr("天数应为 0 至 73000","Days must be 0–73000"));return false;}page->calc_days=(int)v;}else {notify(tr("文字超过容量限制","Text exceeds capacity"));return false;}if(page->field<3)page->dirty=true;return true;}
static void field_close(void)
{
    if(page->field==2&&page->notes_input){
        /* The textarea admits at most NOTES_INPUT_CHARS UTF-8 characters. */
        strcpy(page->notes_input,lv_textarea_get_text(page->input));
        cal_free(page->draft.notes);page->draft.notes=page->notes_input;page->notes_input=NULL;
        page->dirty=true;
    }else field_apply();
}
static bool file_selected(const char *path,void *context)
{(void)context;bool ok=cal_runtime_submit(CAL_PREVIEW,NULL,0,0,false,path,NULL);if(ok){page->waiting=true;page->completed=page->status.completed;}else notify(tr("当前无法导入","Import unavailable"));return ok;}
static void action(lv_event_t *event)
{
    if(!page||page->retired)return;binding_t b=*(binding_t *)lv_event_get_user_data(event);
    if(!cal_runtime_start())return;
    if(page->retired||page->waiting||page->draft_waiting||page->status.busy){
        if(b.action==NEW)rt_kprintf("calendr: new blocked ready=%u busy=%u waiting=%u draft_waiting=%u retired=%u\n",
            (unsigned)page->status.ready,(unsigned)page->status.busy,(unsigned)page->waiting,(unsigned)page->draft_waiting,(unsigned)page->retired);
        return;
    }
    page->message[0]=0;
    switch(b.action){
    case BACK:
        if(page->modal){if(page->status.preview)cal_runtime_submit(CAL_CANCEL_IMPORT,NULL,0,0,false,NULL,NULL);if(page->input&&!field_apply())return;modal_close();break;}
        if(page->mode==EDIT&&page->expanded){page->expanded=0;page->date_invalid=false;break;}
        if(page->mode==EDIT&&page->dirty){ask_discard();return;}
        if(page->mode==HOME){gui_app_goback();return;}
        page->mode=page->mode==DETAIL||page->mode==EDIT?page->source:HOME;request_rows();break;
    case DISCARD:modal_close();if(b.value){if(page->status.has_draft){if(cal_runtime_submit(CAL_CLEAR_DRAFT,NULL,0,0,false,NULL,NULL)){page->waiting=true;page->completed=page->status.completed;}else notify(tr("草稿未能清除","Draft could not be cleared"));break;}page->dirty=false;page->mode=page->source;request_rows();}break;
    case VIEW:if(page->view==b.value)return;page->scroll[HOME]=0;page->view=b.value;page->offset=0;request_rows();break;
    case PREV:case NEXT:{int sign=b.action==NEXT?1:-1;if(page->view<=1)navigate_month(sign*(page->view==0?6:1));else {epd_date_t d=epd_date_from_day(epd_date_day(page->selected)+sign*(page->view==2?7:1));if(epd_date_valid(d))page->selected=d;}page->offset=0;request_rows();break;}
    case TODAY:{epd_date_t d=today();if(epd_date_valid(d)){page->selected=d;page->offset=0;request_rows();}else notify(tr("系统时间尚未设置","System time is not set"));break;}
    case MONTH:page->selected.month=(uint8_t)b.value;page->selected.day=(uint8_t)LV_MIN(page->selected.day,epd_date_month_days(page->selected.year,b.value));page->view=1;request_rows();break;
    case DAY:page->list_only=page->mode==HOME&&(page->view==1||page->view==2)&&epd_date_from_day(b.value).month==page->selected.month;page->selected=epd_date_from_day(b.value);if(page->mode==CALCULATE){page->mode=HOME;page->view=3;}page->offset=0;request_rows();break;
    case NEW:if(!page->status.ready){rt_kprintf("calendr: new blocked ready=0 busy=0\n");break;}rt_kprintf("calendr: new editor open\n");new_event();break;
    case OPEN:{if(!rows_ready()||page->list_revision!=page->status.data_revision||b.value<0||((b.value<CAL_ROWS)&&(size_t)b.value>=page->status.count)||((b.value>=CAL_ROWS)&&(size_t)(b.value-CAL_ROWS)>=page->status.upcoming_count))break;cal_row_t *r=b.value<CAL_ROWS?&page->status.rows[b.value]:&page->status.upcoming[b.value-CAL_ROWS];if(cal_runtime_event(r->occurrence.id,&page->draft)){page->occurrence=r->occurrence.original_day;page->source=page->mode;page->scroll[DETAIL]=0;page->mode=DETAIL;}break;}
    case MORE:{const char *items[]={tr("搜索日程","Search events"),tr("日期跳转","Go to date"),tr("日期推算","Date calculation"),tr("导入日历","Import calendar"),tr("导出日历","Export calendar"),tr("设置","Settings")};choice(tr("日历","Calendar"),items,6,MENU,-1);return;}
    case MENU:modal_close();if(b.value==0){page->mode=SEARCH;page->offset=0;request_rows();}else if(b.value==1){date_picker(0);return;}else if(b.value==2){page->mode=CALCULATE;page->calc_date=page->selected;}else if(b.value==3){epd_file_picker_open(page->screen,".ics;.calbak",file_selected,NULL);}else if(b.value==4){const char *items[]={tr("完整备份 · 内部存储","Full backup · Internal"),tr("ICS · 内部存储","ICS · Internal"),tr("完整备份 · TF 卡","Full backup · TF card"),tr("ICS · TF 卡","ICS · TF card")};choice(tr("导出日历","Export calendar"),items,4,EXPORT,-1);return;}else page->mode=SETTINGS;break;
    case IMPORT:modal_close();if(cal_runtime_submit(b.value==2?CAL_CANCEL_IMPORT:CAL_IMPORT,NULL,0,0,b.value==1,NULL,NULL)){page->waiting=true;page->completed=page->status.completed;}break;
    case EXPORT:{char path[128];snprintf(path,sizeof(path),"%s/Exports/calendar-%ld.%s",b.value>=2?"/sdcard":"/flash/data/calendr",(long)system_time_get_utc(),b.value%2?"ics":"calbak");modal_close();if(cal_runtime_submit(CAL_EXPORT,NULL,0,0,false,path,NULL)){page->waiting=true;page->completed=page->status.completed;}break;}
    case EDIT_EVENT:if(page->draft.rule.repeat){const char *items[]={tr("仅本次","This occurrence"),tr("整个系列","Entire series")};choice(tr("编辑日程","Edit event"),items,2,EDIT_SCOPE,-1);return;}b.value=1;/* fall through */
    case EDIT_SCOPE:modal_close();page->instance=b.value==0&&page->draft.rule.repeat!=EPD_REPEAT_NONE;if(page->instance)page->draft.rule.start_day=page->occurrence;cal_event_copy(&page->original,&page->draft);page->mode=EDIT;page->dirty=false;break;
    case COPY_EVENT:page->draft.id=0;page->draft.uid[0]=0;page->draft.parent=0;page->draft.rule.excluded_count=0;page->instance=false;page->mode=EDIT;page->dirty=true;break;
    case REMOVE_EVENT:{const char *items[]={tr("仅本次","This occurrence"),tr("整个系列","Entire series")};if(page->draft.rule.repeat)choice(tr("删除日程","Delete event"),items,2,DELETE_SCOPE,-1);else {const char *confirm[]={tr("删除","Delete")};choice(tr("删除日程？","Delete event?"),confirm,1,DELETE_SCOPE,-1);}return;}
    case DELETE_SCOPE:modal_close();if(cal_runtime_submit(CAL_REMOVE,NULL,page->draft.id,page->occurrence,b.value==0,NULL,NULL)){page->waiting=true;page->completed=page->status.completed;}break;
    case SAVE:if(page->date_invalid)return;if(!cal_event_valid(&page->draft)){notify(tr("标题、日期或重复设置不完整","Check title, dates and repetition"));break;}if(cal_runtime_submit(CAL_SAVE,&page->draft,page->draft.id,page->occurrence,page->instance,NULL,NULL)){page->waiting=true;page->completed=page->status.completed;}break;
    case FIELD:text_editor(b.value);return;
    case FIELD_OK:if(field_apply())modal_close();break;
    case DATE:case TIME:
        if(page->mode==EDIT&&(b.value==1||b.value==2)){page->expanded=page->expanded==b.value&&page->time_picker==(b.action==TIME)?0:b.value;page->time_picker=b.action==TIME;page->date_invalid=false;break;}
        date_picker(b.value);return;
    case DATE_OK:{if(!page->picker)return;epd_date_picker_value_t value;epd_date_picker_get(page->picker,&value);const char *error=apply_date(page->date_field,value);if(error){notify(error);return;}modal_close();break;}
    case KIND:page->expanded=0;page->date_invalid=false;page->draft.kind=(uint8_t)b.value;if(b.value==CAL_EVENT||b.value==CAL_COUNTDOWN){page->draft.rule.lunar_month=0;page->draft.rule.month_end=false;}if(b.value){page->draft.all_day=true;page->draft.rule.minute=0;page->draft.duration_minutes=1440;page->draft.rule.repeat=b.value==CAL_COUNTDOWN?EPD_REPEAT_NONE:EPD_REPEAT_YEARLY;page->draft.rule.month_end=b.value==CAL_BIRTHDAY;}else page->draft.rule.month_end=false;page->dirty=true;break;
    case ALL_DAY:page->expanded=0;page->date_invalid=false;page->draft.all_day=!page->draft.all_day;page->draft.rule.minute=page->draft.all_day?0:9*60;page->draft.duration_minutes=page->draft.all_day?1440:page->status.preferences.default_duration;page->dirty=true;break;
    case REPEAT:{const char *items[]={repeat_name(0),repeat_name(1),repeat_name(2),repeat_name(3),repeat_name(4)};choice(tr("重复","Repeat"),items,5,REPEAT_SET,page->draft.rule.repeat);return;}
    case REPEAT_SET:page->draft.rule.repeat=b.value;if(b.value!=EPD_REPEAT_YEARLY)page->draft.rule.lunar_month=0;page->dirty=true;modal_close();break;
    case WEEKDAY:if(page->draft.rule.weekdays==(1u<<b.value))break;page->draft.rule.weekdays^=1u<<b.value;page->dirty=true;break;
    case REMIND:{const char *items[]={tr("不提醒","None"),tr("准时","At time"),tr("提前 10 分钟","10 minutes before"),tr("提前 30 分钟","30 minutes before"),tr("提前 1 天","1 day before")};choice(tr("提醒","Reminder"),items,5,REMIND_SET,-1);return;}
    case REMIND_SET:{int v[]={-1,0,10,30,1440};page->draft.reminder_minutes=v[b.value];page->dirty=true;modal_close();break;}
    case LUNAR:if(page->draft.rule.lunar_month)page->draft.rule.lunar_month=0;else{epd_lunar_t l;if(epd_date_to_lunar(epd_date_from_day(page->draft.rule.start_day),&l)){page->draft.rule.lunar_month=l.date.month;page->draft.rule.lunar_day=l.date.day;page->draft.rule.lunar_leap=l.leap;page->draft.rule.repeat=EPD_REPEAT_YEARLY;}}page->dirty=true;break;
    case INCLUSIVE:page->draft.inclusive=!page->draft.inclusive;page->dirty=true;break;
    case FILTER:page->filter=b.value;page->offset=0;request_rows();break;
    case LIST_PREV:page->offset=page->offset>=CAL_ROWS?page->offset-CAL_ROWS:0;request_rows();break;
    case LIST_NEXT:page->offset+=CAL_ROWS;request_rows();break;
    case PREF:page->field=b.value;if(b.value==1){const char *views[]={tr("年","Year"),tr("月","Month"),tr("周","Week"),tr("日","Day")};choice(tr("默认视图","Default view"),views,4,PREF_SET,page->status.preferences.default_view);return;}else if(b.value==4){const char *lengths[]={tr("30 分钟","30 minutes"),tr("60 分钟","60 minutes"),tr("120 分钟","120 minutes")};choice(tr("默认时长","Default duration"),lengths,3,PREF_SET,-1);return;}b.value=0;/* fall through */
    case PREF_SET:{cal_book_t prefs=page->status.preferences;if(page->field==0)prefs.first_weekday=!prefs.first_weekday;else if(page->field==1)prefs.default_view=b.value;else if(page->field==2)prefs.lunar=!prefs.lunar;else if(page->field==3)prefs.festivals=!prefs.festivals;else{int lengths[]={30,60,120};prefs.default_duration=lengths[b.value];}modal_close();cal_runtime_submit(CAL_PREFERENCES,NULL,0,0,false,NULL,&prefs);break;}
    case CALC_MODE:page->calc_mode=(page->calc_mode+1)%3;break;
    default:break;
    }
    if(page->mode==EDIT){++page->edit_revision;page->draft_saved=false;page->draft_failed=false;}page->need_build=true;
    if(page->timer&&(b.action==VIEW||b.action==PREV||b.action==NEXT||b.action==TODAY||b.action==MONTH||b.action==DAY))lv_timer_set_period(page->timer,0);
}
bool cal_ui_flush(void)
{
    if(!page||page->retired||page->mode!=EDIT)return true;
    if(!cal_runtime_start())return false;
    cal_status_t *status=&page->status;cal_runtime_status(status);
    if(page->draft_waiting&&status->completed!=page->draft_completed){page->draft_waiting=false;page->draft_failed=*status->error!=0;page->draft_saved=!page->draft_failed&&page->draft_revision==page->edit_revision;}
    if(page->draft_waiting||page->waiting||page->draft_failed)return false;
    if(page->input&&!page->draft_saved&&!field_apply())return false;
    if(!page->dirty||page->draft_saved)return true;
    if(cal_runtime_submit(CAL_DRAFT,&page->draft,page->draft.id,page->occurrence,page->instance,NULL,NULL)){page->draft_waiting=true;page->draft_completed=status->completed;page->draft_revision=page->edit_revision;}
    return false;
}
static void poll(lv_timer_t *timer)
{
    if(timer)lv_timer_set_period(timer,200);
    if(!page||page->retired||!cal_runtime_start())return;
    uint32_t old=page->status.revision,old_data=page->status.data_revision;
    cal_book_t prefs=page->status.preferences;cal_runtime_status(&page->status);
    bool prefs_changed=prefs.first_weekday!=page->status.preferences.first_weekday||prefs.lunar!=page->status.preferences.lunar||
        prefs.festivals!=page->status.preferences.festivals;
    if(page->status.ready&&!page->status.busy&&!page->prefs_loaded){page->prefs_loaded=true;page->view=page->status.preferences.default_view;request_rows();if(!page->pending_id&&page->status.has_draft&&cal_runtime_draft(&page->draft,&page->occurrence,&page->instance)){page->mode=EDIT;page->source=HOME;page->dirty=true;page->draft_saved=cal_runtime_draft_saved();}page->need_build=true;}
    if(page->pending_id&&page->status.ready){if(cal_runtime_event(page->pending_id,&page->draft)){page->mode=DETAIL;page->source=HOME;page->occurrence=intent_day;page->need_build=true;}page->pending_id=0;}
    if(page->draft_waiting&&page->status.completed!=page->draft_completed){page->draft_waiting=false;page->draft_failed=*page->status.error!=0;page->draft_saved=!page->draft_failed&&page->draft_revision==page->edit_revision;if(page->draft_failed)notify(page->status.error);}
    if(page->waiting&&page->status.completed!=page->completed){page->waiting=false;if(*page->status.error)notify(page->status.error);else if(*page->status.result)notify(page->status.result);else if(page->status.preview){char title[96];snprintf(title,sizeof(title),tr("导入：新增 %u 项，重复 %u 项","Import: %u new, %u existing"),(unsigned)page->status.import_new,(unsigned)page->status.import_existing);
        const char *items[]={tr("导入并更新重复项","Import and update existing"),tr("仅导入新事项","Import new events only"),tr("取消","Cancel")};choice(title,items,3,IMPORT,-1);}else {page->dirty=false;page->mode=page->source==SEARCH?SEARCH:HOME;request_rows();page->need_build=true;}}
    if(old!=page->status.revision){if(*page->status.error)snprintf(page->message,sizeof(page->message),"%s",page->status.error);
        bool data_changed=old_data!=page->status.data_revision&&rows_ready();
        if(page->mode!=EDIT&&(page->mode!=HOME||prefs_changed||strcmp(page->message,page->built_message)||data_changed)){
            if(!page->need_build&&!prefs_changed&&page->mode==HOME&&page->list)page->list_only=true;page->need_build=true;}}
    int64_t minute=system_time_get_utc()/60;
    if(page->minute!=minute){page->minute=minute;epd_app_header_refresh(page->screen);system_time_settings_t settings;system_time_get_settings(&settings);int32_t day=system_time_local_day(system_time_get_utc());if(day!=page->last_day||settings.timezone_minutes!=page->last_timezone||settings.hour24!=page->hour24){page->last_day=day;page->last_timezone=settings.timezone_minutes;page->hour24=settings.hour24;if(page->mode==HOME||page->mode==SEARCH)request_rows();}}
    lv_area_t area;epd_ui_content_area(page->screen,&area);
    if(page->scale!=epd_ui_scale_revision()||page->locale!=epd_locale_revision()||page->content_bottom!=area.y2+1){if(page->keyboard)epd_keyboard_refresh(page->keyboard);page->list_only=false;page->need_build=true;}
    if(page->need_build&&!page->modal)rebuild();
}
void cal_ui_retire(void){if(!page)return;epd_file_picker_cancel(page->screen);modal_close();page->retired=true;if(page->timer)lv_timer_pause(page->timer);}
bool cal_ui_present(void){return page!=NULL;}
static void screen_init(lv_obj_t *screen)
{
    epd_app_object_init(screen);
    lv_obj_set_style_bg_color(screen,lv_color_white(),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
}
static void message(gui_app_msg_type_t msg,void *parameter)
{
    (void)parameter;page_t **slot=gui_app_this_page_memory();
    if(msg==GUI_APP_MSG_ONSTART){page=cal_alloc(sizeof(*page));*slot=page;if(!page){cal_runtime_ui_closed(NULL,0,false);return;}bool started=cal_runtime_start();if(started)cal_runtime_status(&page->status);page->screen=lv_screen_active();gui_app_close_anim();screen_init(page->screen);epd_app_header(page->screen);
        page->selected=today();if(!epd_date_valid(page->selected))page->selected=(epd_date_t){2026,1,1};page->calc_date=page->selected;page->calc_days=30;page->view=1;page->filter=-1;page->pending_id=intent_id;page->minute=-1;
        if(started)request_rows();rebuild();page->timer=lv_timer_create(poll,200,NULL);if(page->timer)lv_timer_pause(page->timer);
    }else{page=*slot;if(!page)return;if(msg==GUI_APP_MSG_ONRESUME){epd_app_header_refresh(page->screen);if(page->timer)lv_timer_resume(page->timer);if(cal_runtime_start())request_rows();}
        else if(msg==GUI_APP_MSG_ONPAUSE){cal_ui_flush();if(page->timer)lv_timer_pause(page->timer);}
        else if(msg==GUI_APP_MSG_ONSTOP){bool saved=cal_ui_flush();if(page->input&&!page->retired)field_close();cal_runtime_ui_closed(!page->retired&&!page->waiting&&page->mode==EDIT&&page->dirty&&!saved?&page->draft:NULL,page->occurrence,page->instance);epd_file_picker_cancel(page->screen);modal_close();if(page->timer)lv_timer_delete(page->timer);cal_event_clear(&page->draft);cal_event_clear(&page->original);cal_free(page);*slot=NULL;page=NULL;}}
}
int app_main(intent_t intent)
{
    if(!cal_runtime_start()&&!app_service_running("calendr"))return -RT_ENOMEM;
    intent_id=0;intent_day=0;const char *target=intent_get_string(intent,"page");if(target){unsigned long id;long d;if(sscanf(target,"event:%lu:%ld",&id,&d)==2){intent_id=(uint32_t)id;intent_day=(int32_t)d;}}
    int result=gui_app_regist_msg_handler_ext("calendr",message,NULL,sizeof(page_t *));
    if(result!=RT_EOK)cal_runtime_ui_closed(NULL,0,false);
    return result;
}
