#include "reader_controls.h"
#include "reader_ai.h"
#include "document/document_view.h"
#include "../common/reader/reader.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_memory.h"
#include "reading_font.h"
#include "frontlight.h"
#include <stdlib.h>

enum { CLOSE=-501, TOC=-502, AI=BOOKS_MENU_AI, PROGRESS=-504, DISPLAY=-505,
       TYPE=-506, FONTS=-507, WEIGHT=-508, REFRESH=-509, SLEEP=-510, INSTALL=-511,
       FONT_PREV=-512,FONT_NEXT=-513, SIZE_DOWN=-514,SIZE_UP=-515,
       LIGHT=-516,LIGHT_TOGGLE=-517,FULL_REFRESH=-518,TOUCH=-519,
       SEEK=-520, PAGE_PREV=-521,PAGE_NEXT=-522, CHAPTER_PREV=-523,CHAPTER_NEXT=-524,
       TOC_PREV=-525,TOC_NEXT=-526,TOC_CURRENT=-527,MANAGE_FONTS=-528,FONT_BASE=-600,TOC_BASE=-650,TOC_FOLD=-680,OPTION_BASE=-1000 };
static struct {
    int tab, built_tab;
    unsigned first, font_first, jump, light_restore;
    bool dirty, touch, font_failed, ai_panel, toc_pending;
    uint32_t locale, font_ticket;
    unsigned sheet_focus_start, toc_feedback;
    int navigation;
    unsigned target;
    lv_obj_t *head,*dock,*chapter,*tabs[5],*captions[5];
    lv_obj_t *sheet,*content,*error,*jump_input,*keyboard,*slider,*progress,*light;
    reading_font_info_t fonts[READING_FONT_LIMIT + 1];
    size_t font_count;
    uint8_t *collapsed;
    size_t collapsed_bytes;
    unsigned visible[7],visible_count,toc_count;
} menu={.touch=true,.light_restore=45};

static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int width)
{return label_create(parent,text,x,y,width,ui_font_small());}
static void style(lv_obj_t *obj,bool selected)
{
    lv_obj_set_style_radius(obj,20,0);
    lv_obj_set_style_border_width(obj,selected?2:1,0);
    lv_obj_set_style_border_color(obj,lv_color_hex(selected?0x222222:0xdddddd),0);
    lv_obj_set_style_bg_color(obj,lv_color_hex(selected?0xe8e8e8:0xffffff),0);
    lv_obj_set_style_shadow_width(obj,0,0);
}
lv_obj_t *books_icon_button(lv_obj_t *parent,int x,int y,int width,int height,book_icon_t icon,int target)
{
    lv_obj_t *button=button_create(parent,x,y,width,height,NULL,target);
    lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    lv_obj_t *image=icon_create(button,book_icon_source(icon),0,0);lv_obj_center(image);
    return button;
}
static lv_obj_t *choice(lv_obj_t *parent,const char *text,int x,int y,int width,int target,bool selected)
{
    lv_obj_t *button=button_create(parent,x,y,width,64,text,target);style(button,selected);
    lv_obj_t *text_obj=lv_obj_get_child(button,0);
    lv_obj_set_style_text_font(text_obj,ui_font_small(),0);lv_obj_set_width(text_obj,width-16);
    lv_label_set_long_mode(text_obj,LV_LABEL_LONG_DOT);return button;
}
static void disable(lv_obj_t *obj,bool disabled)
{if(disabled)lv_obj_add_state(obj,LV_STATE_DISABLED);else lv_obj_remove_state(obj,LV_STATE_DISABLED);}
static void notice(const char *text){if(menu.error)text_update(menu.error,text);}
static void row(const char *title,const char *value,int y,int target)
{
    lv_obj_t *button=button_create(menu.content,0,y,620,74,NULL,target);
    lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    label(button,title,8,20,270);lv_obj_t *v=label(button,value,282,20,266);
    lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_long_mode(v,LV_LABEL_LONG_DOT);
    icon_create(button,book_icon_source(BOOK_ICON_NEXT),560,12);
}
static void slider_event(lv_event_t *event)
{
    lv_obj_t *slider=lv_event_get_target_obj(event);
    if(lv_event_get_code(event)==LV_EVENT_VALUE_CHANGED && slider==menu.slider){
        menu.jump=(unsigned)lv_slider_get_value(slider);
        char text[48];snprintf(text,sizeof(text),"%u / %u",menu.jump,ui_reader_view()->pages);
        if(menu.progress)text_update(menu.progress,text);
        if(menu.jump_input){snprintf(text,sizeof(text),"%u",menu.jump);lv_textarea_set_text(menu.jump_input,text);}
    }
    if(lv_event_get_code(event)==LV_EVENT_RELEASED && pending_page==NAV_IDLE)
        pending_page=slider==menu.light?LIGHT:SEEK;
}
static lv_obj_t *slider_create(lv_obj_t *parent,int y,int max,int value)
{
    lv_obj_t *slider=lv_slider_create(parent);lv_obj_set_pos(slider,76,y);lv_obj_set_size(slider,468,14);
    lv_slider_set_range(slider,1,max?max:1);lv_slider_set_value(slider,value,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider,lv_color_hex(0xdddddd),LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider,lv_color_black(),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider,lv_color_white(),LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_KNOB);
    lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);
    lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
    lv_obj_set_style_border_width(slider,2,LV_PART_KNOB);
    lv_obj_set_style_border_color(slider,lv_color_black(),LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider,15,LV_PART_KNOB);
    lv_obj_add_event_cb(slider,slider_event,LV_EVENT_ALL,NULL);
    return slider;
}
static void options(ui_setting_id_t setting,const char *const *labels,const unsigned *indices,unsigned count,int y)
{
    unsigned value=ui_settings_index(setting);
    unsigned nearest=0;
    if(indices)for(unsigned i=1;i<count;++i)if(abs((int)value-(int)indices[i])<abs((int)value-(int)indices[nearest]))nearest=i;
    int width=(620-(int)(count-1)*10)/(int)count;
    for(unsigned i=0;i<count;++i)
        choice(menu.content,labels[i],(width+10)*(int)i,y,width,
               OPTION_BASE-(int)setting*16-(int)(indices?indices[i]:i),indices?nearest==i:value==i);
}
static int parent_tab(void)
{return menu.tab==FONTS || menu.tab==WEIGHT?TYPE:menu.tab==REFRESH || menu.tab==SLEEP?DISPLAY:0;}
static const char *title(void)
{
    switch(menu.tab){case TOC:return BR(toc);case PROGRESS:return BR(progress);case DISPLAY:return BR(display);
    case TYPE:return BR(type);case FONTS:return BR(fonts);case WEIGHT:return BR(weight);case REFRESH:return BR(refresh);
    case SLEEP:return BR(sleep);default:return BR(menu);}
}
static void destroy_panel(void)
{
    if(!current || !current->reader_panel)return;
    menu.built_tab=0;
    books_ai_ui_close();
    lv_obj_delete(current->reader_panel);current->reader_panel=NULL;
    while(current->focus_count>current->reader_panel_focus_start)current->focus_items[--current->focus_count]=NULL;
    menu.sheet=menu.content=menu.error=menu.keyboard=menu.jump_input=menu.slider=menu.light=menu.progress=NULL;
    menu.head=menu.dock=menu.chapter=NULL;menu.ai_panel=false;
    memset(menu.tabs,0,sizeof(menu.tabs));memset(menu.captions,0,sizeof(menu.captions));
    current->reader_confirm_pending=false;
    if(current->back)lv_obj_add_flag(current->back,LV_OBJ_FLAG_HIDDEN);
}
bool books_menu_active(void)
{
    return current && current->id==UI_PAGE_READER && current->foreground &&
        (menu.tab || (current->reader_panel && !lv_obj_has_flag(current->reader_panel,LV_OBJ_FLAG_HIDDEN)));
}
bool books_menu_font_pending(void){return menu.font_ticket!=0;}
void books_menu_close(void)
{
    epd_reader_toc_prepare(false);
    destroy_panel();menu.tab=0;menu.navigation=0;
    epd_app_free(menu.collapsed);menu.collapsed=NULL;menu.collapsed_bytes=0;menu.first=0;
    ui_reader_background_pause(menu.font_ticket ||
        (current && current->foreground && current->popup && !lv_obj_has_flag(current->popup,LV_OBJ_FLAG_HIDDEN)));
}
static void clear_sheet(void)
{
    /* The number keyboard belongs to the overlay, not the sheet. */
    if(menu.keyboard)lv_obj_delete(menu.keyboard);
    if(menu.sheet)lv_obj_delete(menu.sheet);
    while(current->focus_count>menu.sheet_focus_start)current->focus_items[--current->focus_count]=NULL;
    menu.sheet=menu.content=menu.error=menu.keyboard=menu.jump_input=menu.slider=menu.light=menu.progress=NULL;
}
static void outside(lv_event_t *event)
{
    if(lv_event_get_target_obj(event)==current->reader_panel && pending_page==NAV_IDLE)pending_page=CLOSE;
}
static void jump_keyboard_hide(void)
{
    if(menu.keyboard)lv_obj_add_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN);
    if(menu.sheet)lv_obj_set_y(menu.sheet,HEIGHT-108-744);
}
static void jump_editor(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_CANCEL){jump_keyboard_hide();return;}
    if(code==LV_EVENT_READY){if(pending_page==NAV_IDLE)pending_page=SEEK;return;}
    if(code!=LV_EVENT_CLICKED)return;
    if(!menu.keyboard){
        epd_keyboard_config_t config={.mode=EPD_KEYBOARD_NUMBER};
        menu.keyboard=epd_keyboard_create(current->reader_panel,menu.jump_input,&config);
        if(!menu.keyboard){notice(BR(open_failed));return;}
        lv_obj_add_event_cb(menu.keyboard,jump_editor,LV_EVENT_READY,NULL);
        lv_obj_add_event_cb(menu.keyboard,jump_editor,LV_EVENT_CANCEL,NULL);
    }
    lv_obj_set_size(menu.keyboard,684,epd_keyboard_preferred_height(menu.keyboard));
    lv_obj_align(menu.keyboard,LV_ALIGN_BOTTOM_MID,0,0);lv_obj_remove_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(menu.sheet,112);
}
static void build_progress(bool full)
{
    const ui_reader_view_t *v=ui_reader_view();char text[80];
    if(v->page)snprintf(text,sizeof(text),v->pages?"%u / %u":"%u",v->page,v->pages);
    else snprintf(text,sizeof(text),"%u%%",v->percent);
    menu.progress=label(menu.content,text,0,0,620);menu.jump=v->page;
    menu.slider=slider_create(menu.content,100,(int)v->pages,(int)v->page);disable(menu.slider,!v->pages);
    disable(books_icon_button(menu.content,0,74,64,64,BOOK_ICON_BACK,PAGE_PREV),v->page==1);
    disable(books_icon_button(menu.content,556,74,64,64,BOOK_ICON_NEXT,PAGE_NEXT),v->pages && v->page>=v->pages);
    if(!full)return;
    disable(choice(menu.content,BR(chapter_prev),0,160,300,CHAPTER_PREV,false),!document_view_active());
    disable(choice(menu.content,BR(chapter_next),320,160,300,CHAPTER_NEXT,false),!document_view_active());
    label(menu.content,BR(page),0,270,175);
    menu.jump_input=lv_textarea_create(menu.content);lv_obj_set_pos(menu.jump_input,180,252);lv_obj_set_size(menu.jump_input,230,70);
    lv_obj_set_style_border_width(menu.jump_input,1,0);lv_obj_set_style_border_color(menu.jump_input,lv_color_hex(0x999999),0);
    lv_obj_set_style_radius(menu.jump_input,16,0);lv_obj_set_style_pad_all(menu.jump_input,12,0);
    lv_obj_set_style_text_font(menu.jump_input,ui_font_small(),0);lv_textarea_set_one_line(menu.jump_input,true);
    lv_textarea_set_accepted_chars(menu.jump_input,"0123456789");lv_textarea_set_max_length(menu.jump_input,10);
    if(v->page)snprintf(text,sizeof(text),"%u",v->page);else text[0]=0;
    lv_textarea_set_text(menu.jump_input,text);
    disable(menu.jump_input,!v->pages);
    lv_obj_add_event_cb(menu.jump_input,jump_editor,LV_EVENT_CLICKED,NULL);
    disable(choice(menu.content,BR(jump),440,256,180,SEEK,false),!v->pages);
}
static void build_type(void)
{
    label(menu.content,BR(font_size),0,0,300);label(menu.content,ui_settings_value(UI_SETTING_FONT_SIZE),440,0,180);
    disable(books_icon_button(menu.content,0,54,100,64,BOOK_ICON_MINUS,SIZE_DOWN),ui_settings_index(UI_SETTING_FONT_SIZE)==0);
    centered_label(menu.content,"Aa",110,60,400,ui_font_title());
    disable(books_icon_button(menu.content,520,54,100,64,BOOK_ICON_PLUS,SIZE_UP),ui_settings_index(UI_SETTING_FONT_SIZE)>=7);
    const char *lines[]={BR(tight),BR(medium),BR(loose)},*margins[]={BR(small),BR(medium),BR(large)};
    static const unsigned line_indices[]={1,3,4},margin_indices[]={0,2,4};
    label(menu.content,BR(line),0,140,620);options(UI_SETTING_LINE_SPACING,lines,line_indices,3,188);
    label(menu.content,BR(margin),0,278,620);options(UI_SETTING_MARGIN,margins,margin_indices,3,326);
    row(BR(fonts),ui_settings_value(UI_SETTING_FONT),412,FONTS);
    row(BR(weight),ui_settings_value(UI_SETTING_FONT_WEIGHT),492,WEIGHT);
}
static void build_fonts(void)
{
    size_t count=reading_font_list(menu.fonts,READING_FONT_LIMIT + 1);
    menu.font_count=LV_MIN(count,READING_FONT_LIMIT + 1);
    if(menu.font_first>=menu.font_count)menu.font_first=0;
    for(unsigned i=0;i<5 && menu.font_first+i<menu.font_count;++i){
        reading_font_info_t *f=&menu.fonts[menu.font_first+i];
        disable(choice(menu.content,f->name,0,82*i,620,FONT_BASE-(int)i,f->selected),!f->available || menu.font_ticket);
    }
    if(menu.font_count>5){
        disable(books_icon_button(menu.content,0,416,64,64,BOOK_ICON_BACK,FONT_PREV),!menu.font_first);
        disable(books_icon_button(menu.content,556,416,64,64,BOOK_ICON_NEXT,FONT_NEXT),menu.font_first+5>=menu.font_count);
    }
    choice(menu.content,BR(install),0,492,300,INSTALL,false);
    choice(menu.content,BR(font_manage),320,492,300,MANAGE_FONTS,false);
}
static void build_display(void)
{
    frontlight_snapshot_t light;frontlight_get(&light);
    label(menu.content,BR(brightness),0,0,620);
    menu.light=slider_create(menu.content,92,100,light.committed_percent);
    lv_slider_set_range(menu.light,0,100);lv_slider_set_value(menu.light,light.committed_percent,LV_ANIM_OFF);
    disable(menu.light,!light.available || light.locked);
    if(light.committed_percent)menu.light_restore=light.committed_percent;
    choice(menu.content,light.committed_percent?BR(light_off):BR(light_on),0,148,300,LIGHT_TOGGLE,false);
    choice(menu.content,BR(refresh_now),320,148,300,FULL_REFRESH,false);
    row(BR(refresh),ui_settings_value(UI_SETTING_FULL_REFRESH),250,REFRESH);
    row(BR(sleep),"",334,SLEEP);row(BR(touch),menu.touch?BR(on):BR(off),418,TOUCH);
}
static void build_options(void)
{
    if(menu.tab==WEIGHT){const char *names[]={BR(normal),BR(bold),BR(light)};options(UI_SETTING_FONT_WEIGHT,names,NULL,3,0);return;}
    if(menu.tab==REFRESH){
        static const unsigned values[]={1,5,8,10,15,20};
        for(unsigned i=0;i<6;++i){char text[64];snprintf(text,sizeof(text),"%u %s",values[i],BR(refresh_unit));
            choice(menu.content,text,0,78*i,620,OPTION_BASE-UI_SETTING_FULL_REFRESH*16-i,ui_settings_index(UI_SETTING_FULL_REFRESH)==i);}
        return;
    }
    static const unsigned values[2][6]={{1,3,5,10,15,0},{1,5,15,30,60,0}};
    for(unsigned kind=0;kind<2;++kind){
        ui_setting_id_t setting=kind?UI_SETTING_SYSTEM_SLEEP:UI_SETTING_TIMEOUT;
        int y=kind?270:0;label(menu.content,kind?BR(system_sleep):BR(screen_sleep),0,y,620);
        for(unsigned i=0;i<6;++i){char text[48];
            if(values[kind][i])snprintf(text,sizeof(text),"%u %s",values[kind][i],BR(minute));else snprintf(text,sizeof(text),"%s",BR(never));
            choice(menu.content,text,(i%3)*210,y+52+(i/3)*80,200,OPTION_BASE-setting*16-i,ui_settings_index(setting)==i);
        }
    }
}
static void build_toc(void)
{
    int navigation=epd_reader_toc_prepare(true);
    menu.toc_pending=navigation==0;
    unsigned count=document_view_toc_count();menu.toc_count=menu.toc_feedback=count;
    size_t bytes=(count+7u)/8u;
    if(bytes>menu.collapsed_bytes){
        uint8_t *next=epd_app_alloc(bytes,EPD_APP_PSRAM);
        if(next){memset(next,0,bytes);if(menu.collapsed)memcpy(next,menu.collapsed,menu.collapsed_bytes);
            epd_app_free(menu.collapsed);menu.collapsed=next;menu.collapsed_bytes=bytes;}
    }
    char chapter[256];unsigned active=UINT_MAX;
    epd_reader_chapter_title(chapter,sizeof(chapter),&active);
    choice(menu.content,BR(locate_current),320,0,300,TOC_CURRENT,false);
    if(!count)label(menu.content,navigation==0?BR(indexing):navigation<0?BR(open_failed):BR(no_toc),0,20,620);
    memset(menu.visible,0xff,sizeof(menu.visible));
    unsigned rank=0,hidden=UINT_MAX;
    for(unsigned index=0;index<count;++index){
        unsigned level=epd_reader_toc_level(index);
        if(hidden!=UINT_MAX && level>hidden)continue;
        hidden=UINT_MAX;
        bool folded=index/8<menu.collapsed_bytes && (menu.collapsed[index/8]&(1u<<(index%8)));
        if(folded)hidden=level;
        unsigned i=rank++;
        if(i<menu.first || i>=menu.first+7)continue;
        i-=menu.first;menu.visible[i]=index;
        char title_text[256],text[280];
        if(!document_view_toc_title(index,title_text,sizeof(title_text),&level))continue;
        snprintf(text,sizeof(text),"%s",title_text);
        bool children=index+1<count && epd_reader_toc_level(index+1)>level;
        if(children)books_icon_button(menu.content,0,76+80*i,64,64,folded?BOOK_ICON_NEXT:BOOK_ICON_DOWN,TOC_FOLD-(int)i);
        lv_obj_t *button=choice(menu.content,text,68,76+80*i,552,TOC_BASE-(int)i,index==active);
        lv_obj_t *caption=lv_obj_get_child(button,0);
        int inset=16+(int)(level>4?4:level)*24;
        lv_obj_set_x(caption,inset);lv_obj_set_width(caption,536-inset);
        lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_LEFT,0);
    }
    menu.visible_count=rank;
    disable(books_icon_button(menu.content,0,652,64,64,BOOK_ICON_BACK,TOC_PREV),!menu.first);
    disable(books_icon_button(menu.content,556,652,64,64,BOOK_ICON_NEXT,TOC_NEXT),menu.first+7>=rank);
}
static void chapter_update(bool idle)
{
    if(!menu.chapter)return;
    char text[256];
    if(!epd_reader_chapter_title(text,sizeof(text),NULL))snprintf(text,sizeof(text),"%s %u",BR(page),ui_reader_view()->page);
    if(!strcmp(lv_label_get_text(menu.chapter),text))return;
    if(idle)text_update(menu.chapter,text);
    else epd_service_ui_request(books_service,16);
}
static void build_frame(void)
{
    current->reader_panel_focus_start=current->focus_count;
    lv_obj_t *root=panel_create(current->screen,0,0,WIDTH,HEIGHT);current->reader_panel=root;
    lv_obj_set_style_bg_opa(root,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(root,0,0);
    lv_obj_add_event_cb(root,outside,LV_EVENT_CLICKED,NULL);
    epd_app_input_group(root);
    if(menu.tab==AI){menu.ai_panel=true;books_ai_ui_open(root);return;}
    lv_obj_t *head=menu.head=panel_create(root,0,0,WIDTH,112);lv_obj_set_style_border_width(head,0,0);
    books_icon_button(head,12,20,72,72,BOOK_ICON_BACK,NAV_BACK);
    lv_obj_t *book=label(head,ui_reader_view()->title,104,22,548);lv_label_set_long_mode(book,LV_LABEL_LONG_DOT);
    menu.chapter=label(head,"",104,66,548);
    lv_obj_t *dock=menu.dock=panel_create(root,0,HEIGHT-108,WIDTH,108);lv_obj_set_style_border_width(dock,1,0);
    const int tabs[]={TOC,PROGRESS,AI,DISPLAY,TYPE};
    const book_icon_t icons[]={BOOK_ICON_LIST,BOOK_ICON_PROGRESS,BOOK_ICON_AI,BOOK_ICON_SUN,BOOK_ICON_TYPE};
    const char *names[]={BR(toc),BR(progress_tab),BR(ai),BR(display_tab),BR(type_tab)};
    for(unsigned i=0;i<5;++i){
        lv_obj_t *button=menu.tabs[i]=button_create(dock,136*i,0,i==4?140:136,108,NULL,tabs[i]);
        lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_radius(button,0,0);
        lv_obj_set_style_bg_color(button,lv_color_hex(menu.tab==tabs[i]?0xe8e8e8:0xffffff),0);
        icon_create(button,book_icon_source(icons[i]),44,10);
        lv_obj_t *t=menu.captions[i]=centered_label(button,names[i],2,68,132,ui_font_caption());lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);
    }
    menu.sheet_focus_start=current->focus_count;
}
static void build(void)
{
    if(menu.tab!=TOC)epd_reader_toc_prepare(false);
    /* AI owns a full-height overlay and keyboard; deleting its root also
     * releases objects that books_ai_ui_close only detaches from its state. */
    if(current->reader_panel && (menu.ai_panel || menu.tab==AI))destroy_panel();
    if(!current->reader_panel)build_frame();
    if(menu.tab==AI){menu.built_tab=AI;return;}
    clear_sheet();
    lv_obj_t *root=current->reader_panel;
    const int tabs[]={TOC,PROGRESS,AI,DISPLAY,TYPE};
    const char *names[]={BR(toc),BR(progress_tab),BR(ai),BR(display_tab),BR(type_tab)};
    for(unsigned i=0;i<5;++i){
        bool selected=menu.tab==tabs[i] || parent_tab()==tabs[i];
        lv_color_t color=lv_color_hex(selected?0xe8e8e8:0xffffff);
        if(!lv_color_eq(lv_obj_get_style_bg_color(menu.tabs[i],0),color))lv_obj_set_style_bg_color(menu.tabs[i],color,0);
        if(strcmp(lv_label_get_text(menu.captions[i]),names[i]))text_update(menu.captions[i],names[i]);
    }
    int height=menu.tab==TOC?896:menu.tab==READER_MENU?320:744;
    menu.sheet=panel_create(root,0,HEIGHT-108-height,WIDTH,height);
    lv_obj_set_style_radius(menu.sheet,24,0);lv_obj_set_style_border_width(menu.sheet,1,0);
    lv_obj_t *back=books_icon_button(menu.sheet,parent_tab()?16:596,12,72,72,parent_tab()?BOOK_ICON_BACK:BOOK_ICON_DOWN,parent_tab()?BOOKS_MENU_BACK:CLOSE);
    epd_app_input_back(root,back);
    label(menu.sheet,title(),parent_tab()?100:32,32,parent_tab()?480:540);
    menu.content=panel_create(menu.sheet,32,104,620,height-116);
    lv_obj_set_style_border_width(menu.content,0,0);lv_obj_set_style_bg_opa(menu.content,LV_OPA_TRANSP,0);
    if(menu.tab==TOC)build_toc();else if(menu.tab==PROGRESS || menu.tab==READER_MENU)build_progress(menu.tab==PROGRESS);
    else if(menu.tab==TYPE)build_type();else if(menu.tab==FONTS)build_fonts();else if(menu.tab==DISPLAY)build_display();else build_options();
    menu.error=label(menu.content,"",0,height-164,620);lv_label_set_long_mode(menu.error,LV_LABEL_LONG_DOT);
    if(menu.font_ticket && menu.tab==FONTS)notice(BR(font_wait));
    else if(menu.font_failed && menu.tab==FONTS)notice(BR(font_failed));
    menu.built_tab=menu.tab;
}
void books_menu_open(void)
{
    if(!current || !ui_reader_view()->ready)return;
    menu.tab=READER_MENU;menu.dirty=true;ui_reader_background_pause(true);books_menu_process();
}
bool books_reader_touch_enabled(void){return menu.touch;}
void books_menu_pause(void){epd_reader_toc_prepare(false);books_ai_cancel();books_ai_ui_close();menu.navigation=0;if(menu.tab)menu.dirty=true;ui_reader_background_pause(menu.font_ticket!=0);}
void books_menu_process(void)
{
    if(menu.font_ticket){
        reading_font_status_t status;
        bool found=reading_font_job_status(menu.font_ticket,&status);
        if(!found || !status.busy){
            menu.font_ticket=0;menu.dirty=true;
            menu.font_failed=!found || status.phase==READING_FONT_FAILED;
            if(!menu.font_failed && menu.tab==FONTS)menu.tab=TYPE;
        }
    }
    if(!current || current->id!=UI_PAGE_READER || !current->foreground)return;
    /* Retain UI work until the panel is idle, including font jobs whose
     * result can arrive after the menu has closed. */
    if(menu.font_ticket || (menu.tab && menu.dirty && !epd_app_refresh_done()))
        epd_service_ui_request(books_service,16);
    if(!epd_app_refresh_done()){chapter_update(false);return;}
    if(!menu.tab)return;
    if(menu.tab==TOC){
        unsigned count=document_view_toc_count();
        bool finished=menu.toc_pending && epd_reader_toc_prepare(true)!=0;
        if(menu.toc_feedback!=count || finished){
            menu.toc_feedback=count;
            if(!menu.toc_count || finished)menu.dirty=true;
            else {char text[64];snprintf(text,sizeof(text),"%s: %u",BR(toc),count);notice(text);}
        }
    }
    if(menu.locale!=epd_locale_revision()){menu.locale=epd_locale_revision();menu.dirty=true;}
    if(menu.dirty){menu.dirty=false;build();}
    chapter_update(true);
    if(menu.tab==AI)books_ai_ui_process();
}
static bool navigate(int target,unsigned value)
{
    if(target==PAGE_PREV || target==PAGE_NEXT){ui_reader_turn(target==PAGE_PREV?-1:1);return true;}
    if(target==TOC)return document_view_toc_seek(value);
    if(target==CHAPTER_PREV || target==CHAPTER_NEXT)return epd_reader_chapter_move(target==CHAPTER_PREV?-1:1);
    ui_reader_seek(value);return true;
}
static void request_navigation(int target,unsigned value)
{
    if(ui_reader_settings_changed() || menu.font_ticket || current->reader_reflow_pending){
        books_menu_close();menu.navigation=target;menu.target=value;
    }
    else if(navigate(target,value))books_menu_close();
    else notice(BR(open_failed));
}
void books_menu_navigate(void)
{
    if(!menu.navigation || books_menu_active() || current->popup || menu.font_ticket ||
        ui_reader_settings_changed())return;
    if(ui_reader_view()->error[0]){menu.navigation=0;return;}
    /* TXT seek requires a published page after layout_prepare. Document
     * anchors can be selected earlier and may themselves make it ready. */
    if(!document_view_active() && !ui_reader_view()->ready)return;
    int target=menu.navigation;
    /* Reflow rebuilds the TOC incrementally. Keep the selected index until
     * its anchor is available, or report failure when indexing has ended. */
    if(target==TOC && menu.target>=document_view_toc_count() && ui_reader_view()->indexing)return;
    if((target==CHAPTER_PREV || target==CHAPTER_NEXT) && current->reader_reflow_pending)return;
    menu.navigation=0;
    if(navigate(target,menu.target))return;
    menu.tab=target==TOC?TOC:PROGRESS;menu.dirty=true;ui_reader_background_pause(true);
    books_menu_process();notice(BR(open_failed));
}
bool books_menu_action(int target)
{
    if(target==NAV_BACK){books_menu_close();return false;}
    if(target==READER_MENU){books_menu_open();return true;}
    if(target==BOOKS_MENU_AI && !current->reader_panel){menu.tab=AI;menu.dirty=true;ui_reader_background_pause(true);books_menu_process();return true;}
    if(target<=TOC && target>=SLEEP && (current->reader_panel || menu.tab)){
        menu.tab=target;menu.dirty=true;return true;
    }
    if(!current->reader_panel)return false;
    if(books_ai_ui_action(target))return true;
    if(target==BOOKS_MENU_BACK){
        if(menu.tab==AI && books_ai_ui_back())return true;
        if(menu.keyboard && !lv_obj_has_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN)){jump_keyboard_hide();return true;}
        int parent=parent_tab();if(parent){menu.tab=parent;menu.dirty=true;}else books_menu_close();return true;
    }
    if(target==CLOSE){books_menu_close();return true;}
    if(target==INSTALL){
        if(!storage_available(STORAGE_SD)){notice(BR(font_failed));return true;}
        if(!epd_app_open_font_install())notice(BR(open_failed));return true;
    }
    if(target==MANAGE_FONTS){
        if(!epd_app_open_reading_fonts())notice(BR(open_failed));return true;
    }
    if(target<=FONT_BASE && target>FONT_BASE-5){
        menu.font_failed=false;
        unsigned i=menu.font_first+FONT_BASE-target;
        if(i<menu.font_count && reading_font_select(menu.fonts[i].id,&menu.font_ticket)!=RT_EOK)notice(BR(font_failed));
        else menu.dirty=true;return true;
    }
    if(target==FONT_PREV || target==FONT_NEXT){menu.font_first=target==FONT_PREV?(menu.font_first>5?menu.font_first-5:0):menu.font_first+5;menu.dirty=true;return true;}
    if(target==TOC_PREV || target==TOC_NEXT){menu.first=target==TOC_PREV?(menu.first>7?menu.first-7:0):menu.first+7;menu.dirty=true;return true;}
    if(target==TOC_CURRENT){
        char chapter[256];unsigned active=UINT_MAX;
        if(epd_reader_chapter_title(chapter,sizeof(chapter),&active) && active!=UINT_MAX){
            if(menu.collapsed)memset(menu.collapsed,0,menu.collapsed_bytes);
            menu.first=active/7*7;menu.dirty=true;
        }return true;
    }
    if(target<=TOC_FOLD && target>TOC_FOLD-7){
        unsigned index=menu.visible[TOC_FOLD-target];
        if(index/8<menu.collapsed_bytes){menu.collapsed[index/8]^=(uint8_t)(1u<<(index%8));menu.dirty=true;}return true;
    }
    if(target<=TOC_BASE && target>TOC_BASE-7){request_navigation(TOC,menu.visible[TOC_BASE-target]);return true;}
    if(target<=OPTION_BASE && target>OPTION_BASE-UI_SETTING_COUNT*16){
        unsigned code=(unsigned)(OPTION_BASE-target);ui_setting_id_t setting=(ui_setting_id_t)(code/16);
        if(!ui_settings_set(setting,code%16)){notice(BR(save_failed));return true;}
        if(menu.tab==WEIGHT)menu.tab=TYPE;else if(menu.tab==REFRESH)menu.tab=DISPLAY;
        menu.dirty=true;return true;
    }
    if(target==SIZE_DOWN || target==SIZE_UP){
        int size=(int)ui_settings_index(UI_SETTING_FONT_SIZE)+(target==SIZE_UP?1:-1);
        if(size>=0 && size<8 && !ui_settings_set(UI_SETTING_FONT_SIZE,(unsigned)size))notice(BR(save_failed));else menu.dirty=true;return true;
    }
    if(target==FULL_REFRESH){epd_app_request_full_refresh();return true;}
    if(target==TOUCH){menu.touch=!menu.touch;menu.dirty=true;return true;}
    if(target==LIGHT || target==LIGHT_TOGGLE){
        frontlight_snapshot_t light;frontlight_get(&light);
        unsigned value=target==LIGHT?(unsigned)lv_slider_get_value(menu.light):(light.committed_percent?0:menu.light_restore);
        if(!frontlight_commit((uint8_t)value))notice(BR(save_failed));else menu.dirty=true;return true;
    }
    if(target==CHAPTER_PREV || target==CHAPTER_NEXT){
        request_navigation(target,0);return true;
    }
    if(target==PAGE_PREV || target==PAGE_NEXT){
        request_navigation(target,0);return true;
    }
    if(target==SEEK){
        if(!ui_reader_view()->pages)return true;
        unsigned page=menu.jump;
        if(menu.jump_input){
            const char *text=lv_textarea_get_text(menu.jump_input);char *end=NULL;
            unsigned long value=strtoul(text,&end,10);
            if(!*text || !end || *end || value<1 || value>UI_READER_PAGE_MAX){notice(BR(page_range));return true;}
            page=(unsigned)value;
        }
        if(!page || (ui_reader_view()->pages && page>ui_reader_view()->pages)){notice(BR(page_range));return true;}
        request_navigation(SEEK,page);return true;
    }
    return false;
}

/* Called only by the UI-thread command consumer. No build or job polling. */
void books_menu_diagnostic(void)
{
    bool reader = current && current->id == UI_PAGE_READER;
    bool panel = reader && current->reader_panel;
    rt_kprintf("reader menu: menuopen=%u panel_tab=%d target_tab=%d pending_tab=%d dirty=%u font_ticket=%u\n",
        panel && current->foreground && !lv_obj_has_flag(current->reader_panel, LV_OBJ_FLAG_HIDDEN),
        panel ? menu.built_tab : 0, reader ? menu.tab : 0,
        reader && menu.dirty ? menu.tab : 0, reader && menu.dirty,
        reader ? (unsigned)menu.font_ticket : 0);
}
