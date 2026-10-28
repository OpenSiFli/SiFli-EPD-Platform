#include "notes_composer.h"
#include "platform/epd_app.h"
#include <string.h>
#include <stdio.h>

typedef struct { uint32_t block, begin, end; int32_t x, y, width, height; bool insertion; } text_line_t;
struct notes_composer {
    lv_obj_t *viewport, *canvas, *input;
    note_t *note;
    text_line_t *lines;
    size_t count, capacity;
    notes_compose_cb callback;
    void *user;
    notes_caret_t caret;
    uint32_t anchor, pressed_block;
    lv_point_t pressed;
    bool editable, selecting, moved, updating, hide_completed;
    int width, content_height;
};
/* The hidden textarea stores text and cursor positions; the canvas owns layout. */
static bool input_metrics(const lv_font_t *font,lv_font_glyph_dsc_t *glyph,uint32_t c,uint32_t next)
{
    (void)font;(void)c;(void)next;glyph->adv_w=1;return true;
}
static const lv_font_t input_font={.get_glyph_dsc=input_metrics,.line_height=1,.kerning=LV_FONT_KERNING_NONE};
static size_t next_char(const char *s,size_t at)
{ if(!s[at])return at; ++at;while(((unsigned char)s[at]&0xc0)==0x80)++at;return at; }
static uint32_t codepoint(const char *s,size_t at)
{
    const unsigned char *p=(const unsigned char*)s+at;uint32_t c=*p++;
    if(c<128)return c;unsigned n=c<224?1:c<240?2:3;c&=(1u<<(6-n))-1;
    while(n--)c=(c<<6)|(*p++&63);return c;
}
static uint32_t characters(const char *s,size_t bytes)
{uint32_t n=0;for(size_t p=0;p<bytes&&s[p];p=next_char(s,p))++n;return n;}
static unsigned flags_at(const note_block_t *b,size_t at)
{for(size_t i=0;i<b->span_count;i++)if(b->spans[i].start<=at&&at<b->spans[i].end)return b->spans[i].style;return 0;}
static const lv_font_t *font_at(const note_block_t *b,size_t at)
{return epd_app_font_style(b->kind==NOTE_HEADING?EPD_FONT_TITLE:EPD_FONT_BODY,flags_at(b,at)|(b->kind==NOTE_HEADING?NOTE_BOLD:0));}
static int advance(const lv_font_t *font,uint32_t c)
{lv_font_glyph_dsc_t g;return lv_font_get_glyph_dsc(font,&g,c,0)?g.adv_w:font->line_height/2;}
static bool attachment(const note_block_t *b)
{return b->kind==NOTE_IMAGE||b->kind==NOTE_AUDIO||b->kind==NOTE_RULE;}
static bool reserve_lines(notes_composer_t *e,size_t count)
{
    if(count>e->capacity){size_t cap=e->capacity?e->capacity:64;while(cap<count)cap*=2;
        void *p=note_alloc(cap*sizeof(*e->lines));if(!p)return false;
        if(e->count)memcpy(p,e->lines,e->count*sizeof(*e->lines));note_free(e->lines);e->lines=p;e->capacity=cap;}
    return true;
}
static bool add_line(notes_composer_t *e,text_line_t line)
{
    if(!reserve_lines(e,e->count+1))return false;
    e->lines[e->count++]=line;return true;
}
static bool layout_text(notes_composer_t *e,size_t block,size_t begin,int *y)
{
    note_block_t *b=e->note->blocks+block;
    int x=b->kind==NOTE_TASK?epd_ui_px(48):b->kind==NOTE_QUOTE?epd_ui_px(24):0;
    size_t p=begin;int used=0,height=font_at(b,p)->line_height;
    for(;;){
        uint32_t c=codepoint(b->text,p);const lv_font_t *f=font_at(b,p);int w=c=='\n'||!c?0:advance(f,c);
        if(!c||c=='\n'||(used&&used+w>e->width-x)){
            if(!add_line(e,(text_line_t){(uint32_t)block,(uint32_t)begin,(uint32_t)p,x,*y,used,height}))return false;
            *y+=height+epd_ui_px(8);if(!c)break;if(c=='\n')p=next_char(b->text,p);begin=p;used=0;height=font_at(b,p)->line_height;continue;
        }
        used+=w;if(f->line_height>height)height=f->line_height;p=next_char(b->text,p);
    }
    *y+=epd_ui_px(b->kind==NOTE_TASK?14:24);
    return true;
}
static void layout(notes_composer_t *e)
{
    e->count=0;int y=0;
    for(size_t i=0;i<e->note->count;i++){
        note_block_t *b=e->note->blocks+i;
        if(e->hide_completed&&b->kind==NOTE_TASK&&b->checked)continue;
        if(attachment(b)){
            if(!i||attachment(e->note->blocks+i-1)){int h=epd_ui_px(64);if(!add_line(e,(text_line_t){(uint32_t)i,0,0,0,y,e->width,h,true}))break;y+=h;}
            int h=epd_ui_px(b->kind==NOTE_RULE?32:112);if(!add_line(e,(text_line_t){(uint32_t)i,0,0,0,y,e->width,h}))break;y+=h+epd_ui_px(24);continue;}
        if(!layout_text(e,i,0,&y))goto done;
    }
    if(e->note->count&&attachment(e->note->blocks+e->note->count-1)){
        int h=epd_ui_px(64);add_line(e,(text_line_t){(uint32_t)e->note->count,0,0,0,y,e->width,h,true});y+=h;
    }
done:
    e->content_height=y;
    lv_obj_set_height(e->canvas,LV_MAX(y+epd_ui_px(70),lv_obj_get_height(e->viewport)));
    lv_obj_invalidate(e->canvas);
}
static size_t edit_prefix(notes_composer_t *e,const char *text)
{
    note_block_t *b=note_find(e->note,e->caret.block);size_t p=0;
    if(!b)return 0;
    while(b->text[p]&&b->text[p]==text[p])++p;
    while(p&&((unsigned char)b->text[p]&0xc0)==0x80)--p;
    return p;
}
static void layout_edit(notes_composer_t *e,size_t prefix)
{
    note_block_t *b=note_find(e->note,e->caret.block);
    if(!b||attachment(b)){layout(e);return;}
    size_t block=(size_t)(b-e->note->blocks),first=0,end;
    while(first<e->count&&(e->lines[first].block!=block||e->lines[first].insertion))++first;
    if(first==e->count){layout(e);return;}
    end=first;while(end<e->count&&e->lines[end].block==block&&!e->lines[end].insertion)++end;
    size_t start=first;
    while(start+1<end&&e->lines[start].end<prefix)++start;
    /* A deletion at a wrap boundary can pull text onto the previous line. */
    if(start>first)--start;
    int top=e->lines[start].y,y=top;
    text_line_t *last=e->lines+end-1;
    int old_bottom=last->y+last->height+epd_ui_px(8)+epd_ui_px(b->kind==NOTE_TASK?14:24);
    notes_composer_t changed={.note=e->note,.width=e->width};
    if(!layout_text(&changed,block,e->lines[start].begin,&y)||
       !reserve_lines(e,e->count-(end-start)+changed.count)){
        note_free(changed.lines);layout(e);return;
    }
    size_t tail=e->count-end,new_end=start+changed.count;
    memmove(e->lines+new_end,e->lines+end,tail*sizeof(*e->lines));
    memcpy(e->lines+start,changed.lines,changed.count*sizeof(*e->lines));
    note_free(changed.lines);
    int delta=y-old_bottom;e->count=new_end+tail;
    for(size_t i=new_end;i<e->count;++i)e->lines[i].y+=delta;
    e->content_height+=delta;
    lv_obj_set_height(e->canvas,LV_MAX(e->content_height+epd_ui_px(70),lv_obj_get_height(e->viewport)));
    lv_area_t dirty;lv_obj_get_coords(e->canvas,&dirty);
    if(!delta)dirty.y2=dirty.y1+y+epd_ui_px(12);
    dirty.y1+=top;
    lv_obj_invalidate_area(e->canvas,&dirty);
}
static void rect(lv_layer_t *layer,int x,int y,int w,int h,unsigned color,int radius,int border)
{
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);d.bg_opa=255;d.radius=radius;
    d.border_width=border;d.border_color=lv_color_hex(0xc8c8c8);d.border_opa=255;
    lv_area_t a={x,y,x+w-1,y+h-1};lv_draw_rect(layer,&d,&a);
}
static void text(lv_layer_t *layer,const char *s,int x,int y,int w,int h,const lv_font_t *font,unsigned color)
{
    lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);d.text=s;d.text_local=true;d.font=font;d.color=lv_color_hex(color);d.opa=255;
    lv_area_t a={x,y,x+w-1,y+h-1};lv_draw_label(layer,&d,&a);
}
static void draw(lv_event_t *event)
{
    notes_composer_t *e=lv_event_get_user_data(event);lv_layer_t *layer=lv_event_get_layer(event);lv_area_t area,visible;
    lv_obj_get_coords(e->canvas,&area);lv_obj_get_coords(e->viewport,&visible);
    visible.x1=LV_MAX(visible.x1,layer->_clip_area.x1);visible.y1=LV_MAX(visible.y1,layer->_clip_area.y1);
    visible.x2=LV_MIN(visible.x2,layer->_clip_area.x2);visible.y2=LV_MIN(visible.y2,layer->_clip_area.y2);
    if(visible.x1>visible.x2||visible.y1>visible.y2)return;
    for(size_t i=0;i<e->count;i++){
        text_line_t *line=e->lines+i;int y=area.y1+line->y;if(y>visible.y2)break;if(y+line->height+epd_ui_px(12)<visible.y1)continue;
        int x=area.x1+line->x;
        if(line->insertion){text(layer,"添加文字",x+4,y+epd_ui_px(12),e->width,line->height,epd_app_font_role(EPD_FONT_BODY),0x999999);continue;}
        note_block_t *b=e->note->blocks+line->block;
        if(attachment(b)){
            if(b->kind==NOTE_RULE){rect(layer,x,y+epd_ui_px(14),e->width,1,0xaaaaaa,0,0);continue;}
            rect(layer,x,y,e->width,line->height,0xf3f3f3,epd_ui_px(18),1);
            if(b->kind==NOTE_AUDIO){int height=epd_ui_px(30);for(int row=0;row<height;++row){int width=(height/2-LV_ABS(row-height/2))*4/3+1;rect(layer,x+epd_ui_px(28),y+epd_ui_px(40)+row,width,1,0x333333,0,0);}}
            else text(layer,"+",x+epd_ui_px(24),y+epd_ui_px(34),epd_ui_px(40),epd_ui_px(48),epd_app_font_role(EPD_FONT_TITLE),0x222222);
            text(layer,b->text,x+epd_ui_px(82),y+epd_ui_px(18),e->width-epd_ui_px(100),epd_ui_px(40),epd_app_font_role(EPD_FONT_BODY),0x222222);
            char detail[80];snprintf(detail,sizeof(detail),b->kind==NOTE_AUDIO?"%02lu:%02lu  ·  %u 个标记":"图片附件",(unsigned long)(b->milliseconds/60000),(unsigned long)(b->milliseconds/1000%60),(unsigned)b->mark_count);
            text(layer,detail,x+epd_ui_px(82),y+epd_ui_px(64),e->width-epd_ui_px(100),epd_ui_px(36),epd_app_font_role(EPD_FONT_SMALL),0x666666);continue;
        }
        if(!line->begin&&b->kind==NOTE_TASK){int n=epd_ui_px(30);rect(layer,area.x1,y+3,n,n,b->checked?0x444444:0xffffff,epd_ui_px(6),1);
            if(b->checked)text(layer,"✓",area.x1+2,y,n,n,epd_app_font_role(EPD_FONT_SMALL),0xffffff);}
        if(b->kind==NOTE_QUOTE)rect(layer,area.x1,y,epd_ui_px(3),line->height+epd_ui_px(8),0x888888,0,0);
        uint32_t a=e->caret.start,z=e->caret.end;if(a>z){uint32_t t=a;a=z;z=t;}
        for(size_t p=line->begin;p<line->end;){size_t end=next_char(b->text,p);const lv_font_t *font=font_at(b,p);int w=advance(font,codepoint(b->text,p));
            bool active=e->editable&&b->id==e->caret.block;
            if(active&&p>=a&&p<z)rect(layer,x,y,w,line->height,0xd4d4d4,0,0);
            if(active&&a!=z&&p==a){rect(layer,x,y,2,line->height,0x444444,0,0);rect(layer,x-4,y+line->height-1,10,10,0x444444,5,0);}
            if(active&&a==z&&p==e->caret.cursor)rect(layer,x,y,2,line->height,0x333333,0,0);
            char letter[5]={0};memcpy(letter,b->text+p,end-p);text(layer,letter,x,y,w+epd_ui_px(8),line->height+4,font,b->checked&&b->kind==NOTE_TASK?0x777777:b->kind==NOTE_QUOTE?0x555555:0x222222);x+=w;p=end;
            if(active&&a!=z&&p==z){rect(layer,x,y,2,line->height,0x444444,0,0);rect(layer,x-4,y+line->height-1,10,10,0x444444,5,0);}
        }
        if(e->editable&&b->id==e->caret.block&&e->caret.start==e->caret.end&&e->caret.cursor==line->end)
            rect(layer,x,y,2,line->height,0x333333,0,0);
        if(!*b->text)text(layer,e->note->count==1?"开始书写":"添加文字",x+4,y,e->width,epd_ui_px(44),epd_app_font_role(EPD_FONT_BODY),0x999999);
    }
}
static void sync_input(notes_composer_t *e)
{
    note_block_t *b=note_find(e->note,e->caret.block);
    if(!b||attachment(b)||(e->hide_completed&&b->kind==NOTE_TASK&&b->checked)){
        b=NULL;
        for(size_t i=0;i<e->note->count;i++){note_block_t *candidate=e->note->blocks+i;
            if(attachment(candidate)||(e->hide_completed&&candidate->kind==NOTE_TASK&&candidate->checked))continue;
            if(!b||candidate->id<e->caret.block)b=candidate;
        }
        e->caret=(notes_caret_t){.block=b?b->id:0,.scroll=-1};
    }
    e->updating=true;
    if(!b){lv_textarea_set_text(e->input,"");lv_obj_add_state(e->input,LV_STATE_DISABLED);e->updating=false;return;}
    lv_obj_remove_state(e->input,LV_STATE_DISABLED);
    size_t length=strlen(b->text);e->caret.cursor=LV_MIN(e->caret.cursor,length);e->caret.start=LV_MIN(e->caret.start,length);e->caret.end=LV_MIN(e->caret.end,length);
    while(e->caret.cursor&&((unsigned char)b->text[e->caret.cursor]&0xc0)==0x80)--e->caret.cursor;
    while(e->caret.start&&((unsigned char)b->text[e->caret.start]&0xc0)==0x80)--e->caret.start;
    while(e->caret.end&&((unsigned char)b->text[e->caret.end]&0xc0)==0x80)--e->caret.end;
    lv_obj_t *l=lv_textarea_get_label(e->input);uint32_t a=e->caret.start,z=e->caret.end;if(a>z){uint32_t t=a;a=z;z=t;}
    lv_textarea_set_text(e->input,b->text);lv_textarea_set_cursor_pos(e->input,characters(b->text,a==z?e->caret.cursor:z));
    lv_label_set_text_selection_start(l,a==z?LV_LABEL_TEXT_SELECTION_OFF:characters(b->text,a));
    lv_label_set_text_selection_end(l,a==z?LV_LABEL_TEXT_SELECTION_OFF:characters(b->text,z));e->updating=false;
}
static void ensure_caret(notes_composer_t *e)
{
    for(size_t i=0;i<e->count;i++) {text_line_t *l=e->lines+i;if(l->insertion)continue;note_block_t *b=e->note->blocks+l->block;
        if(b->id!=e->caret.block||e->caret.cursor<l->begin||e->caret.cursor>l->end)continue;
        int top=lv_obj_get_scroll_y(e->viewport),height=lv_obj_get_height(e->viewport);
        if(l->y<top)lv_obj_scroll_to_y(e->viewport,l->y,LV_ANIM_OFF);
        else if(l->y+l->height>top+height)lv_obj_scroll_to_y(e->viewport,l->y+l->height-height+epd_ui_px(16),LV_ANIM_OFF);break;}
}
static void input_changed(lv_event_t *event)
{
    notes_composer_t *e=lv_event_get_user_data(event);if(e->updating)return;
    const char *s=lv_textarea_get_text(e->input);size_t cursor=note_byte_offset(s,lv_textarea_get_cursor_pos(e->input));
    size_t prefix=LV_MIN(edit_prefix(e,s),LV_MIN(e->caret.start,e->caret.end));
    if(e->callback(COMPOSE_EDIT,e->caret.block,s,e->user)){
        e->caret.cursor=e->caret.start=e->caret.end=(uint32_t)cursor;layout_edit(e,prefix);ensure_caret(e);
    }else sync_input(e);
}
static void input_insert(lv_event_t *event)
{
    notes_composer_t *e=lv_event_get_user_data(event);if(e->updating||e->caret.start==e->caret.end)return;
    const char *old=lv_textarea_get_text(e->input),*insert=lv_event_get_param(event);
    size_t a=LV_MIN(e->caret.start,e->caret.end),z=LV_MAX(e->caret.start,e->caret.end),length=strlen(old);
    size_t bytes=insert[0]==LV_KEY_DEL&&!insert[1]?0:strlen(insert);
    lv_textarea_set_insert_replace(e->input,"");
    if(z>length||length-(z-a)+bytes>NOTE_TEXT_LIMIT)return;
    char *replacement=note_alloc(length-(z-a)+bytes+1);if(!replacement)return;
    memcpy(replacement,old,a);memcpy(replacement+a,insert,bytes);memcpy(replacement+a+bytes,old+z,length-z+1);
    if(e->callback(COMPOSE_EDIT,e->caret.block,replacement,e->user)){
        e->caret.cursor=e->caret.start=e->caret.end=(uint32_t)(a+bytes);layout_edit(e,a);sync_input(e);ensure_caret(e);
    }
    note_free(replacement);
    lv_textarea_set_insert_replace(e->input,"");
}
static text_line_t *hit(notes_composer_t *e,lv_point_t *p,uint32_t *offset)
{
    lv_area_t a,visible;lv_obj_get_coords(e->canvas,&a);lv_obj_get_coords(e->viewport,&visible);int y=p->y-a.y1,x=p->x-a.x1;
    if(x<0||x>=e->width||p->y<visible.y1||p->y>visible.y2)return NULL;
    for(size_t i=0;i<e->count;i++){text_line_t *l=e->lines+i;
        if(l->insertion){if(y>=l->y&&(y<l->y+l->height||i+1==e->count)){*offset=0;return l;}continue;}
        if(attachment(e->note->blocks+l->block)){
            if(y>=l->y&&y<l->y+l->height){*offset=0;return l;}continue;
        }
        if(y<l->y-epd_ui_px(12))continue;
        if(y>l->y+l->height+epd_ui_px(12)&&i+1<e->count)continue;
        note_block_t *b=e->note->blocks+l->block;int at=l->x;size_t q=l->begin;
        for(;q<l->end;q=next_char(b->text,q)){int w=advance(font_at(b,q),codepoint(b->text,q));if(x<at+w/2)break;at+=w;}
        *offset=(uint32_t)q;return l;}
    return NULL;
}
static void pointer(lv_event_t *event)
{
    notes_composer_t *e=lv_event_get_user_data(event);lv_indev_t *indev=lv_event_get_indev(event);if(!indev)return;
    lv_point_t p;lv_indev_get_point(indev,&p);lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){lv_obj_add_flag(e->viewport,LV_OBJ_FLAG_SCROLLABLE);return;}
    if(code==LV_EVENT_PRESSING&&(LV_ABS(p.x-e->pressed.x)>epd_ui_px(12)||LV_ABS(p.y-e->pressed.y)>epd_ui_px(12)))e->moved=true;
    uint32_t offset; text_line_t *line=hit(e,&p,&offset);if(!line)return;
    if(line->insertion){
        if(code==LV_EVENT_PRESSED){e->pressed=p;e->moved=e->selecting=false;}
        else if(code==LV_EVENT_CLICKED&&!e->moved&&!e->selecting)e->callback(COMPOSE_INSERT,line->block,NULL,e->user);
        return;
    }
    note_block_t *b=e->note->blocks+line->block;
    if(code==LV_EVENT_PRESSED){e->pressed=p;e->moved=false;e->selecting=false;e->pressed_block=b->id;e->anchor=offset;}
    else if(code==LV_EVENT_LONG_PRESSED&&e->editable&&!attachment(b)){
        e->selecting=true;e->caret.block=b->id;e->caret.start=offset;e->caret.end=(uint32_t)next_char(b->text,offset);e->caret.cursor=e->caret.end;
        lv_obj_remove_flag(e->viewport,LV_OBJ_FLAG_SCROLLABLE);sync_input(e);lv_obj_invalidate(e->canvas);
    }else if(code==LV_EVENT_PRESSING){
        if(LV_ABS(p.x-e->pressed.x)>epd_ui_px(12)||LV_ABS(p.y-e->pressed.y)>epd_ui_px(12))e->moved=true;
        if(e->selecting&&b->id==e->caret.block){e->caret.end=offset;e->caret.cursor=offset;sync_input(e);lv_obj_invalidate(e->canvas);}
    }else if(code==LV_EVENT_CLICKED&&!e->moved&&!e->selecting){
        lv_area_t a;lv_obj_get_coords(e->canvas,&a);
        if(b->kind==NOTE_TASK&&p.x<a.x1+epd_ui_px(42)){e->callback(COMPOSE_CHECK,b->id,NULL,e->user);layout(e);return;}
        if(attachment(b)){e->callback(COMPOSE_ATTACHMENT,b->id,NULL,e->user);return;}
        e->caret.block=b->id;e->caret.cursor=e->caret.start=e->caret.end=offset;sync_input(e);lv_obj_invalidate(e->canvas);
        e->callback(COMPOSE_FOCUS,b->id,NULL,e->user);
    }
}
static void deleted(lv_event_t *event)
{notes_composer_t *e=lv_event_get_user_data(event);note_free(e->lines);note_free(e);}
notes_composer_t *notes_composer_create(lv_obj_t *parent,note_t *note,bool editable,int width,int height,notes_compose_cb cb,void *user)
{
    notes_composer_t *e=note_alloc(sizeof(*e));if(!e)return NULL;e->note=note;e->editable=editable;e->width=width;e->callback=cb;e->user=user;
    e->viewport=lv_obj_create(parent);epd_app_object_init(e->viewport);lv_obj_set_size(e->viewport,width,height);lv_obj_add_flag(e->viewport,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(e->viewport,LV_DIR_VER);lv_obj_set_scrollbar_mode(e->viewport,LV_SCROLLBAR_MODE_OFF);
    e->canvas=lv_obj_create(e->viewport);epd_app_object_init(e->canvas);lv_obj_set_width(e->canvas,width);lv_obj_add_flag(e->canvas,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(e->canvas,draw,LV_EVENT_DRAW_MAIN,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_PRESSED,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_PRESSING,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_LONG_PRESSED,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_RELEASED,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_PRESS_LOST,e);lv_obj_add_event_cb(e->canvas,pointer,LV_EVENT_CLICKED,e);
    e->input=lv_textarea_create(parent);epd_app_object_init(e->input);lv_obj_set_size(e->input,width,1);lv_obj_set_style_opa(e->input,LV_OPA_TRANSP,0);lv_obj_set_style_text_font(e->input,&input_font,0);
    lv_textarea_set_text(e->input,"");lv_textarea_set_text_selection(e->input,true);lv_obj_add_event_cb(e->input,input_changed,LV_EVENT_VALUE_CHANGED,e);lv_obj_add_event_cb(e->input,input_insert,LV_EVENT_INSERT,e);
    lv_obj_add_event_cb(e->viewport,deleted,LV_EVENT_DELETE,e);layout(e);return e;
}
lv_obj_t *notes_composer_input(notes_composer_t *e){return e?e->input:NULL;}
void notes_composer_resize(notes_composer_t *e,int height){if(e){lv_obj_set_height(e->viewport,LV_MAX(epd_ui_px(100),height));ensure_caret(e);}}
void notes_composer_refresh(notes_composer_t *e){if(e){layout(e);sync_input(e);}}
void notes_composer_poll(notes_composer_t *e)
{
    if(!e||!e->editable||e->updating||e->caret.start!=e->caret.end)return;
    uint32_t cursor=(uint32_t)note_byte_offset(lv_textarea_get_text(e->input),lv_textarea_get_cursor_pos(e->input));
    if(cursor!=e->caret.cursor){e->caret.cursor=e->caret.start=e->caret.end=cursor;ensure_caret(e);lv_obj_invalidate(e->canvas);}
}
void notes_composer_caret(notes_composer_t *e,notes_caret_t *s,bool restore)
{if(!e)return;if(restore){e->caret=*s;sync_input(e);if(e->caret.scroll<0){lv_obj_update_layout(e->viewport);ensure_caret(e);}else lv_obj_scroll_to_y(e->viewport,e->caret.scroll,LV_ANIM_OFF);}else{*s=e->caret;s->scroll=lv_obj_get_scroll_y(e->viewport);}}
bool notes_composer_selection(notes_composer_t *e,uint32_t *id,uint32_t *a,uint32_t *z)
{if(!e||e->caret.start==e->caret.end)return false;*id=e->caret.block;*a=LV_MIN(e->caret.start,e->caret.end);*z=LV_MAX(e->caret.start,e->caret.end);return true;}
void notes_composer_scroll(notes_composer_t *e,int dir)
{if(e)lv_obj_scroll_to_y(e->viewport,LV_MAX(0,lv_obj_get_scroll_y(e->viewport)+dir*(lv_obj_get_height(e->viewport)-epd_ui_px(40))),LV_ANIM_OFF);}
void notes_composer_hide_completed(notes_composer_t *e,bool hide){if(e){e->hide_completed=hide;layout(e);}}
