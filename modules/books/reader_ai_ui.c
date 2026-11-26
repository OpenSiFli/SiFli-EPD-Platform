#include "reader_ai.h"
#include "reader_controls.h"
#include "../common/reader/reader.h"
#include "document/document_view.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_memory.h"
#include <ctype.h>

enum { AI_SEND=-800,AI_STOP=-801,AI_READ=-802,AI_VOICE=-803,AI_SERVICE=-804,
       AI_CONTEXT=-805,AI_SUMMARY=-806,AI_CHARACTER=-807,AI_BACKGROUND=-808,AI_RETRY=-809 };
static struct {
    lv_obj_t *root,*body,*input,*keyboard,*composer,*model,*state,*send,*speaker,*context,*quote,*welcome,*retry;
    lv_obj_t *questions[BOOK_AI_TURNS],*answers[BOOK_AI_TURNS];
    char *scratch,*page_text;
    unsigned count;
    uint32_t revision,generation,last_update;
    int message_width;
    bool landscape,keyboard_visible,expanded;
} ui;
static void keyboard_layout(bool visible)
{
    visible=visible && ui.keyboard;
    ui.keyboard_visible=visible;
    if(!ui.landscape){
        int top=visible?60:188;
        lv_obj_set_pos(ui.root,0,top);lv_obj_set_size(ui.root,684,1216-top);
        int height=ui.keyboard?epd_keyboard_preferred_height(ui.keyboard):0;
        if(ui.keyboard){
            lv_obj_set_size(ui.keyboard,684,height);
            lv_obj_align(ui.keyboard,LV_ALIGN_BOTTOM_MID,0,0);
        }
        int bottom=1216-top-(visible?height:0);
        lv_obj_set_pos(ui.composer,0,bottom-120);
        lv_obj_set_height(ui.body,bottom-380);
        lv_obj_set_y(ui.state,bottom-164);
        lv_obj_set_y(ui.retry,bottom-184);
    }
    if(ui.keyboard){
        if(visible)lv_obj_remove_flag(ui.keyboard,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui.keyboard,LV_OBJ_FLAG_HIDDEN);
    }
}
static void send_text(const char *question)
{
    if(!ui.input)return;
    if(!question)question=lv_textarea_get_text(ui.input);
    while(*question && isspace((unsigned char)*question))++question;
    if(!*question)return;
    if(books_ai_send(question)){
        lv_textarea_set_text(ui.input,"");keyboard_layout(false);
    }
}
static void editor_event(lv_event_t *event)
{
    if(!ui.root)return;
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_VALUE_CHANGED){books_ai_draft(lv_textarea_get_text(ui.input));return;}
    if(code==LV_EVENT_CLICKED){keyboard_layout(true);return;}
    if(code==LV_EVENT_READY){send_text(NULL);return;}
    if(code==LV_EVENT_CANCEL){books_ai_draft(lv_textarea_get_text(ui.input));keyboard_layout(false);}
    if(code==LV_EVENT_SCROLL_BEGIN){
        /* Keep native caret-follow, but skip intermediate scroll positions. */
        lv_anim_t *scroll=lv_event_get_param(event);
        if(scroll){scroll->start_value=scroll->end_value;scroll->duration=0;}
    }
}
static lv_obj_t *message_label(const char *text,bool question)
{
    lv_obj_t *obj=label_create(ui.body,text,0,0,ui.message_width,ui_font_small());
    lv_label_set_long_mode(obj,LV_LABEL_LONG_WRAP);lv_obj_set_height(obj,LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(obj,12,0);
    lv_obj_set_style_bg_color(obj,lv_color_white(),0);
    lv_obj_set_style_border_width(obj,question?2:0,0);
    lv_obj_set_style_border_color(obj,lv_color_hex(0x666666),0);
    lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);lv_obj_set_style_radius(obj,16,0);
    return obj;
}
static void empty_error(lv_obj_t *root,const char *error)
{
    label_create(root,error,32,200,620,ui_font_small());
}
void books_ai_ui_open(lv_obj_t *parent)
{
    memset(&ui,0,sizeof(ui));
    lv_obj_t *overlay=parent;
    lv_obj_update_layout(overlay);
    int width=lv_obj_get_width(overlay),height=lv_obj_get_height(overlay);
    bool landscape=width>height;
    ui.landscape=landscape;
    const int margin=24,gap=12,icon=72,body_pad=8;
    const int retry_width=114,retry_height=64;
    int top=landscape?32:188;
    int panel_height=(landscape?height:1216)-top;
    int bottom=panel_height-40; /* Keep the landscape controls clear of the bottom edge. */
    int header_y=landscape?8:14;
    int close_x=landscape?width-margin-icon:596;
    int compose_width=landscape?width*9/16:684;
    int chat_width=landscape?width-compose_width-3*margin:636;
    int compose_x=landscape?2*margin+chat_width:0;
    int row_y=landscape?92:104;
    ui.message_width=landscape?chat_width-2*body_pad:596;

    parent=panel_create(overlay,0,top,landscape?width:684,panel_height);ui.root=parent;
    lv_obj_set_style_radius(parent,28,0);lv_obj_set_style_border_width(parent,1,0);
    lv_obj_set_style_bg_color(parent,lv_color_white(),0);lv_obj_set_style_bg_opa(parent,LV_OPA_COVER,0);
    const lv_font_t *title=ui_font_body();
    label_create(parent,BR(ai),landscape?margin:32,landscape?header_y+(icon-title->line_height)/2:36,
                 landscape?chat_width-icon-gap:170,title);
    lv_obj_t *close=books_icon_button(parent,close_x,header_y,icon,icon,BOOK_ICON_CLOSE,BOOKS_MENU_BACK);
    epd_app_input_back(overlay,close);
    if(!books_ai_init(books_service)){empty_error(parent,BR(ai_unavailable));return;}
    ui.page_text=epd_app_alloc(BOOK_AI_CONTEXT,EPD_APP_PSRAM);
    ui.scratch=epd_app_alloc(EPD_AI_TEXT_LIMIT+1,EPD_APP_PSRAM);
    if(!ui.page_text || !ui.scratch){empty_error(parent,BR(ai_memory));return;}
    const ui_reader_view_t *reading=ui_reader_view();
    bool extracted=false;
    if(!ui_reader_waiting()){
        if(document_view_active())extracted=epd_reader_page_text(ui.page_text,BOOK_AI_CONTEXT);
        else if(reading->text && strlen(reading->text)<BOOK_AI_CONTEXT){strcpy(ui.page_text,reading->text);extracted=true;}
    }
    if(!extracted){empty_error(parent,BR(ai_extract));return;}
    if(!ui.page_text[0]){empty_error(parent,BR(ai_empty));return;}
    if(!books_ai_bind(ui_reader_identity(),reading->title,reading->page,ui.page_text)){empty_error(parent,BR(ai_busy));return;}
    books_ai_refresh_profiles();
    ui.speaker=books_icon_button(parent,landscape?margin+chat_width-icon:202,header_y,icon,icon,BOOK_ICON_SPEAKER,AI_READ);
    int model_width=landscape?compose_width-icon-gap:300;
    ui.model=button_create(parent,landscape?compose_x:290,landscape?header_y:20,
                           model_width,landscape?icon:64,BR(ai_configure),AI_SERVICE);
    lv_obj_set_style_border_width(ui.model,0,0);
    lv_obj_t *model_label=lv_obj_get_child(ui.model,0);lv_obj_set_width(model_label,model_width-16);
    lv_obj_set_style_text_font(model_label,ui_font_caption(),0);lv_label_set_long_mode(model_label,LV_LABEL_LONG_DOT);

    int context_height=landscape?84:106;
    int name_y=landscape?8:12;
    int context_width=chat_width-38;
    const lv_font_t *small=ui_font_small(),*caption=ui_font_caption();
    ui.context=button_create(parent,margin,row_y,chat_width,context_height,NULL,AI_CONTEXT);
    lv_obj_set_style_radius(ui.context,18,0);lv_obj_set_style_border_width(ui.context,1,0);
    lv_obj_t *name=label_create(ui.context,reading->title,16,name_y,context_width,small);
    lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
    char context[128];snprintf(context,sizeof(context),"%s %u · %s",BR(page),reading->page,BR(ai_context));
    label_create(ui.context,context,16,landscape?name_y+small->line_height+4:58,context_width,caption);

    int retry_x=margin+chat_width-retry_width;
    int retry_y=landscape?bottom-retry_height:1032;
    int chat_y=landscape?row_y+context_height+gap:224;
    int chat_height=landscape?LV_MAX(1,retry_y-gap-chat_y):836;
    ui.body=panel_create(parent,margin,chat_y,chat_width,chat_height);lv_obj_set_style_border_width(ui.body,0,0);
    lv_obj_add_flag(ui.body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(ui.body,LV_DIR_VER);
    lv_obj_set_flex_flow(ui.body,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_all(ui.body,body_pad,0);lv_obj_set_style_pad_row(ui.body,18,0);
    ui.quote=message_label(ui.page_text,true);lv_obj_add_flag(ui.quote,LV_OBJ_FLAG_HIDDEN);
    ui.welcome=panel_create(ui.body,0,0,ui.message_width,landscape?308:348);lv_obj_set_style_border_width(ui.welcome,0,0);
    label_create(ui.welcome,BR(ai_welcome),0,0,ui.message_width,ui_font_title());
    const char *suggestions[]={BR(ai_summary),BR(ai_character),BR(ai_background)};
    int suggestion_height=landscape?72:76;
    for(unsigned i=0;i<3;++i){
        int y=(landscape?56:66)+(suggestion_height+gap)*(int)i;
        lv_obj_t *b=button_create(ui.welcome,0,y,ui.message_width,suggestion_height,suggestions[i],AI_SUMMARY-(int)i);
        lv_obj_set_style_radius(b,18,0);lv_obj_t *text=lv_obj_get_child(b,0);
        lv_obj_set_style_text_font(text,small,0);lv_obj_set_width(text,ui.message_width-32);
        if(landscape)lv_obj_set_height(text,LV_SIZE_CONTENT);
        lv_label_set_long_mode(text,landscape?LV_LABEL_LONG_WRAP:LV_LABEL_LONG_DOT);
    }
    ui.state=label_create(parent,"",32,landscape?retry_y+(retry_height-caption->line_height)/2:1052,
                          landscape?retry_x-32-gap:506,caption);
    lv_label_set_long_mode(ui.state,LV_LABEL_LONG_DOT);
    ui.retry=button_create(parent,retry_x,retry_y,retry_width,retry_height,BR(retry),AI_RETRY);
    lv_obj_add_flag(ui.retry,LV_OBJ_FLAG_HIDDEN);

    int compose_y=landscape?row_y:1096,compose_height=landscape?108:120;
    int input_x=84,input_y=landscape?8:12,send_x=compose_width-84;
    int action_height=80,action_y=(compose_height-action_height)/2;
    ui.composer=panel_create(parent,compose_x,compose_y,compose_width,compose_height);
    lv_obj_set_style_border_width(ui.composer,1,0);
    books_icon_button(ui.composer,8,action_y,icon,action_height,BOOK_ICON_MIC,AI_VOICE);
    ui.input=lv_textarea_create(ui.composer);
    if(landscape){
        epd_app_object_init(ui.input);
        lv_obj_add_flag(ui.input,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(ui.input,lv_color_white(),0);
        lv_obj_set_style_bg_opa(ui.input,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(ui.input,lv_color_black(),0);
        lv_obj_set_style_anim_duration(ui.input,0,LV_PART_CURSOR);
        lv_obj_set_style_border_width(ui.input,2,LV_PART_CURSOR|LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ui.input,lv_color_black(),LV_PART_CURSOR);
        lv_obj_set_style_border_side(ui.input,LV_BORDER_SIDE_LEFT,LV_PART_CURSOR);
        lv_obj_add_event_cb(ui.input,editor_event,LV_EVENT_SCROLL_BEGIN,NULL);
    }
    lv_obj_set_pos(ui.input,input_x,input_y);
    lv_obj_set_size(ui.input,send_x-input_x-8,compose_height-2*input_y);
    lv_obj_set_style_text_font(ui.input,small,0);lv_obj_set_style_radius(ui.input,18,0);
    lv_obj_set_style_border_width(ui.input,1,0);lv_obj_set_style_border_color(ui.input,lv_color_hex(0x666666),0);
    lv_obj_set_style_pad_all(ui.input,12,0);
    lv_textarea_set_max_length(ui.input,BOOK_AI_INPUT_CHARS);lv_textarea_set_placeholder_text(ui.input,BR(ai_hint));
    books_ai_get_draft(ui.scratch,EPD_AI_TEXT_LIMIT+1);lv_textarea_set_text(ui.input,ui.scratch);
    lv_obj_add_event_cb(ui.input,editor_event,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(ui.input,editor_event,LV_EVENT_CLICKED,NULL);
    ui.send=books_icon_button(ui.composer,send_x,action_y,icon,action_height,BOOK_ICON_SEND,AI_SEND);
    epd_keyboard_config_t config={.mode=EPD_KEYBOARD_TEXT,.chinese=true,.voice=true};
    ui.keyboard=epd_keyboard_create(parent,ui.input,&config);
    if(ui.keyboard){
        if(landscape){
            /* Keep the composer and chat fixed when the keyboard is shown. */
            int y=compose_y+compose_height+gap;
            int available=LV_MAX(1,bottom-y);
            int preferred=epd_keyboard_preferred_height(ui.keyboard);
            lv_obj_set_pos(ui.keyboard,compose_x,y);
            lv_obj_set_size(ui.keyboard,compose_width,LV_CLAMP(1,preferred,available));
        }
        lv_obj_add_event_cb(ui.keyboard,editor_event,LV_EVENT_READY,NULL);
        lv_obj_add_event_cb(ui.keyboard,editor_event,LV_EVENT_CANCEL,NULL);
    }
    keyboard_layout(false);ui.revision=UINT32_MAX;books_ai_ui_process();
}
void books_ai_ui_process(void)
{
    if(!ui.root || !ui.body || !ui.scratch)return;
    book_ai_view_t view;books_ai_view(&view);
    if(view.revision==ui.revision || (view.busy && lv_tick_elaps(ui.last_update)<300))return;
    bool following=lv_obj_get_scroll_bottom(ui.body)<64;
    if(view.generation!=ui.generation){
        for(unsigned i=0;i<ui.count;++i){lv_obj_delete(ui.questions[i]);lv_obj_delete(ui.answers[i]);}
        memset(ui.questions,0,sizeof(ui.questions));memset(ui.answers,0,sizeof(ui.answers));ui.count=0;ui.generation=view.generation;
    }
    if(view.count)lv_obj_add_flag(ui.welcome,LV_OBJ_FLAG_HIDDEN);
    for(unsigned i=0;i<view.count;++i){
        bool created=i>=ui.count;
        if(i>=ui.count){
            books_ai_message(i,false,ui.scratch,EPD_AI_TEXT_LIMIT+1);ui.questions[i]=message_label(ui.scratch,true);
            ui.answers[i]=message_label("",false);++ui.count;
        }
        if(created || i+1==view.count || ui.revision==UINT32_MAX){
            books_ai_message(i,true,ui.scratch,EPD_AI_TEXT_LIMIT+1);text_update(ui.answers[i],ui.scratch);
        }
    }
    text_update(lv_obj_get_child(ui.model,0),view.authorized?view.model:BR(ai_configure));
    lv_obj_t *speaker=lv_obj_get_child(ui.speaker,0);
    lv_obj_set_style_opa(speaker,view.auto_read?LV_OPA_COVER:LV_OPA_80,0);
    lv_image_set_src(lv_obj_get_child(ui.send,0),book_icon_source(view.busy?BOOK_ICON_STOP:BOOK_ICON_SEND));
    if(view.count && (view.status==BOOK_AI_FAILED || view.status==BOOK_AI_STOPPED))lv_obj_remove_flag(ui.retry,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui.retry,LV_OBJ_FLAG_HIDDEN);
    const char *status=view.status==BOOK_AI_WAITING?BR(ai_wait):view.status==BOOK_AI_STREAMING?BR(ai_stream):view.status==BOOK_AI_STOPPED?BR(ai_stopped):"";
    char error[200];
    if(view.status==BOOK_AI_FAILED){snprintf(error,sizeof(error),"%s · %s · HTTP %d",BR(ai_failed),epd_ai_error_name(view.error),view.http);status=error;}
    else if(view.speech_error!=EPD_SPEECH_OK){snprintf(error,sizeof(error),"%s · %s",BR(ai_speech_error),epd_speech_error_name(view.speech_error));status=error;}
    text_update(ui.state,status);
    lv_obj_update_layout(ui.body);
    if(following && ui.count)lv_obj_scroll_to_view(ui.answers[ui.count-1],LV_ANIM_OFF);
    ui.revision=view.revision;ui.last_update=lv_tick_get();
}
void books_ai_ui_close(void)
{
    if(!ui.root)return;
    if(ui.input)books_ai_draft(lv_textarea_get_text(ui.input));
    books_ai_cancel();epd_app_free(ui.scratch);epd_app_free(ui.page_text);memset(&ui,0,sizeof(ui));
}
bool books_ai_ui_back(void)
{
    if(ui.keyboard && !lv_obj_has_flag(ui.keyboard,LV_OBJ_FLAG_HIDDEN)){
        books_ai_draft(lv_textarea_get_text(ui.input));keyboard_layout(false);return true;
    }
    return false;
}
bool books_ai_ui_action(int target)
{
    if(!ui.root || target>AI_SEND || target<AI_RETRY)return false;
    book_ai_view_t view;books_ai_view(&view);
    if(target==AI_SEND){if(view.busy)books_ai_cancel();else send_text(NULL);}
    else if(target==AI_STOP)books_ai_cancel();
    else if(target==AI_READ)books_ai_auto_read(!view.auto_read);
    else if(target==AI_SERVICE){if(!view.busy)books_ai_select_service();}
    else if(target==AI_CONTEXT && ui.quote){ui.expanded=!ui.expanded;if(ui.expanded)lv_obj_remove_flag(ui.quote,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(ui.quote,LV_OBJ_FLAG_HIDDEN);}
    else if(target==AI_VOICE && ui.keyboard && !view.busy){keyboard_layout(true);epd_keyboard_open_voice(ui.keyboard);}
    else if(target==AI_SUMMARY)send_text(BR(ai_summary));
    else if(target==AI_CHARACTER)send_text(BR(ai_character));
    else if(target==AI_BACKGROUND)send_text(BR(ai_background));
    else if(target==AI_RETRY)books_ai_retry();
    return true;
}
