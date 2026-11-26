#include "reader_controls.h"
#include "reader_ai.h"
#include "reader_marks.h"
#include <string.h>
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
       TOC_PREV=-525,TOC_NEXT=-526,TOC_CURRENT=-527,MANAGE_FONTS=-528,
       SEEK_PROGRESS=-529,NEGATIVE=-530,GUIDE_OFF=-531,GUIDE_SOLID=-532,GUIDE_DOTTED=-533,MARKS=-534,MARK_ADD=-535,MARK_NOTE=-536,
       MARK_EDIT=-537,MARK_PREV=-538,MARK_NEXT=-539,MARK_SAVE=-540,MARK_DELETE=-541,MARK_JUMP=-542,
       PAGE_NOTES=-543,NOTE_PREV=-544,NOTE_NEXT=-545,ANTIALIAS=-546,ROTATE=-547,
       NOTE_ROW=-730,MARK_ROW=-710,FONT_BASE=-600,TOC_BASE=-650,TOC_FOLD=-680,OPTION_BASE=-1000 };
enum { TOC_ROWS=7, TOC_ROW_HEIGHT=96, WIDE_TOC_ROW_HEIGHT=60 };
enum {
    WIDE_HEADER_HEIGHT=80, WIDE_NAV_WIDTH=168, WIDE_NAV_ITEM_HEIGHT=120,
    WIDE_INSET=24, WIDE_TOOLBAR_HEIGHT=72,
    WIDE_SHEET_WIDTH=HEIGHT-WIDE_NAV_WIDTH, WIDE_SHEET_HEIGHT=WIDTH-WIDE_HEADER_HEIGHT,
    WIDE_CONTENT_WIDTH=WIDE_SHEET_WIDTH-2*WIDE_INSET,
    WIDE_CONTENT_HEIGHT=WIDE_SHEET_HEIGHT-WIDE_TOOLBAR_HEIGHT-44,
    WIDE_COLUMN_WIDTH=(WIDE_CONTENT_WIDTH-WIDE_INSET)/2,
    WIDE_RIGHT_X=WIDE_COLUMN_WIDTH+WIDE_INSET,
    WIDE_EDITOR_WIDTH=WIDE_CONTENT_WIDTH-WIDTH-16
};
static struct {
    int tab, built_tab;
    unsigned displayed_appearance;
    unsigned first, font_first, jump, light_restore;
    unsigned mark_first, mark_index, note_first;
    books_mark_t editing;
    bool dirty, touch, font_failed, ai_panel, toc_pending, toc_locate, progress_by_percent;
    uint32_t locale, font_ticket;
    unsigned sheet_focus_start, toc_feedback;
    int navigation;
    bool mark_busy, mark_edit_pending;
    unsigned target;
    lv_obj_t *head,*dock,*bookshelf,*chapter,*tabs[5],*captions[5];
    lv_obj_t *sheet,*content,*error,*jump_input,*keyboard,*slider,*progress,*light;
    reading_font_info_t fonts[READING_FONT_LIMIT + 1];
    size_t font_count;
    uint8_t *collapsed;
    size_t collapsed_bytes;
    unsigned visible[TOC_ROWS],visible_count,toc_count;
} menu={.touch=true,.light_restore=45};

static bool menu_landscape(void){return current->reader_landscape;}
static int menu_width(void){return menu_landscape()?WIDE_CONTENT_WIDTH:CONTENT_WIDTH;}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int width)
{return label_create(parent,text,x,y,width,ui_font_body());}
static void style(lv_obj_t *obj,bool selected)
{
    lv_obj_set_style_radius(obj,20,0);
    lv_obj_set_style_border_width(obj,selected?2:1,0);
    lv_obj_set_style_border_color(obj,lv_color_hex(selected?0x222222:0x555555),0);
    lv_obj_set_style_bg_color(obj,lv_color_hex(selected?0x333333:0xffffff),0);
    lv_obj_set_style_shadow_width(obj,0,0);
}
lv_obj_t *books_icon_button(lv_obj_t *parent,int x,int y,int width,int height,book_icon_t icon,int target)
{
    lv_obj_t *button=button_create(parent,x,y,width,height,NULL,target);
    lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    lv_obj_t *image=icon_create(button,book_icon_source(icon),0,0);lv_obj_center(image);
    if(icon==BOOK_ICON_BACK || icon==BOOK_ICON_DOWN)lv_obj_set_ext_click_area(button,12);
    return button;
}
static lv_obj_t *choice(lv_obj_t *parent,const char *text,int x,int y,int width,int target,bool selected)
{
    lv_obj_t *button=button_create(parent,x,y,width,64,text,target);style(button,selected);
    lv_obj_t *text_obj=lv_obj_get_child(button,0);
    lv_obj_set_style_text_color(text_obj,lv_color_hex(selected?0xffffff:0x222222),0);
    const lv_font_t *font=menu_landscape() && width<256?ui_font_small():ui_font_body();
    lv_obj_set_style_text_font(text_obj,font,0);lv_obj_set_width(text_obj,width-16);
    if(font!=ui_font_body())lv_obj_set_height(text_obj,font->line_height);
    lv_label_set_long_mode(text_obj,LV_LABEL_LONG_DOT);return button;
}
static void disable(lv_obj_t *obj,bool disabled)
{if(disabled)lv_obj_add_state(obj,LV_STATE_DISABLED);else lv_obj_remove_state(obj,LV_STATE_DISABLED);}
static void notice(const char *text){if(menu.error)text_update(menu.error,text);}
static void row(const char *title,const char *value,int x,int y,int width,int target)
{
    bool wide=menu_landscape(), has_value=value && value[0];
    int height=wide?64:74;
    lv_obj_t *button=button_create(menu.content,x,y,width,height,NULL,target);
    lv_obj_set_style_border_width(button,wide?1:0,0);
    lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    if(wide){
        lv_obj_set_style_border_side(button,LV_BORDER_SIDE_BOTTOM,0);
        lv_obj_set_style_radius(button,0,0);
    }
    const lv_font_t *font=wide?ui_font_small():ui_font_body();
    int text_y=(height-font->line_height)/2;
    int value_x=wide?width-64-140:282;
    int title_width=wide?(has_value?value_x-16:width-72):270;
    label_create(button,title,8,text_y,title_width,font);
    if(!wide || has_value){
        lv_obj_t *v=label_create(button,value,value_x,text_y,wide?140:266,font);
        lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);lv_label_set_long_mode(v,LV_LABEL_LONG_DOT);
    }
    icon_create(button,book_icon_source(BOOK_ICON_NEXT),width-60,wide?8:12);
}
static void slider_event(lv_event_t *event)
{
    lv_obj_t *slider=lv_event_get_target_obj(event);
    if(lv_event_get_code(event)==LV_EVENT_VALUE_CHANGED && slider==menu.slider){
        menu.jump=(unsigned)lv_slider_get_value(slider);
        char text[48];
        if(menu.progress_by_percent) snprintf(text,sizeof(text),"%u%%",menu.jump);
        else snprintf(text,sizeof(text),"%u / %u",menu.jump,ui_reader_view()->pages);
        if(menu.progress)text_update(menu.progress,text);
        if(menu.jump_input && !menu.progress_by_percent){
            snprintf(text,sizeof(text),"%u",menu.jump);lv_textarea_set_text(menu.jump_input,text);
        }
    }
    if(lv_event_get_code(event)==LV_EVENT_RELEASED && pending_page==NAV_IDLE){
        if(slider==menu.light)pending_page=LIGHT;
        else if(slider==menu.slider)pending_page=menu.progress_by_percent?SEEK_PROGRESS:SEEK;
    }
}
static lv_obj_t *slider_create(lv_obj_t *parent,int x,int y,int width,int min,int max,int value)
{
    lv_obj_t *slider=lv_slider_create(parent);lv_obj_set_pos(slider,x,y);lv_obj_set_size(slider,width,14);
    if(menu_landscape())lv_obj_set_ext_click_area(slider,21);
    lv_slider_set_range(slider,min,max);lv_slider_set_value(slider,value,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider,lv_color_hex(0x666666),LV_PART_MAIN);
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
static void options(ui_setting_id_t setting,const char *const *labels,const unsigned *indices,unsigned count,
                    int x,int y,int span)
{
    unsigned value=ui_settings_index(setting);
    unsigned nearest=0;
    if(indices)for(unsigned i=1;i<count;++i)if(abs((int)value-(int)indices[i])<abs((int)value-(int)indices[nearest]))nearest=i;
    int width=(span-(int)(count-1)*10)/(int)count;
    for(unsigned i=0;i<count;++i)
        choice(menu.content,labels[i],x+(width+10)*(int)i,y,width,
               OPTION_BASE-(int)setting*16-(int)(indices?indices[i]:i),indices?nearest==i:value==i);
}
static int parent_tab(void)
{
    if(menu.tab==MARK_EDIT)return MARKS;
    if(menu.tab==MARKS || menu.tab==PAGE_NOTES)return PROGRESS;
    return menu.tab==FONTS || menu.tab==WEIGHT?TYPE:menu.tab==REFRESH || menu.tab==SLEEP?DISPLAY:0;
}
static const char *title(void)
{
    switch(menu.tab){case TOC:return BR(toc);case PROGRESS:return BR(progress);case DISPLAY:return BR(display);
    case TYPE:return BR(type);case FONTS:return BR(fonts);case WEIGHT:return BR(weight);case REFRESH:return BR(refresh);
    case SLEEP:return BR(sleep);case MARKS:return BR(marks);case MARK_EDIT:return BR(mark_edit);case PAGE_NOTES:return BR(page_notes);default:return BR(menu);}
}
static void destroy_panel(void)
{
    if(!current || !current->reader_panel)return;
    menu.built_tab=0;
    books_ai_ui_close();
    lv_obj_delete(current->reader_panel);current->reader_panel=NULL;
    while(current->focus_count>current->reader_panel_focus_start)current->focus_items[--current->focus_count]=NULL;
    menu.sheet=menu.content=menu.error=menu.keyboard=menu.jump_input=menu.slider=menu.light=menu.progress=NULL;
    menu.head=menu.dock=menu.bookshelf=menu.chapter=NULL;menu.ai_panel=false;
    memset(menu.tabs,0,sizeof(menu.tabs));memset(menu.captions,0,sizeof(menu.captions));
    current->reader_confirm_pending=false;
    if(current->back)lv_obj_add_flag(current->back,LV_OBJ_FLAG_HIDDEN);
}
void books_menu_rotation_changed(void)
{
    if(!current || current->id!=UI_PAGE_READER)return;
    destroy_panel();
    if(menu.tab)menu.dirty=true;
}
bool books_menu_active(void)
{
    return current && current->id==UI_PAGE_READER && current->foreground &&
        (menu.tab || (current->reader_panel && !lv_obj_has_flag(current->reader_panel,LV_OBJ_FLAG_HIDDEN)));
}
bool books_menu_pagination_active(void)
{
    return books_menu_active() && menu.tab == PROGRESS && menu.built_tab == PROGRESS;
}
bool books_menu_font_pending(void){return menu.font_ticket!=0;}
void books_menu_close(void)
{
    epd_reader_pagination_request(false);
    epd_reader_toc_prepare(false);
    destroy_panel();menu.tab=0;menu.navigation=0;menu.mark_edit_pending=false;
    epd_app_free(menu.collapsed);menu.collapsed=NULL;menu.collapsed_bytes=0;menu.first=0;menu.toc_locate=false;
    ui_reader_background_pause(menu.font_ticket ||
        (current && current->foreground && current->popup && !lv_obj_has_flag(current->popup,LV_OBJ_FLAG_HIDDEN)));
}
static void clear_sheet(void)
{
    /* The keyboard belongs to the overlay, not the sheet. */
    if(menu.keyboard)lv_obj_delete(menu.keyboard);
    if(menu.sheet)lv_obj_delete(menu.sheet);
    while(current->focus_count>menu.sheet_focus_start)current->focus_items[--current->focus_count]=NULL;
    menu.sheet=menu.content=menu.error=menu.keyboard=menu.jump_input=menu.slider=menu.light=menu.progress=NULL;
}
static void outside(lv_event_t *event)
{
    if(lv_event_get_target_obj(event)==current->reader_panel && pending_page==NAV_IDLE)pending_page=CLOSE;
}
static void keyboard_hide(void)
{
    if(menu.keyboard)lv_obj_add_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN);
    if(!menu_landscape() && menu.sheet && menu.tab==PROGRESS)lv_obj_set_y(menu.sheet,HEIGHT-108-744);
}
static void keyboard_show(bool number,lv_event_cb_t callback)
{
    if(!menu.keyboard){
        epd_keyboard_config_t config={.mode=number?EPD_KEYBOARD_NUMBER:EPD_KEYBOARD_TEXT,.chinese=!number};
        menu.keyboard=epd_keyboard_create(current->reader_panel,menu.jump_input,&config);
        if(!menu.keyboard){notice(BR(open_failed));return;}
        lv_obj_add_event_cb(menu.keyboard,callback,LV_EVENT_READY,NULL);
        lv_obj_add_event_cb(menu.keyboard,callback,LV_EVENT_CANCEL,NULL);
    }
    bool wide=menu_landscape();
    lv_obj_set_size(menu.keyboard,wide && number?WIDE_COLUMN_WIDTH:WIDTH,epd_keyboard_preferred_height(menu.keyboard));
    lv_obj_align(menu.keyboard,wide?LV_ALIGN_BOTTOM_RIGHT:LV_ALIGN_BOTTOM_MID,wide?-WIDE_INSET:0,0);
    lv_obj_remove_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN);
    if(!wide && number)lv_obj_set_y(menu.sheet,112);
}
static void jump_editor(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_CANCEL){keyboard_hide();return;}
    if(code==LV_EVENT_READY){if(pending_page==NAV_IDLE)pending_page=SEEK;return;}
    if(code==LV_EVENT_CLICKED)keyboard_show(true,jump_editor);
}
static bool progress_uses_percent(const ui_reader_view_t *view)
{
    return !view->page || !view->pages || view->pages > UI_READER_PAGE_MAX;
}

static void build_progress(bool full)
{
    const ui_reader_view_t *v=ui_reader_view();char text[80];
    bool wide=menu_landscape();
    int span=wide?WIDE_COLUMN_WIDTH:CONTENT_WIDTH, slider_x=wide?88:76, page_y=wide?52:74;
    menu.progress_by_percent=progress_uses_percent(v);
    menu.jump=menu.progress_by_percent?v->percent:v->page;
    if(menu.progress_by_percent)snprintf(text,sizeof(text),"%u%%",menu.jump);
    else snprintf(text,sizeof(text),"%u / %u",menu.jump,v->pages);
    menu.progress=label(menu.content,text,0,0,span);
    int maximum=menu.progress_by_percent?100:(int)LV_MAX(v->pages,2u);
    menu.slider=slider_create(menu.content,slider_x,page_y+26,span-2*slider_x,
                             menu.progress_by_percent?0:1,maximum,(int)menu.jump);
    disable(menu.slider,menu.progress_by_percent?!epd_reader_can_seek_progress():v->pages==1);
    lv_obj_t *previous=books_icon_button(menu.content,0,page_y,64,64,BOOK_ICON_BACK,PAGE_PREV);
    if(wide)lv_obj_set_ext_click_area(previous,0);
    disable(previous,v->page==1);
    disable(books_icon_button(menu.content,span-64,page_y,64,64,BOOK_ICON_NEXT,PAGE_NEXT),v->pages && v->page>=v->pages);
    if(!full)return;
    int jump_x=wide?WIDE_RIGHT_X:0, chapter_y=wide?76:160, chapter_gap=wide?16:20;
    int chapter_width=(span-chapter_gap)/2;
    disable(choice(menu.content,BR(chapter_prev),jump_x,chapter_y,chapter_width,CHAPTER_PREV,false),!document_view_active());
    disable(choice(menu.content,BR(chapter_next),jump_x+chapter_width+chapter_gap,chapter_y,chapter_width,CHAPTER_NEXT,false),!document_view_active());
    label(menu.content,BR(page),jump_x,wide?(70-ui_font_body()->line_height)/2:270,wide?96:175);
    menu.jump_input=lv_textarea_create(menu.content);
    lv_obj_set_pos(menu.jump_input,jump_x+(wide?104:180),wide?0:252);
    lv_obj_set_size(menu.jump_input,230,70);
    lv_obj_set_style_border_width(menu.jump_input,1,0);lv_obj_set_style_border_color(menu.jump_input,lv_color_hex(0x666666),0);
    lv_obj_set_style_radius(menu.jump_input,16,0);lv_obj_set_style_pad_all(menu.jump_input,12,0);
    lv_obj_set_style_text_font(menu.jump_input,ui_font_body(),0);lv_textarea_set_one_line(menu.jump_input,true);
    lv_textarea_set_accepted_chars(menu.jump_input,"0123456789");lv_textarea_set_max_length(menu.jump_input,10);
    if(v->page)snprintf(text,sizeof(text),"%u",v->page);else text[0]=0;
    lv_textarea_set_text(menu.jump_input,text);
    disable(menu.jump_input,!v->pages);
    lv_obj_add_event_cb(menu.jump_input,jump_editor,LV_EVENT_CLICKED,NULL);
    int go_x=wide?348:440;
    disable(choice(menu.content,BR(jump),jump_x+go_x,wide?3:256,span-go_x,SEEK,false),!v->pages);
    row(BR(marks),"",0,wide?148:356,span,MARKS);
    unsigned notes=epd_reader_note_count();
    if(notes){
        snprintf(text,sizeof(text),"%u",notes);
        row(BR(page_notes),text,0,wide?220:436,span,PAGE_NOTES);
    }
}
static void build_page_notes(void)
{
    bool wide=menu_landscape();
    int width=menu_width(), footer_y=wide?244:450;
    unsigned count=epd_reader_note_count();
    for(unsigned i=0;i<5 && menu.note_first+i<count;++i){
        char text[72];
        snprintf(text,sizeof(text),"%s %u",BR(note_item),menu.note_first+i+1u);
        row(text,"",wide?(int)(i%2)*WIDE_RIGHT_X:0,wide?(int)(i/2)*76:(int)i*84,
            wide?WIDE_COLUMN_WIDTH:width,NOTE_ROW-(int)i);
    }
    disable(books_icon_button(menu.content,8,footer_y,88,64,BOOK_ICON_BACK,NOTE_PREV),!menu.note_first);
    disable(books_icon_button(menu.content,width-96,footer_y,88,64,BOOK_ICON_NEXT,NOTE_NEXT),menu.note_first+5>=count);
}

static bool notes_action(int target)
{
    if(target==PAGE_NOTES){
        menu.note_first=0;menu.tab=PAGE_NOTES;menu.dirty=true;return true;
    }
    if(menu.tab!=PAGE_NOTES)return false;
    if(target==NOTE_PREV || target==NOTE_NEXT){
        if(target==NOTE_PREV && menu.note_first>=5)menu.note_first-=5;
        if(target==NOTE_NEXT && menu.note_first+5<epd_reader_note_count())menu.note_first+=5;
        menu.dirty=true;return true;
    }
    if(target<=NOTE_ROW && target>NOTE_ROW-5){
        unsigned index=menu.note_first+NOTE_ROW-target;
        books_menu_close();
        epd_reader_open_note(index);
        return true;
    }
    return false;
}

static void build_marks(void)
{
    enum { rows=4 };
    bool wide=menu_landscape();
    if(!books_marks_ready()){
        label(menu.content,books_marks_failed()?BR(open_failed):BR(marks_reading),8,30,CONTENT_WIDTH-16);
        return;
    }
    unsigned count;
    const books_mark_t *marks=books_marks_list(&count);
    bool busy=books_marks_busy();
    int side_x=CONTENT_WIDTH+WIDE_INSET, action_width=wide?WIDE_CONTENT_WIDTH-side_x:304;
    disable(choice(menu.content,BR(bookmark_add),wide?side_x:0,0,action_width,MARK_ADD,false),busy || count==BOOKS_MARK_LIMIT);
    disable(choice(menu.content,BR(mark_note),wide?side_x:316,wide?82:0,action_width,MARK_NOTE,false),busy);
    if(!count){label(menu.content,BR(marks_empty),8,wide?30:156,CONTENT_WIDTH-16);return;}
    if(menu.mark_first>=count)menu.mark_first=(count-1u)/rows*rows;
    for(unsigned i=0;i<rows && menu.mark_first+i<count;++i){
        const books_mark_t *mark=&marks[menu.mark_first+i];
        char caption[BOOKS_MARK_NOTE_BYTES + 24];
        snprintf(caption,sizeof(caption),"%u%%  %s",mark->percent,mark->note[0]?mark->note:mark->excerpt);
        choice(menu.content,caption,0,wide?82*i:82+88*i,CONTENT_WIDTH,MARK_ROW-(int)i,false);
    }
    int footer_y=wide?334:452;
    disable(books_icon_button(menu.content,8,footer_y,88,64,BOOK_ICON_BACK,MARK_PREV),!menu.mark_first);
    disable(books_icon_button(menu.content,CONTENT_WIDTH-96,footer_y,88,64,BOOK_ICON_NEXT,MARK_NEXT),menu.mark_first+rows>=count);
}

static void mark_keyboard_event(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_READY || code==LV_EVENT_CANCEL)keyboard_hide();
    else if(code==LV_EVENT_CLICKED)keyboard_show(false,mark_keyboard_event);
}

static void build_mark_editor(void)
{
    unsigned count;
    books_marks_list(&count);
    bool saved=menu.mark_index<count, wide=menu_landscape();
    /* Keep the excerpt, input and actions left of the text keyboard. */
    int width=wide?WIDE_EDITOR_WIDTH:CONTENT_WIDTH, action_width=wide?width:300;
    lv_obj_t *excerpt=label(menu.content,menu.editing.excerpt,8,4,width-16);
    if(wide)lv_obj_set_height(excerpt,LV_MIN(88,2*ui_font_body()->line_height));
    lv_label_set_long_mode(excerpt,LV_LABEL_LONG_DOT);
    menu.jump_input=lv_textarea_create(menu.content);
    lv_obj_set_pos(menu.jump_input,0,wide?96:60);
    lv_obj_set_size(menu.jump_input,width,wide?160:130);
    lv_obj_set_style_text_font(menu.jump_input,ui_font_body(),0);
    lv_textarea_set_max_length(menu.jump_input,(BOOKS_MARK_NOTE_BYTES-1u)/4u);
    lv_textarea_set_text(menu.jump_input,menu.editing.note);
    lv_obj_add_event_cb(menu.jump_input,mark_keyboard_event,LV_EVENT_CLICKED,NULL);
    choice(menu.content,BR(mark_save),0,wide?272:206,action_width,MARK_SAVE,false);
    if(saved){
        choice(menu.content,BR(mark_jump),wide?0:320,wide?344:206,action_width,MARK_JUMP,false);
        choice(menu.content,BR(mark_remove),0,wide?416:288,action_width,MARK_DELETE,false);
    }
}

static bool mark_action(int target)
{
    if(target==MARKS){
        menu.mark_first=0;
        if(!books_marks_request(ui_reader_file(),books_service)){
            notice(BR(open_failed));
            return true;
        }
        menu.tab=MARKS;menu.dirty=true;
        return true;
    }
    if(menu.tab!=MARKS && menu.tab!=MARK_EDIT)return false;
    unsigned count;
    const books_mark_t *marks=books_marks_list(&count);
    if(!marks || books_marks_busy())return true;
    if(target==MARK_PREV || target==MARK_NEXT){
        if(target==MARK_PREV)menu.mark_first=menu.mark_first>=4?menu.mark_first-4:0;
        else if(menu.mark_first+4<count)menu.mark_first+=4;
        menu.dirty=true;return true;
    }
    if(target==MARK_ADD || target==MARK_NOTE){
        books_mark_t mark={0};
        if(!epd_reader_position(&mark.source)){notice(BR(open_failed));return true;}
        mark.percent=ui_reader_view()->percent;
        if(!epd_reader_page_excerpt(mark.excerpt,sizeof(mark.excerpt)))
            epd_reader_chapter_title(mark.excerpt,sizeof(mark.excerpt),NULL);
        for(unsigned i=0;i<count;++i){
            if(!memcmp(&marks[i].source,&mark.source,sizeof(mark.source)) &&
               (target==MARK_NOTE || !marks[i].note[0])){
                menu.mark_index=i;menu.editing=marks[i];menu.tab=MARK_EDIT;menu.dirty=true;return true;
            }
        }
        if(count==BOOKS_MARK_LIMIT){notice(BR(mark_limit));return true;}
        if(target==MARK_ADD){
            if(!books_marks_submit(books_service,count,&mark))notice(BR(save_failed));
            else menu.dirty=true;
        }else{menu.mark_index=count;menu.editing=mark;menu.tab=MARK_EDIT;menu.dirty=true;}
        return true;
    }
    if(target<=MARK_ROW && target>MARK_ROW-4){
        unsigned index=menu.mark_first+MARK_ROW-target;
        if(index<count){menu.mark_index=index;menu.editing=marks[index];menu.tab=MARK_EDIT;menu.dirty=true;}
        return true;
    }
    if(target==MARK_JUMP){
        if(epd_reader_seek_source(&menu.editing.source))books_menu_close();
        else notice(BR(open_failed));
        return true;
    }
    if(target==MARK_SAVE || target==MARK_DELETE){
        snprintf(menu.editing.note,sizeof(menu.editing.note),"%s",lv_textarea_get_text(menu.jump_input));
        if(books_marks_submit(books_service,menu.mark_index,target==MARK_SAVE?&menu.editing:NULL)){
            menu.mark_edit_pending=true;
        }else notice(BR(save_failed));
        return true;
    }
    return false;
}

static void build_type(void)
{
    bool wide=menu_landscape();
    int span=wide?WIDE_COLUMN_WIDTH:CONTENT_WIDTH;
    label(menu.content,BR(font_size),0,0,300);
    disable(books_icon_button(menu.content,0,54,100,64,BOOK_ICON_MINUS,SIZE_DOWN),ui_settings_index(UI_SETTING_FONT_SIZE)==0);
    centered_label(menu.content,ui_settings_value(UI_SETTING_FONT_SIZE),110,60,span-220,ui_font_title());
    disable(books_icon_button(menu.content,span-100,54,100,64,BOOK_ICON_PLUS,SIZE_UP),ui_settings_index(UI_SETTING_FONT_SIZE)>=7);
    const char *lines[]={BR(tight),BR(medium),BR(loose)},*margins[]={BR(small),BR(medium),BR(large)};
    static const unsigned line_indices[]={1,3,4},margin_indices[]={0,2,4};
    int x=wide?WIDE_RIGHT_X:0, line_y=wide?0:140, margin_y=wide?146:278;
    label(menu.content,BR(line),x,line_y,span);
    options(UI_SETTING_LINE_SPACING,lines,line_indices,3,x,line_y+48,span);
    label(menu.content,BR(margin),x,margin_y,span);
    options(UI_SETTING_MARGIN,margins,margin_indices,3,x,margin_y+48,span);
    row(BR(fonts),ui_settings_value(UI_SETTING_FONT),0,wide?146:412,span,FONTS);
    row(BR(weight),ui_settings_value(UI_SETTING_FONT_WEIGHT),0,wide?222:492,span,WEIGHT);
}
static void build_fonts(void)
{
    bool wide=menu_landscape();
    size_t count=reading_font_list(menu.fonts,READING_FONT_LIMIT + 1);
    menu.font_count=LV_MIN(count,READING_FONT_LIMIT + 1);
    if(menu.font_first>=menu.font_count)menu.font_first=0;
    for(unsigned i=0;i<5 && menu.font_first+i<menu.font_count;++i){
        reading_font_info_t *f=&menu.fonts[menu.font_first+i];
        disable(choice(menu.content,f->name,0,82*i,CONTENT_WIDTH,FONT_BASE-(int)i,f->selected),!f->available || menu.font_ticket);
    }
    if(menu.font_count>5){
        disable(books_icon_button(menu.content,0,416,64,64,BOOK_ICON_BACK,FONT_PREV),!menu.font_first);
        disable(books_icon_button(menu.content,CONTENT_WIDTH-64,416,64,64,BOOK_ICON_NEXT,FONT_NEXT),menu.font_first+5>=menu.font_count);
    }
    int side_x=CONTENT_WIDTH+WIDE_INSET, action_width=wide?WIDE_CONTENT_WIDTH-side_x:300;
    choice(menu.content,BR(install),wide?side_x:0,wide?0:492,action_width,INSTALL,false);
    choice(menu.content,BR(font_manage),wide?side_x:320,wide?82:492,action_width,MANAGE_FONTS,false);
}
static void build_display(void)
{
    bool wide=menu_landscape();
    int span=wide?WIDE_COLUMN_WIDTH:CONTENT_WIDTH, right=wide?WIDE_RIGHT_X:0;
    frontlight_snapshot_t light;frontlight_get(&light);
    label(menu.content,BR(brightness),0,0,span);
    menu.light=slider_create(menu.content,24,wide?74:92,span-48,0,100,light.committed_percent);
    disable(menu.light,!light.available || light.locked);
    if(light.committed_percent)menu.light_restore=light.committed_percent;
    int button_width=(span-(wide?16:20))/2, action_y=wide?126:148;
    choice(menu.content,light.committed_percent?BR(light_off):BR(light_on),0,action_y,button_width,LIGHT_TOGGLE,false);
    choice(menu.content,BR(refresh_now),span-button_width,action_y,button_width,FULL_REFRESH,false);
    row(BR(refresh),ui_settings_value(UI_SETTING_FULL_REFRESH),0,wide?214:250,span,REFRESH);
    row(BR(sleep),"",0,wide?286:306,span,SLEEP);
    if(wide)row(BR(orientation),BR(landscape),right,0,span,ROTATE);
    row(BR(touch),menu.touch?BR(on):BR(off),right,wide?72:378,span,TOUCH);
    unsigned appearance=menu.displayed_appearance=ui_settings_index(UI_SETTING_READER_APPEARANCE);
    bool negative=(appearance & UI_READER_NEGATIVE)!=0;
    bool antialias=(appearance & UI_READER_ANTIALIAS)!=0;
    row(BR(negative),negative?BR(on):BR(off),right,wide?144:450,span,NEGATIVE);
    row(BR(antialias),antialias?BR(on):BR(off),right,wide?216:524,span,ANTIALIAS);
    label(menu.content,BR(guide),wide?right:8,wide?308:607,wide?span:130);
    const char *styles[]={BR(off),BR(guide_solid),BR(guide_dotted)};
    int guide_x=wide?right:140, guide_width=wide?(span-20)/3:150;
    for(unsigned i=0;i<3;++i)
        choice(menu.content,styles[i],guide_x+(guide_width+10)*(int)i,wide?356:600,guide_width,GUIDE_OFF-(int)i,
               (appearance & UI_READER_GUIDE_MASK)==2u*i);
    if(!wide)row(BR(orientation),BR(portrait),0,676,span,ROTATE);
}
static void build_options(void)
{
    int width=menu_width();
    if(menu.tab==WEIGHT){
        const char *names[]={BR(normal),BR(bold),BR(light)};
        options(UI_SETTING_FONT_WEIGHT,names,NULL,3,0,0,width);return;
    }
    if(menu.tab==REFRESH){
        static const unsigned values[]={UI_FULL_REFRESH_COUNTS};
        int option_width=(width-20)/3;
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){
            char text[64];snprintf(text,sizeof(text),"%u %s",values[i],BR(refresh_unit));
            choice(menu.content,text,(int)(i%3)*(option_width+10),(int)(i/3)*88,option_width,
                   OPTION_BASE-UI_SETTING_FULL_REFRESH*16-(int)i,ui_settings_index(UI_SETTING_FULL_REFRESH)==i);
        }
        return;
    }
    static const unsigned values[2][6]={{1,3,5,10,15,0},{1,5,15,30,60,0}};
    bool wide=menu_landscape();
    int span=wide?WIDE_COLUMN_WIDTH:width, option_width=(span-20)/3;
    for(unsigned kind=0;kind<2;++kind){
        ui_setting_id_t setting=kind?UI_SETTING_SYSTEM_SLEEP:UI_SETTING_TIMEOUT;
        int x=wide && kind?WIDE_RIGHT_X:0, y=!wide && kind?270:0;
        label(menu.content,kind?BR(system_sleep):BR(screen_sleep),x,y,span);
        for(unsigned i=0;i<6;++i){char text[48];
            if(values[kind][i])snprintf(text,sizeof(text),"%u %s",values[kind][i],BR(minute));else snprintf(text,sizeof(text),"%s",BR(never));
            choice(menu.content,text,x+(int)(i%3)*(option_width+10),y+52+(int)(i/3)*80,option_width,
                   OPTION_BASE-(int)setting*16-(int)i,ui_settings_index(setting)==i);
        }
    }
}
static bool toc_folded(unsigned index)
{return index/8<menu.collapsed_bytes && (menu.collapsed[index/8]&(1u<<(index%8)));}

static void toc_reveal(unsigned active)
{
    unsigned level=epd_reader_toc_level(active);
    /* Only open ancestors of the current chapter; preserve other folds. */
    for(unsigned index=active;index>0 && level;){
        unsigned parent=--index,depth=epd_reader_toc_level(parent);
        if(depth>=level)continue;
        if(parent/8<menu.collapsed_bytes)menu.collapsed[parent/8]&=(uint8_t)~(1u<<(parent%8));
        level=depth;
    }
}

static unsigned toc_collect(unsigned count,unsigned active,bool locate)
{
    unsigned rank=0,hidden=UINT_MAX,active_rank=UINT_MAX;
    memset(menu.visible,0xff,sizeof(menu.visible));
    for(unsigned index=0;index<count;++index){
        unsigned level=epd_reader_toc_level(index);
        if(hidden!=UINT_MAX && level>hidden)continue;
        hidden=toc_folded(index)?level:UINT_MAX;
        if(index==active)active_rank=rank;
        if(rank>=menu.first && rank-menu.first<TOC_ROWS)menu.visible[rank-menu.first]=index;
        ++rank;
    }
    unsigned first=menu.first;
    if(locate && active_rank!=UINT_MAX)first=active_rank/TOC_ROWS*TOC_ROWS;
    else if(first>=rank)first=rank?(rank-1)/TOC_ROWS*TOC_ROWS:0;
    if(first!=menu.first){
        menu.first=first;
        return toc_collect(count,active,false);
    }
    return rank;
}

static void build_toc(void)
{
    bool wide=menu_landscape();
    int width=menu_width(), row_height=wide?WIDE_TOC_ROW_HEIGHT:TOC_ROW_HEIGHT;
    int footer_y=TOC_ROWS*row_height, footer_height=wide?64:72;
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
    bool locate=menu.toc_locate && active<count;
    if(locate)toc_reveal(active);
    menu.visible_count=toc_collect(count,active,locate);
    if(navigation!=0)menu.toc_locate=false;
    int locate_x=wide?WIDE_SHEET_WIDTH-WIDE_INSET-64-16-280:292;
    lv_obj_t *locate_button=button_create(menu.sheet,locate_x,wide?4:16,280,64,BR(current),TOC_CURRENT);
    lv_obj_set_style_radius(locate_button,12,0);
    lv_obj_set_style_border_width(locate_button,1,0);
    lv_obj_set_style_border_color(locate_button,lv_color_hex(0x444444),0);
    disable(locate_button,active>=count);
    if(!count)centered_label(menu.content,navigation==0?BR(indexing):navigation<0?BR(open_failed):BR(no_toc),
                             0,wide?192:260,width,ui_font_body());
    for(unsigned i=0;i<TOC_ROWS;++i){
        unsigned index=menu.visible[i];
        if(index>=count)break;
        unsigned level=epd_reader_toc_level(index);
        char title_text[256];
        if(!document_view_toc_title(index,title_text,sizeof(title_text),&level))continue;
        bool children=index+1<count && epd_reader_toc_level(index+1)>level;
        bool selected=index==active;
        lv_obj_t *entry=panel_create(menu.content,0,row_height*(int)i,width,row_height);
        lv_obj_set_style_radius(entry,selected?10:0,0);
        lv_obj_set_style_border_width(entry,selected?0:1,0);
        lv_obj_set_style_border_side(entry,LV_BORDER_SIDE_BOTTOM,0);
        lv_obj_set_style_border_color(entry,lv_color_hex(0x666666),0);
        lv_obj_set_style_bg_color(entry,selected?lv_color_black():lv_color_white(),0);
        int text_width=children?width-64:width, inset=16+(int)LV_MIN(level,3u)*20;
        lv_obj_t *button=button_create(entry,0,0,text_width,row_height,NULL,TOC_BASE-(int)i);
        lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
        const lv_font_t *font=children?epd_app_font_style(EPD_FONT_BODY,EPD_FONT_STYLE_BOLD):ui_font_body();
        lv_obj_t *caption=label_create(button,wide?title_text:"",inset,0,text_width-inset-16,font);
        lv_obj_set_style_text_color(caption,selected?lv_color_white():lv_color_black(),0);
        lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_LEFT,0);
        if(!wide){
            lv_label_set_long_mode(caption,LV_LABEL_LONG_WRAP);lv_obj_set_height(caption,LV_SIZE_CONTENT);
            lv_label_set_text(caption,title_text);
            lv_obj_update_layout(caption);
            int max_height=LV_MIN(2,(row_height-16)/font->line_height)*font->line_height;
            lv_obj_set_height(caption,LV_MIN(lv_obj_get_height(caption),max_height));
        }
        lv_label_set_long_mode(caption,LV_LABEL_LONG_DOT);
        lv_obj_align(caption,LV_ALIGN_LEFT_MID,inset,0);
        if(children){
            lv_obj_t *fold=books_icon_button(entry,text_width,0,64,row_height,toc_folded(index)?BOOK_ICON_NEXT:BOOK_ICON_DOWN,TOC_FOLD-(int)i);
            lv_obj_set_ext_click_area(fold,0);
            disable(fold,index/8>=menu.collapsed_bytes);
            if(selected){lv_obj_t *icon=lv_obj_get_child(fold,0);
                lv_obj_set_style_image_recolor(icon,lv_color_white(),0);lv_obj_set_style_image_recolor_opa(icon,LV_OPA_COVER,0);}
        }
    }
    disable(books_icon_button(menu.content,0,footer_y+4,96,footer_height,BOOK_ICON_BACK,TOC_PREV),!menu.first);
    disable(books_icon_button(menu.content,width-96,footer_y+4,96,footer_height,BOOK_ICON_NEXT,TOC_NEXT),menu.first+TOC_ROWS>=menu.visible_count);
    if(menu.visible_count){
        char page[32];snprintf(page,sizeof(page),"%u / %u",menu.first/TOC_ROWS+1,(menu.visible_count+TOC_ROWS-1)/TOC_ROWS);
        int text_y=wide?4+(footer_height-ui_font_body()->line_height)/2:24;
        centered_label(menu.content,page,108,footer_y+text_y,width-216,ui_font_body());
    }
}
static void chapter_update(void)
{
    if(!menu.chapter)return;
    char text[256];
    if(!epd_reader_chapter_title(text,sizeof(text),NULL))snprintf(text,sizeof(text),"%s %u",BR(page),ui_reader_view()->page);
    if(!strcmp(lv_label_get_text(menu.chapter),text))return;
    text_update(menu.chapter,text);
}
static void build_frame(void)
{
    bool wide=menu_landscape();
    int width=wide?HEIGHT:WIDTH, height=wide?WIDTH:HEIGHT;
    current->reader_panel_focus_start=current->focus_count;
    lv_obj_t *root=panel_create(current->screen,0,0,width,height);current->reader_panel=root;
    lv_obj_set_style_bg_opa(root,wide?LV_OPA_COVER:LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(root,0,0);
    if(wide)lv_obj_set_style_radius(root,0,0);
    lv_obj_add_event_cb(root,outside,LV_EVENT_CLICKED,NULL);
    epd_app_input_group(root);
    if(menu.tab==AI){menu.ai_panel=true;books_ai_ui_open(root);return;}
    lv_obj_t *head=menu.head=panel_create(root,0,0,width,wide?WIDE_HEADER_HEIGHT:112);
    lv_obj_set_style_border_width(head,wide?1:0,0);
    if(wide){
        lv_obj_set_style_border_side(head,LV_BORDER_SIDE_BOTTOM,0);
        lv_obj_set_style_radius(head,0,0);
        lv_obj_t *back=books_icon_button(head,12,8,WIDE_NAV_WIDTH-24,64,BOOK_ICON_BACK,NAV_BACK);
        lv_obj_align(lv_obj_get_child(back,0),LV_ALIGN_LEFT_MID,0,0);
        lv_obj_set_ext_click_area(back,0);
        const lv_font_t *small=ui_font_small(), *book_font=ui_font_title();
        menu.bookshelf=label_create(back,epd_get_str(books_title),48,(64-small->line_height)/2,WIDE_NAV_WIDTH-72,small);
        label_create(head,ui_reader_view()->title,WIDE_NAV_WIDTH+WIDE_INSET,(WIDE_HEADER_HEIGHT-book_font->line_height)/2,
                     564,book_font);
        menu.chapter=label(head,"",784,(WIDE_HEADER_HEIGHT-ui_font_body()->line_height)/2,width-WIDE_INSET-784);
    }else{
        books_icon_button(head,12,20,72,72,BOOK_ICON_BACK,NAV_BACK);
        lv_obj_t *book=label(head,ui_reader_view()->title,104,22,548);lv_label_set_long_mode(book,LV_LABEL_LONG_DOT);
        menu.chapter=label(head,"",104,66,548);
    }
    lv_obj_t *dock=menu.dock=panel_create(root,0,wide?WIDE_HEADER_HEIGHT:HEIGHT-108,
                                         wide?WIDE_NAV_WIDTH:WIDTH,wide?WIDE_SHEET_HEIGHT:108);
    lv_obj_set_style_border_width(dock,1,0);
    if(wide){
        lv_obj_set_style_border_side(dock,LV_BORDER_SIDE_RIGHT,0);
        lv_obj_set_style_radius(dock,0,0);
    }
    const int tabs[]={TOC,PROGRESS,AI,DISPLAY,TYPE};
    const book_icon_t icons[]={BOOK_ICON_LIST,BOOK_ICON_PROGRESS,BOOK_ICON_AI,BOOK_ICON_SUN,BOOK_ICON_TYPE};
    const char *names[]={BR(toc),BR(progress_tab),BR(ai),BR(display_tab),BR(type_tab)};
    const lv_font_t *nav_font=wide?ui_font_small():ui_font_body();
    if(wide && 2*nav_font->line_height>WIDE_NAV_ITEM_HEIGHT-50)nav_font=ui_font_caption();
    for(unsigned i=0;i<5;++i){
        int item_width=wide?WIDE_NAV_WIDTH-1:(i==4?140:136);
        lv_obj_t *button=menu.tabs[i]=button_create(dock,wide?0:136*(int)i,wide?WIDE_NAV_ITEM_HEIGHT*(int)i:0,
                                                  item_width,wide?WIDE_NAV_ITEM_HEIGHT:108,NULL,tabs[i]);
        lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_radius(button,0,0);
        lv_obj_set_style_border_side(button,wide?LV_BORDER_SIDE_RIGHT:LV_BORDER_SIDE_BOTTOM,0);
        lv_obj_set_style_border_color(button,lv_color_black(),0);
        icon_create(button,book_icon_source(icons[i]),wide?(item_width-48)/2:44,wide?2:10);
        lv_obj_t *t=menu.captions[i]=centered_label(button,names[i],wide?8:2,wide?50:62,
                                                  wide?item_width-16:132,nav_font);
        if(wide)lv_obj_set_height(t,WIDE_NAV_ITEM_HEIGHT-50);
        lv_label_set_long_mode(t,wide?LV_LABEL_LONG_WRAP:LV_LABEL_LONG_DOT);
    }
    menu.sheet_focus_start=current->focus_count;
}
static void build(void)
{
    epd_reader_pagination_request(menu.tab == PROGRESS);
    if(menu.tab!=TOC)epd_reader_toc_prepare(false);
    /* AI owns a full-height overlay and keyboard; deleting its root also
     * releases objects that books_ai_ui_close only detaches from its state. */
    if(current->reader_panel && (menu.ai_panel || menu.tab==AI))destroy_panel();
    if(!current->reader_panel)build_frame();
    if(menu.tab==AI){menu.built_tab=AI;return;}
    clear_sheet();
    lv_obj_t *root=current->reader_panel;
    bool wide=menu_landscape();
    int parent=parent_tab(), selected_tab=parent?parent:menu.tab;
    if(wide && menu.tab==MARK_EDIT)selected_tab=PROGRESS;
    const int tabs[]={TOC,PROGRESS,AI,DISPLAY,TYPE};
    const char *names[]={BR(toc),BR(progress_tab),BR(ai),BR(display_tab),BR(type_tab)};
    for(unsigned i=0;i<5;++i){
        bool selected=selected_tab==tabs[i];
        int border=selected?3:0;
        if(lv_obj_get_style_border_width(menu.tabs[i],0)!=border){
            lv_obj_set_style_border_width(menu.tabs[i],border,0);
        }
        lv_color_t color=lv_color_hex(selected?0x000000:0x666666);
        if(!lv_color_eq(lv_obj_get_style_text_color(menu.captions[i],0),color))lv_obj_set_style_text_color(menu.captions[i],color,0);
        if(strcmp(lv_label_get_text(menu.captions[i]),names[i]))text_update(menu.captions[i],names[i]);
    }
    if(menu.bookshelf)text_update(menu.bookshelf,epd_get_str(books_title));
    bool child=parent!=0;
    int height=wide?WIDE_SHEET_HEIGHT:menu.tab==TOC?896:menu.tab==READER_MENU?320:menu.tab==DISPLAY?920:744;
    int sheet_width=wide?WIDE_SHEET_WIDTH:WIDTH;
    menu.sheet=panel_create(root,wide?WIDE_NAV_WIDTH:0,wide?WIDE_HEADER_HEIGHT:HEIGHT-108-height,sheet_width,height);
    lv_obj_set_style_radius(menu.sheet,wide?0:24,0);lv_obj_set_style_border_width(menu.sheet,wide?0:1,0);
    int back_x=wide?(child?WIDE_INSET:sheet_width-WIDE_INSET-64):(child?16:596), back_size=wide?64:72;
    lv_obj_t *back=books_icon_button(menu.sheet,back_x,wide?4:12,back_size,back_size,
                                   child?BOOK_ICON_BACK:BOOK_ICON_DOWN,child?BOOKS_MENU_BACK:CLOSE);
    epd_app_input_back(root,back);
    int title_x=wide?(child?104:WIDE_INSET):(child?100:32);
    int title_width=wide?(menu.tab==TOC?560:sheet_width-title_x-WIDE_INSET-(child?0:80)):
                         (menu.tab==TOC?240:child?480:540);
    label(menu.sheet,title(),title_x,wide?(WIDE_TOOLBAR_HEIGHT-ui_font_body()->line_height)/2:32,title_width);
    menu.content=panel_create(menu.sheet,wide?WIDE_INSET:32,wide?WIDE_TOOLBAR_HEIGHT:104,
                              menu_width(),wide?WIDE_CONTENT_HEIGHT:height-116);
    lv_obj_set_style_border_width(menu.content,0,0);lv_obj_set_style_bg_opa(menu.content,LV_OPA_TRANSP,0);
    if(menu.tab==TOC)build_toc();else if(menu.tab==PROGRESS || menu.tab==READER_MENU)build_progress(menu.tab==PROGRESS);
    else if(menu.tab==PAGE_NOTES)build_page_notes();
    else if(menu.tab==MARKS)build_marks();else if(menu.tab==MARK_EDIT)build_mark_editor();
    else if(menu.tab==TYPE)build_type();else if(menu.tab==FONTS)build_fonts();else if(menu.tab==DISPLAY)build_display();else build_options();
    if(wide){
        int width=menu.tab==MARK_EDIT?WIDE_EDITOR_WIDTH:menu.tab==PROGRESS?WIDE_COLUMN_WIDTH:WIDE_CONTENT_WIDTH;
        menu.error=label_create(menu.sheet,"",WIDE_INSET,WIDE_TOOLBAR_HEIGHT+WIDE_CONTENT_HEIGHT+2,width,ui_font_small());
    }else{
        menu.error=label(menu.content,"",0,menu.tab==TOC?TOC_ROWS*TOC_ROW_HEIGHT+78:height-164,CONTENT_WIDTH);
    }
    lv_label_set_long_mode(menu.error,LV_LABEL_LONG_DOT);
    if((menu.tab==MARKS || menu.tab==MARK_EDIT) && books_marks_ready() && books_marks_failed())
        notice(BR(save_failed));
    if(menu.font_ticket && menu.tab==FONTS)notice(BR(font_wait));
    else if(menu.font_failed && menu.tab==FONTS)notice(BR(font_failed));
    menu.built_tab=menu.tab;
}
void books_menu_open(void)
{
    if(!current || !ui_reader_view()->ready)return;
    menu.tab=menu_landscape()?PROGRESS:READER_MENU;
    menu.dirty=true;ui_reader_background_pause(true);books_menu_process();
}
bool books_reader_touch_enabled(void){return menu.touch;}
void books_menu_pause(void){epd_reader_pagination_request(false);epd_reader_toc_prepare(false);books_ai_cancel();books_ai_ui_close();menu.navigation=0;if(menu.tab)menu.dirty=true;ui_reader_background_pause(menu.font_ticket!=0);}
void books_menu_process(void)
{
    bool marks_changed=books_marks_process();
    if(marks_changed && menu.mark_edit_pending){
        menu.mark_edit_pending=false;
        if(menu.tab==MARK_EDIT){
            if(!books_marks_failed())menu.tab=MARKS;
            menu.dirty=true;
        }
    }
    if(menu.tab==MARKS){
        bool busy=books_marks_busy();
        if(marks_changed || busy!=menu.mark_busy)menu.dirty=true;
        menu.mark_busy=busy;
    }
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
    if(!epd_app_refresh_done())return;
    if(!menu.tab)return;
    if(menu.tab==DISPLAY && menu.displayed_appearance!=ui_settings_index(UI_SETTING_READER_APPEARANCE))menu.dirty=true;
    if(menu.tab==TOC){
        unsigned count=document_view_toc_count();
        bool finished=menu.toc_pending && epd_reader_toc_prepare(true)!=0;
        if(menu.toc_feedback!=count || finished){
            menu.toc_feedback=count;
            if(!menu.toc_count || finished)menu.dirty=true;
            else {char text[64];snprintf(text,sizeof(text),"%s: %u",BR(toc),count);notice(text);}
        }
    }
    if (menu.tab == PROGRESS && menu.built_tab == PROGRESS &&
        menu.progress_by_percent != progress_uses_percent(ui_reader_view()) &&
        (!menu.slider || !lv_obj_has_state(menu.slider, LV_STATE_PRESSED)) &&
        (!menu.keyboard || lv_obj_has_flag(menu.keyboard, LV_OBJ_FLAG_HIDDEN)))
        menu.dirty = true;
    if(menu.locale!=epd_locale_revision()){menu.locale=epd_locale_revision();menu.dirty=true;}
    if(menu.dirty){menu.dirty=false;build();}
    chapter_update();
    if(menu.tab==AI)books_ai_ui_process();
}
static bool navigate(int target,unsigned value)
{
    if(target==PAGE_PREV || target==PAGE_NEXT){ui_reader_turn(target==PAGE_PREV?-1:1);return true;}
    if(target==TOC)return document_view_toc_seek(value);
    if(target==CHAPTER_PREV || target==CHAPTER_NEXT)return epd_reader_chapter_move(target==CHAPTER_PREV?-1:1);
    if(target==SEEK_PROGRESS)return epd_reader_seek_progress(value);
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
        if(target==TOC && menu.tab!=TOC)menu.toc_locate=true;
        menu.tab=target;menu.dirty=true;return true;
    }
    if(!current->reader_panel)return false;
    if(books_ai_ui_action(target))return true;
    if(target==BOOKS_MENU_BACK){
        if(menu.tab==AI && books_ai_ui_back())return true;
        if(menu.keyboard && !lv_obj_has_flag(menu.keyboard,LV_OBJ_FLAG_HIDDEN)){keyboard_hide();return true;}
        int parent=parent_tab();if(parent){menu.tab=parent;menu.dirty=true;}else books_menu_close();return true;
    }
    if(target==CLOSE){books_menu_close();return true;}
    if(notes_action(target) || mark_action(target))return true;
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
    if(target==TOC_PREV || target==TOC_NEXT){
        menu.toc_locate=false;
        if(target==TOC_PREV)menu.first=menu.first>=TOC_ROWS?menu.first-TOC_ROWS:0;
        else if(menu.first+TOC_ROWS<menu.visible_count)menu.first+=TOC_ROWS;
        menu.dirty=true;return true;
    }
    if(target==TOC_CURRENT){
        menu.toc_locate=true;menu.dirty=true;return true;
    }
    if(target<=TOC_FOLD && target>TOC_FOLD-TOC_ROWS){
        unsigned index=menu.visible[TOC_FOLD-target];
        menu.toc_locate=false;
        if(index/8<menu.collapsed_bytes){menu.collapsed[index/8]^=(uint8_t)(1u<<(index%8));menu.dirty=true;}return true;
    }
    if(target<=TOC_BASE && target>TOC_BASE-TOC_ROWS){
        unsigned index=menu.visible[TOC_BASE-target];
        if(index<menu.toc_count)request_navigation(TOC,index);return true;
    }
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
    if(target==NEGATIVE || target==ANTIALIAS || (target<=GUIDE_OFF && target>=GUIDE_DOTTED)){
        unsigned appearance=ui_settings_index(UI_SETTING_READER_APPEARANCE);
        if(target==NEGATIVE)appearance^=UI_READER_NEGATIVE;
        else if(target==ANTIALIAS)appearance^=UI_READER_ANTIALIAS;
        else appearance=(appearance & ~(unsigned)UI_READER_GUIDE_MASK) | (unsigned)(GUIDE_OFF-target)*2u;
        if(!epd_reader_request_appearance(appearance))notice(BR(save_failed));
        return true;
    }
    if(target==ROTATE){
        if(menu.tab==DISPLAY && !books_reader_request_rotation(!menu_landscape()))notice(BR(open_failed));
        return true;
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
    if(target==SEEK_PROGRESS){
        if(menu.jump!=ui_reader_view()->percent)request_navigation(SEEK_PROGRESS,menu.jump);
        return true;
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
