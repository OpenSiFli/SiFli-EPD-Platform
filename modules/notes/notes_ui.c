#include "notes_runtime.h"
#include "notes_composer.h"
#include "system_time.h"
#include "platform/epd_app.h"
#include "platform/epd_app_font.h"
#include "platform/epd_input.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_locale.h"
#include "platform/epd_i18n.h"
#include "epd_file_picker.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

EPD_APP_DEFINE("notes");
MODULE_INIT_DEF(notes_runtime_init)
MODULE_CLEANUP_DEF(notes_runtime_cleanup)

typedef enum { LIST, READ, EDIT, MENU, AI_MENU, PREVIEW, IMAGE, BLOCK_MENU, META, SEARCH, NOTEBOOKS, BOOK_EDIT, MARKS, MARK_EDIT, VIEW_MENU, SORT_MENU, FILTER_MENU, FORMAT, INSERT_MENU, EXPORT_MENU, BLOCK_TITLE, BOOK_PICK, AUDIO_PLAYER, AUDIO_MENU } screen_t;
typedef enum {
    A_BACK=1, A_NEW, A_LIST, A_FILTER, A_GRID, A_SORT, A_SEARCH, A_BOOKS, A_OPEN,
    A_PREV, A_NEXT, A_EDIT, A_MENU, A_INSERT, A_TYPE, A_BOLD, A_ITALIC, A_UNDO, A_REDO,
    A_HIDE, A_KEYBOARD, A_SAVE, A_META, A_PIN, A_DELETE, A_CONFIRM, A_RESTORE, A_PURGE,
    A_IMPORT, A_EXPORT, A_ATTACH, A_RESCAN, A_AI_MENU, A_AI,
    A_GRANT, A_PREVIEW, A_ADOPT, A_REVERT, A_CANCEL_TRANSCRIBE, A_TASK_SELECT,
    A_REC, A_PAUSE, A_RESUME, A_STOP, A_MARK, A_PLAY, A_PLAY_STOP, A_TRANSCRIBE,
    A_BLOCK, A_CHECK, A_REMOVE, A_UP, A_DOWN, A_IMAGE, A_ORIGINAL, A_BOOK_SELECT,
    A_SELECT, A_BATCH_DELETE, A_TASK_CHECK, A_TASK_OPEN, A_COMPLETED,
    A_BOOK_NEW, A_BOOK_RENAME, A_BATCH_MOVE, A_DISCARD_AI, A_HOME_REC,
    A_DIRECTION, A_BATCH_EXPORT, A_BOOK_DELETE, A_BOOK_DELETE_CONFIRM,
    A_MARKS, A_MARK_EDIT, A_MARK_SAVE, A_SKIP, A_TRANSCRIPT_PLAY, A_HIDE_COMPLETED,
    A_VIEW_MENU, A_SORT_MENU, A_FILTER_MENU, A_FORMAT, A_INSERT_MENU, A_EXPORT_MENU, A_FOCUS, A_DATE_ORDER, A_AI_ORIGINAL, A_PICK_BOOK, A_CHOOSE_BOOK, A_CLEAR_SEARCH, A_INSERT_AT, A_AUDIO_MENU, A_SEEK
} action_t;
typedef struct { action_t action; uint32_t value; } binding_t;
typedef struct {
    lv_obj_t *screen, *body, *title, *notice, *input, *keyboard, *meta_book, *progress;
    lv_timer_t *timer;
    note_t note, preview, original;
    notes_status_t status;
    notes_ai_mode_t ai_mode;
    screen_t mode, return_mode;
    action_t action;
    uint32_t value, block, revision, scale, pending_document, confirm_id, ai_target;
    action_t confirm_action;
    binding_t bindings[256]; size_t binding_count;
    size_t offset, total;
    unsigned filter, sort;
    bool grid, rebuilding, pending, retired, attach_mode, selecting, needs_rebuild, completed, batch_move;
    uint32_t book_edit, task_target, batch_pending, mark_index;
    bool record_after_open, preferences_loaded, batch_export, hide_completed;
    unsigned batch_format;
    char destination[96];
    uint32_t selected[64]; size_t selected_count;
    char search[192], notebook[96];
    lv_image_dsc_t image;
    void *pixels;
    lv_area_t area;
    int width, height;
    notes_composer_t *composer;
    notes_caret_t caret;
    lv_obj_t *editor_host, *toolbar;
    lv_font_t *heading, *heading_fallback, *clock_font, *clock_fallback;
    lv_obj_t *audio_time, *audio_fill;
    bool edit_on_open, keyboard_wanted, show_original, caret_override, new_task;
    int composer_top;
    bool ai_authorized, adopt_pending, search_waiting, book_from_picker, book_rename_pending;
    char pending_book[96];
    uint32_t grant_ticket, ai_checked, search_due;
    char feedback[192];uint32_t feedback_until;
} notes_page_t;
static notes_page_t *page;
static void rebuild(void);
static bool commit_editor(void);
static void design_keyboard_layout(void);
static const char *ai_error_text(epd_ai_error_t error);

static const char *tr(const char *zh, const char *en)
{ return !strcmp(epd_locale_get(), "en_us") ? en : zh; }
static const char *kind_name(note_kind_t kind)
{
    static const char *zh[] = {"正文", "标题", "引用", "分隔线", "待办", "图片", "录音", "转写"};
    static const char *en[] = {"Text", "Heading", "Quote", "Divider", "Task", "Image", "Recording", "Transcript"};
    return tr(zh[kind], en[kind]);
}
static void notice_text(const char *text) { if (page && page->notice) { epd_app_text(page->notice, text); if(text&&*text)lv_obj_remove_flag(page->notice,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(page->notice,LV_OBJ_FLAG_HIDDEN); } }
static void message(const char *text){if(!page)return;snprintf(page->feedback,sizeof(page->feedback),"%s",text?text:"");page->feedback_until=lv_tick_get()+4000;notice_text(text);}
static void event(lv_event_t *event)
{
    const binding_t *binding = lv_event_get_user_data(event);
    if (!page) return;
    if (page->retired) { if(binding&&binding->action==A_BACK)gui_app_goback();return; }
    if (binding) { page->action = binding->action; page->value = binding->value; }
}
static binding_t *bind(action_t action, uint32_t value)
{
    if (page->binding_count >= sizeof(page->bindings) / sizeof(page->bindings[0])) return NULL;
    binding_t *b = &page->bindings[page->binding_count++]; *b = (binding_t){action,value}; return b;
}
static void notebook_event(lv_event_t *event)
{
    if (!page || page->retired) return;
    page->action = A_BOOK_SELECT; page->value = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width, bool small)
{
    lv_obj_t *out = epd_app_label(parent, text, x, y, width, epd_app_font_role(small ? EPD_FONT_SMALL : EPD_FONT_BODY));
    lv_label_set_long_mode(out, LV_LABEL_LONG_WRAP); lv_obj_set_height(out, LV_SIZE_CONTENT); return out;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int width, int height, action_t action, uint32_t value)
{
    lv_obj_t *out = epd_app_button(parent, x, y, width, height, text,
        action == A_BOOK_SELECT ? notebook_event : event,
        action == A_BOOK_SELECT ? (void *)(uintptr_t)value : bind(action, value));
    lv_obj_set_style_radius(out, epd_ui_px(12), 0); lv_obj_set_style_border_width(out, 1, 0);
    lv_obj_t *caption = lv_obj_get_child(out, 0);
    lv_obj_set_style_text_font(caption, epd_app_font_role(EPD_FONT_SMALL), 0);
    lv_obj_set_width(caption, width - epd_ui_px(12)); lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT); lv_obj_center(caption);
    return out;
}
static void menu_button(const char *text, action_t action, uint32_t value, unsigned index)
{
    int gap = epd_ui_px(12), w = (page->width-gap)/2, h = epd_ui_touch(76);
    button(page->body, text, (index%2)*(w+gap), (index/2)*(h+gap), w,h,action,value);
}
static void keyboard_close(void)
{
    if (!page || !page->keyboard) return;
    epd_keyboard_set_textarea(page->keyboard, NULL); lv_obj_delete(page->keyboard); page->keyboard = NULL;
}
static void keyboard_event(lv_event_t *e)
{
    (void)e; if (page) page->action = A_HIDE;
}
static void keyboard_open(lv_obj_t *input)
{
    keyboard_close(); if (!input) return;
    page->keyboard = epd_keyboard_create(page->screen, input, NULL);
    if (!page->keyboard) return;
    lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_CANCEL, NULL);
    epd_keyboard_refresh(page->keyboard);
    int h = epd_keyboard_preferred_height(page->keyboard);
    lv_obj_set_pos(page->keyboard, 0, lv_obj_get_height(page->screen)-h);
    lv_obj_set_size(page->keyboard, lv_obj_get_width(page->screen), h);
    design_keyboard_layout();
}
static void input_event(lv_event_t *e)
{
    if (!page || page->rebuilding) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        page->action = A_KEYBOARD; page->value = lv_event_get_target_obj(e) == page->meta_book;
    } else { page->pending = true; if(page->mode==SEARCH)page->search_due=lv_tick_get()+600; }
}
static lv_obj_t *textarea(const char *text, int x, int y, int width, int height, bool one_line)
{
    lv_obj_t *out = lv_textarea_create(page->body); epd_app_object_init(out);
    lv_obj_set_pos(out,x,y); lv_obj_set_size(out,width,height);
    lv_obj_set_style_bg_color(out,lv_color_white(),0); lv_obj_set_style_bg_opa(out,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(out,1,0); lv_obj_set_style_pad_all(out,epd_ui_px(12),0);
    lv_obj_set_style_text_font(out,epd_app_font_role(EPD_FONT_BODY),0);
    lv_textarea_set_one_line(out,one_line); lv_textarea_set_text(out,text);
    lv_textarea_set_text_selection(out,true);
    lv_obj_add_event_cb(out,input_event,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(out,input_event,LV_EVENT_CLICKED,NULL); return out;
}
typedef struct { action_t action; uint32_t id,value,start,end; const char *text,*book; } edit_t;
static bool edit_note(note_t *n, void *argument)
{
    edit_t *edit = argument; note_block_t *b = note_find(n,edit->id);
    switch (edit->action) {
    case A_EDIT: return note_edit(n,edit->id,edit->text);
    case A_MARK_SAVE:
        if(!b||edit->value>=b->mark_count||strlen(edit->text)>=sizeof(b->marks[0].title))return false;
        strcpy(b->marks[edit->value].title,edit->text);return true;
    case A_INSERT: return note_insert(n,n->count,(note_kind_t)edit->value,"") != NULL;
    case A_INSERT_AT: return edit->value<=n->count&&note_insert(n,edit->value,NOTE_TEXT,"")!=NULL;
    case A_TYPE: if (!b || b->kind == NOTE_AUDIO || b->kind == NOTE_IMAGE) return false; b->kind = (note_kind_t)edit->value; return true;
    case A_BOLD: case A_ITALIC: return note_style(n,edit->id,edit->start,edit->end,edit->action==A_BOLD?NOTE_BOLD:NOTE_ITALIC);
    case A_PIN: n->pinned = !n->pinned; return true;
    case A_CHECK: if (!b) return false; b->checked = !b->checked; return true;
    case A_REMOVE: return note_remove(n,edit->id);
    case A_UP: case A_DOWN: return note_move(n,edit->id,edit->action==A_UP?-1:1);
    case A_ORIGINAL: return b && b->original && note_edit(n,b->id,b->original);
    case A_META:
        if (strlen(edit->text)>=sizeof(n->title) || strlen(edit->book)>=sizeof(n->notebook)) return false;
        strcpy(n->title,edit->text); strcpy(n->notebook,edit->book); return true;
    default: return false;
    }
}
static bool mutate(edit_t edit, bool coalesce)
{
    if (!notes_mutate(edit_note,&edit,coalesce)) { message(tr("修改尚未保存，请保留页面后重试。","Change not saved. Keep this page and try again.")); return false; }
    notes_document(&page->note); page->needs_rebuild = true; return true;
}
static bool commit_editor(void)
{
    if (!page || !page->pending || !page->input) return true;
    if (page->composer) return true;
    if (page->mode != EDIT && page->mode != META) return true;
    edit_t edit = {.action=A_EDIT,.id=page->block,.text=lv_textarea_get_text(page->input)};
    if(page->mode==META){edit.action=A_META;edit.book=page->note.notebook;}
    bool ok = edit.action == A_EDIT ? notes_edit_text(edit.id, edit.text, &page->note) : notes_edit_metadata(edit.text, edit.book, &page->note);
    if (!ok) { message(tr("文字尚未保存","Text has not been saved")); return false; }
    page->pending = false; return true;
}
bool notes_ui_flush(void) { return commit_editor(); }
static bool command(notes_command_t kind, uint32_t id, uint32_t arg, const char *text)
{
    if (!commit_editor()) return false;
    if (!notes_command(kind,id,arg,text)) {
        message(tr("当前操作尚未完成，请稍后再试。","Please wait for the current operation.")); return false;
    }
    return true;
}
static bool selected(uint32_t id)
{ for(size_t i=0;i<page->selected_count;++i) if(page->selected[i]==id) return true; return false; }
static bool picked_file(const char *path,void *context)
{
    if(!page||page!=context||page->retired)return true;
    if(!command(page->attach_mode?N_ATTACH:N_IMPORT,0,0,path))return false;
    page->pending_document=1;return true;
}
static void menus_render(void)
{
    unsigned i=0;
    if(page->mode==MENU&&page->return_mode==LIST){
        menu_button(tr("导入文件","Import"),A_IMPORT,0,i++);menu_button(tr("刷新 TF 卡","Refresh TF card"),A_RESCAN,0,i++);
        menu_button(page->grid?tr("列表视图","List view"):tr("网格视图","Grid view"),A_GRID,0,i++);
        menu_button(tr("按修改时间","Modified first"),A_SORT,0,i++);menu_button(tr("按创建时间","Created first"),A_SORT,1,i++);
        menu_button(tr("按标题","By title"),A_SORT,2,i++);menu_button(tr("多选","Select notes"),A_SELECT,0,i++);
        menu_button(tr("切换升降序","Reverse order"),A_DIRECTION,0,i++);
        menu_button(tr("移动所选到回收站","Trash selected notes"),A_BATCH_DELETE,0,i++);
        menu_button(tr("移动所选到笔记本","Move selected to notebook"),A_BATCH_MOVE,0,i++);
        menu_button(tr("导出所选 TXT","Export selected TXT"),A_BATCH_EXPORT,0,i++);
        menu_button(tr("导出所选 Markdown","Export selected Markdown"),A_BATCH_EXPORT,1,i++);
        menu_button(tr("导出所选完整 ZIP","Export selected full ZIP"),A_BATCH_EXPORT,2,i++);
        if(page->notebook[0]&&strcmp(page->notebook,"\1")){
            menu_button(tr("重命名当前笔记本","Rename current notebook"),A_BOOK_RENAME,0,i++);
            menu_button(tr("删除当前笔记本","Delete current notebook"),A_BOOK_DELETE,0,i++);
        }
        return;
    }
    if(page->mode==BLOCK_MENU){
        note_block_t *b=note_find(&page->note,page->block);if(!b)return;
        if(b->kind==NOTE_AUDIO){
            menu_button(tr("播放","Play"),A_PLAY,0,i++);menu_button(tr("暂停播放","Pause playback"),A_PLAY_STOP,0,i++);
            menu_button(tr("后退 10 秒","Back 10 seconds"),A_SKIP,0,i++);menu_button(tr("前进 10 秒","Forward 10 seconds"),A_SKIP,1,i++);
            menu_button(tr("重命名录音","Rename recording"),A_EDIT,b->id,i++);
            menu_button(tr("转写 / 继续转写","Transcribe / Continue"),A_TRANSCRIBE,0,i++);
            menu_button(tr("录音标记","Recording markers"),A_MARKS,0,i++);
        }else if(b->kind==NOTE_IMAGE)menu_button(tr("查看图片","View image"),A_IMAGE,0,i++);
        else {
            menu_button(tr("编辑文字","Edit text"),A_EDIT,b->id,i++);
            const note_kind_t kinds[]={NOTE_TEXT,NOTE_HEADING,NOTE_QUOTE,NOTE_TASK,NOTE_RULE};
            for(unsigned k=0;k<5;++k)menu_button(kind_name(kinds[k]),A_TYPE,kinds[k],i++);
            if(b->kind==NOTE_TRANSCRIPT&&b->original){menu_button(tr("恢复原始转写","Restore original transcript"),A_ORIGINAL,0,i++);
                menu_button(tr("播放对应录音","Play this segment"),A_TRANSCRIPT_PLAY,0,i++);}
            menu_button(tr("整理整篇笔记","AI for this note"),A_AI_MENU,b->id,i++);
        }
        menu_button(tr("上移","Move up"),A_UP,0,i++);menu_button(tr("下移","Move down"),A_DOWN,0,i++);
        if(!page->status.recording)menu_button(tr("删除这一块","Remove block"),A_REMOVE,0,i++);return;
    }
    menu_button(tr("标题和笔记本","Title & notebook"),A_META,0,i++);
    menu_button(page->hide_completed?tr("显示已完成项目","Show completed tasks"):tr("折叠已完成项目","Hide completed tasks"),A_HIDE_COMPLETED,0,i++);
    menu_button(page->note.pinned?tr("取消置顶","Unpin"):tr("置顶","Pin"),A_PIN,0,i++);
    menu_button(tr("添加段落","Add paragraph"),A_INSERT,NOTE_TEXT,i++);menu_button(tr("添加待办","Add task"),A_INSERT,NOTE_TASK,i++);
    menu_button(tr("插入图片","Insert image"),A_ATTACH,0,i++);menu_button(tr("开始录音","Record"),A_REC,0,i++);
    menu_button(tr("导出 TXT","Export TXT"),A_EXPORT,0,i++);menu_button(tr("导出 Markdown","Export Markdown"),A_EXPORT,1,i++);
    menu_button(tr("导出完整笔记 ZIP","Export full ZIP"),A_EXPORT,2,i++);menu_button(tr("恢复 AI 修改前的版本","Restore before AI"),A_REVERT,0,i++);
    menu_button(page->note.deleted?tr("恢复笔记","Restore note"):tr("移到回收站","Move to trash"),page->note.deleted?A_RESTORE:A_DELETE,page->note.id,i++);
    if(page->note.deleted)menu_button(tr("永久删除","Delete permanently"),A_PURGE,page->note.id,i++);
}
static void image_render(void)
{
    uint8_t *gray=NULL;unsigned width,height;
    if(notes_take_image(&gray,&width,&height)){
        note_free(page->pixels);page->pixels=NULL;unsigned stride=(width*2+3)&~3u;
        page->pixels=note_alloc(stride*height+31);
        if(page->pixels){
            uint8_t *pixels=(uint8_t*)(((uintptr_t)page->pixels+31)&~(uintptr_t)31);
            for(unsigned y=0;y<height;++y){uint16_t *line=(uint16_t*)(pixels+y*stride);for(unsigned x=0;x<width;++x){unsigned value=gray[y*width+x];line[x]=(uint16_t)(((value>>3)<<11)|((value>>2)<<5)|(value>>3));}}
            page->image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,.w=width,.h=height,.stride=stride},.data_size=stride*height,.data=pixels};
            lv_draw_buf_t buffer={.data=pixels,.data_size=stride*height};epd_app_clean_draw_buffer(&buffer);
        }
        note_free(gray);
    }
    if(page->pixels){lv_obj_t *image=lv_image_create(page->body);lv_image_set_src(image,&page->image);lv_obj_center(image);}
}
#include "notes_design.inc"

static void rebuild(void)
{
    if(!page||page->retired)return;
    if(!commit_editor())return;
    char *draft=NULL;
    if(page->pending&&page->input&&(page->mode==SEARCH||page->mode==BOOK_EDIT||page->mode==MARK_EDIT||page->mode==BLOCK_TITLE)){
        const char *t=lv_textarea_get_text(page->input);draft=note_string(t,strlen(t));if(!draft)return;
    }
    if(page->composer&&!page->caret_override)notes_composer_caret(page->composer,&page->caret,false);
    page->caret_override=false;
    bool reopen=(page->mode==EDIT||page->mode==SEARCH||page->mode==BOOK_EDIT||page->mode==META||page->mode==MARK_EDIT||page->mode==BLOCK_TITLE)&&(page->keyboard||page->keyboard_wanted);
    page->rebuilding=true;keyboard_close();page->composer=NULL;page->input=page->meta_book=page->progress=page->toolbar=page->editor_host=NULL;
    page->audio_time=page->audio_fill=NULL;
    lv_obj_clean(page->body);page->binding_count=0;
    page->width=lv_obj_get_width(page->screen);page->height=lv_obj_get_height(page->screen);
    lv_obj_set_pos(page->body,0,0);lv_obj_set_size(page->body,page->width,page->height);
    lv_obj_remove_flag(page->body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(page->title,LV_OBJ_FLAG_HIDDEN);
    if(page->scale!=epd_ui_scale_revision()||!page->heading){
        if(page->heading)epd_app_font_destroy(page->heading);if(page->heading_fallback)epd_app_font_destroy(page->heading_fallback);
        uint32_t identity=0;page->heading_fallback=NULL;page->heading=epd_app_font_create(0,epd_ui_px(44),0,&identity,&page->heading_fallback);
        if(page->clock_font)epd_app_font_destroy(page->clock_font);if(page->clock_fallback)epd_app_font_destroy(page->clock_fallback);
        page->clock_font=page->clock_fallback=NULL;
    }
    if((page->mode==AUDIO_PLAYER||page->mode==AUDIO_MENU)&&!page->clock_font){uint32_t identity=0;page->clock_font=epd_app_font_create(0,epd_ui_px(86),0,&identity,&page->clock_fallback);}
    switch(page->mode){
    case LIST:if(page->filter==1)design_tasks();else design_list();break;
    case META:design_meta();break;
    case BOOK_PICK:design_book_picker();break;
    case SEARCH:design_search_page(draft);break;
    case BOOK_EDIT:design_book_edit(draft);break;
    case READ:case EDIT:design_note(page->mode==EDIT);break;
    case AUDIO_PLAYER:design_audio();break;
    case AUDIO_MENU:design_audio();design_audio_menu();break;
    case BLOCK_TITLE:case MARK_EDIT:design_audio_name(draft);break;
    case MARKS:design_marks();break;
    case NOTEBOOKS:{size_t offset=page->offset;page->offset=0;design_list();page->offset=offset;design_notebooks();break;}
    case VIEW_MENU:case SORT_MENU:case FILTER_MENU:design_list();design_options();break;
    case FORMAT:design_note(true);design_format();break;
    case PREVIEW:design_note(false);design_preview();break;
    case MENU:if(page->return_mode==LIST){design_list();design_options();}else{design_note(page->return_mode==EDIT);design_note_menu();}break;
    case AI_MENU:design_note(page->return_mode==EDIT);design_ai();break;
    case INSERT_MENU:design_note(true);design_insert();break;
    case EXPORT_MENU:design_note(false);design_export();break;
    default:{
        design_background(0xffffff);design_topbar(false);
        lv_obj_t *outer=page->body;page->body=box(outer,epd_ui_px(32),epd_ui_px(174),page->width-epd_ui_px(64),page->height-epd_ui_px(222),0,0xffffff,0);
        int ow=page->width,oh=page->height;page->width-=epd_ui_px(64);page->height-=epd_ui_px(222);
        switch(page->mode){
        case IMAGE:image_render();break;
        default:lv_obj_add_flag(page->body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(page->body,LV_DIR_VER);menus_render();break;
        }
        page->width=ow;page->height=oh;page->body=outer;break;
    }}
    design_confirm();
    lv_obj_set_pos(page->notice,epd_ui_px(32),epd_ui_px(130));lv_obj_set_size(page->notice,page->width-epd_ui_px(64),epd_ui_px(30));
    lv_obj_set_style_text_font(page->notice,epd_app_font_role(EPD_FONT_CAPTION),0);lv_obj_set_style_text_color(page->notice,lv_color_hex(0x555555),0);
    note_free(draft);page->rebuilding=false;page->needs_rebuild=false;page->scale=epd_ui_scale_revision();
    if(reopen&&page->input)keyboard_open(page->input);page->keyboard_wanted=false;
}
static void page_previous(void)
{
    if(page->composer&&(page->mode==READ||page->mode==EDIT)){notes_composer_scroll(page->composer,-1);return;}
    else if(page->mode==PREVIEW){page->offset=page->offset>=8?page->offset-8:0;}
    else{size_t n=(page->mode==NOTEBOOKS||page->mode==BOOK_PICK||page->mode==SEARCH)?8:page->mode==MARKS?5:page->filter==1?12:page->grid?6:5;page->offset=page->offset>=n?page->offset-n:0;}page->needs_rebuild=true;
}
static void page_next(void)
{
    if(page->composer&&(page->mode==READ||page->mode==EDIT)){notes_composer_scroll(page->composer,1);return;}
    else if(page->mode==PREVIEW){if(page->offset+8<page->preview.count)page->offset+=8;}
    else{size_t n=(page->mode==NOTEBOOKS||page->mode==BOOK_PICK||page->mode==SEARCH)?8:page->mode==MARKS?5:page->filter==1?12:page->grid?6:5;if(page->offset+n<page->total)page->offset+=n;}page->needs_rebuild=true;
}
static void do_action(action_t action,uint32_t value)
{
    if(!commit_editor())return;
    if(page->composer){notes_composer_caret(page->composer,&page->caret,false);if(page->caret.block)page->block=page->caret.block;}
    edit_t edit={.action=action,.id=page->block,.value=value};
    switch(action){
    case A_BACK:
        if(page->keyboard){keyboard_close();design_keyboard_layout();return;}
        if(page->confirm_action){page->confirm_action=0;break;}
        if(page->mode==VIEW_MENU||page->mode==SORT_MENU||page->mode==FILTER_MENU){page->mode=MENU;page->return_mode=LIST;break;}
        if(page->mode==FORMAT||page->mode==INSERT_MENU){page->mode=EDIT;break;}
        if(page->mode==AI_MENU){page->mode=page->return_mode;break;}
        if(page->mode==AUDIO_MENU||page->mode==MARKS||page->mode==BLOCK_TITLE){note_block_t *b=note_find(&page->note,page->block);page->pending=false;page->mode=b&&b->kind==NOTE_AUDIO?AUDIO_PLAYER:READ;break;}
        if(page->mode==MARK_EDIT){page->pending=false;page->mode=MARKS;break;}
        if(page->mode==BOOK_PICK){page->mode=page->batch_move?LIST:META;page->batch_move=false;break;}
        if(page->mode==BOOK_EDIT&&page->book_from_picker){page->mode=BOOK_PICK;page->book_from_picker=false;break;}
        if(page->mode==LIST&&page->filter==1){page->filter=0;page->offset=0;break;}
        if(page->mode==LIST){notes_save_now();gui_app_goback();return;}
        page->pending=false;page->confirm_action=0;page->mode=(page->mode==READ||page->mode==SEARCH||page->mode==NOTEBOOKS||page->mode==BOOK_EDIT)?LIST:page->mode==MENU?page->return_mode:READ;break;
    case A_NEW:if(command(N_NEW,0,value,value==1||page->notebook[0]=='\1'?"":page->notebook)){page->pending_document=1;page->edit_on_open=true;page->new_task=value==1;}break;
    case A_HOME_REC:if(command(N_NEW,0,2,page->notebook[0]=='\1'?"":page->notebook)){page->pending_document=1;page->record_after_open=true;}break;
    case A_OPEN:if(command(N_OPEN,value,0,NULL))page->pending_document=value;break;
    case A_FILTER:if(value!=1||command(N_TASKS,0,0,NULL)){if(page->mode==NOTEBOOKS)page->notebook[0]=0;page->filter=value;page->offset=0;page->mode=LIST;}break;
    case A_GRID:page->grid=value!=0;command(N_PREFERENCES,0,page->sort|(page->grid?8:0),NULL);page->offset=0;page->mode=LIST;break;
    case A_SORT:page->sort=(page->sort&4)|value;command(N_PREFERENCES,0,page->sort|(page->grid?8:0),NULL);page->offset=0;page->mode=SORT_MENU;break;
    case A_DIRECTION:page->sort^=4;command(N_PREFERENCES,0,page->sort|(page->grid?8:0),NULL);page->offset=0;page->mode=SORT_MENU;break;
    case A_SEARCH:page->mode=SEARCH;page->keyboard_wanted=true;break;
    case A_BOOKS:page->offset=0;page->mode=NOTEBOOKS;break;
    case A_BOOK_SELECT:{char book[1][96];
        if(value==UINT32_MAX)strcpy(page->notebook,"\1");
        else if(!value)page->notebook[0]=0;
        else{if(notes_books(book,value-1,1)<value){message(tr("笔记本已不存在","Notebook no longer exists"));return;}strcpy(page->notebook,book[0]);}
        page->offset=0;page->mode=LIST;page->filter=0;page->selecting=false;page->selected_count=0;
        page->batch_pending=0;page->batch_move=false;page->batch_export=false;page->book_rename_pending=false;break;}
    case A_BOOK_NEW:page->book_from_picker=page->mode==BOOK_PICK;page->book_edit=0;if(!page->book_from_picker)page->batch_move=false;page->mode=BOOK_EDIT;break;
    case A_BOOK_RENAME:{char book[1][96];page->book_edit=0;for(size_t i=0;i<notes_books(book,i,1);++i)if(!strcmp(book[0],page->notebook)){page->book_edit=(uint32_t)i+1;break;}
        page->batch_move=false;page->mode=BOOK_EDIT;break;}
    case A_BOOK_DELETE:page->confirm_action=action;break;
    case A_BOOK_DELETE_CONFIRM:{char book[1][96];for(size_t i=0;i<notes_books(book,i,1);++i)if(!strcmp(book[0],page->notebook)){
        if(command(N_BOOK_DELETE,(uint32_t)i,value,NULL)){page->notebook[0]=0;page->mode=LIST;page->confirm_action=0;}break;}break;}
    case A_BATCH_MOVE:if(!page->selected_count)return;page->batch_move=true;page->batch_export=false;page->book_edit=0;page->offset=0;page->mode=BOOK_PICK;break;
    case A_BATCH_EXPORT:if(!page->selected_count)return;page->batch_export=true;page->batch_move=false;page->batch_format=value;page->selecting=false;page->mode=LIST;break;
    case A_COMPLETED:page->completed=!page->completed;page->offset=0;break;
    case A_HIDE_COMPLETED:page->hide_completed=!page->hide_completed;page->caret.scroll=0;page->caret_override=true;page->mode=READ;break;
    case A_TASK_CHECK:case A_TASK_OPEN:{notes_task_t task;
        bool complete=(value&0x80000000u)!=0;value&=0x7fffffffu;
        if(notes_tasks_query(&task,value,1,complete,page->search)<=value)break;
        if(action==A_TASK_CHECK)command(N_TASK_CHECK,task.note,task.block,NULL);
        else if(command(N_OPEN,task.note,0,NULL)){page->pending_document=task.note;page->task_target=task.block;}
        break;}
    case A_PREV:page_previous();break;case A_NEXT:page_next();break;
    case A_EDIT:
        if(value)page->block=value;
        else if(!page->block&&page->note.count)page->block=page->note.blocks[0].id;
        if(page->block){note_block_t *b=note_find(&page->note,page->block);page->mode=b&&(b->kind==NOTE_AUDIO||b->kind==NOTE_IMAGE)?BLOCK_TITLE:EDIT;
            if(page->mode==EDIT&&page->caret.block!=page->block){page->caret=(notes_caret_t){.block=page->block,.scroll=-1};page->caret_override=true;}
            page->keyboard_wanted=true;}break;
    case A_FOCUS:page->block=value;page->caret.block=value;page->mode=EDIT;page->keyboard_wanted=true;break;
    case A_BLOCK:{if(value)page->block=value;note_block_t *b=note_find(&page->note,page->block);page->mode=b&&b->kind==NOTE_AUDIO?AUDIO_PLAYER:BLOCK_MENU;break;}
    case A_AUDIO_MENU:page->mode=AUDIO_MENU;break;
    case A_MENU:page->return_mode=value?(page->mode==EDIT?EDIT:READ):LIST;page->mode=MENU;break;
    case A_VIEW_MENU:page->mode=VIEW_MENU;break;
    case A_SORT_MENU:page->mode=SORT_MENU;break;
    case A_FILTER_MENU:page->mode=FILTER_MENU;break;
    case A_FORMAT:page->mode=FORMAT;break;
    case A_INSERT_MENU:page->mode=INSERT_MENU;break;
    case A_EXPORT_MENU:page->mode=EXPORT_MENU;break;
    case A_DATE_ORDER:page->sort=(page->sort&3)|(value?4:0);command(N_PREFERENCES,0,page->sort|(page->grid?8:0),NULL);page->offset=0;break;
    case A_AI_ORIGINAL:page->show_original=!page->show_original;break;
    case A_META:page->mode=META;break;
    case A_PICK_BOOK:keyboard_close();page->batch_move=false;page->offset=0;page->mode=BOOK_PICK;break;
    case A_CHOOSE_BOOK:{char name[1][96]={{0}};
        if(value && notes_books(name,value-1,1)<value)return;
        if(page->batch_move){strcpy(page->destination,name[0]);page->selecting=false;page->mode=LIST;}
        else{edit.action=A_META;edit.text=page->note.title;edit.book=name[0];if(!mutate(edit,false))return;page->mode=META;notes_save_now();}
        break;}
    case A_CLEAR_SEARCH:if(page->input){lv_textarea_set_text(page->input,"");page->search_due=lv_tick_get();}return;
    case A_INSERT:
        if(mutate(edit,false)){page->block=page->note.blocks[page->note.count-1].id;page->caret=(notes_caret_t){.block=page->block};page->caret_override=true;page->mode=value==NOTE_RULE?READ:EDIT;page->keyboard_wanted=value!=NOTE_RULE;}break;
    case A_INSERT_AT:
        if(mutate(edit,false)){page->block=page->note.blocks[value].id;page->caret=(notes_caret_t){.block=page->block,.scroll=-1};page->caret_override=true;page->mode=EDIT;page->keyboard_wanted=true;}break;
    case A_CHECK:edit.id=value;mutate(edit,false);break;
    case A_TYPE:if(mutate(edit,false)){page->mode=FORMAT;}break;
    case A_UP:case A_DOWN:case A_REMOVE:case A_ORIGINAL:
        if(mutate(edit,false)){page->mode=READ;page->caret=(notes_caret_t){0};page->caret_override=true;}break;
    case A_PIN:if(mutate(edit,false))page->mode=READ;break;
    case A_UNDO:case A_REDO:
        if(notes_undo(action==A_REDO))notes_document(&page->note);break;
    case A_BOLD:case A_ITALIC:{
        if(!notes_composer_selection(page->composer,&edit.id,&edit.start,&edit.end)){message(tr("长按并拖动选择文字","Hold and drag to select text"));return;}
        mutate(edit,false);break;}
    case A_HIDE:keyboard_close();design_keyboard_layout();return;
    case A_KEYBOARD:keyboard_open(value?page->meta_book:page->input);return;
    case A_SAVE:
        if(page->mode==BLOCK_TITLE){edit.action=A_EDIT;edit.text=lv_textarea_get_text(page->input);if(!mutate(edit,false))return;page->pending=false;note_block_t *b=note_find(&page->note,page->block);page->mode=b&&b->kind==NOTE_AUDIO?AUDIO_PLAYER:READ;}
        else if(page->mode==META){edit.action=A_META;edit.text=lv_textarea_get_text(page->input);edit.book=page->note.notebook;if(!mutate(edit,false))return;page->pending=false;page->mode=READ;}
        else if(page->mode==SEARCH){page->search_due=0;const char *text=lv_textarea_get_text(page->input);if(strlen(text)>=sizeof(page->search)){message(tr("搜索文字过长","Search text is too long"));return;}
            if(!command(N_QUERY,0,0,text))return;strcpy(page->search,text);page->pending=false;page->search_waiting=true;page->offset=0;}
        else if(page->mode==BOOK_EDIT){const char *text=lv_textarea_get_text(page->input);if(!*text||strlen(text)>=96){message(tr("请输入较短的笔记本名称","Enter a shorter notebook name"));return;}
            if(!command(page->book_edit?N_BOOK_RENAME:N_BOOK_NEW,page->book_edit?page->book_edit-1:0,0,text))return;
            if(page->book_edit){strcpy(page->pending_book,text);page->book_rename_pending=true;}page->pending=false;page->offset=0;page->mode=page->book_from_picker?BOOK_PICK:LIST;page->book_from_picker=false;}
        else page->mode=READ;notes_save_now();break;
    case A_DELETE:case A_PURGE:page->confirm_action=action;page->confirm_id=value;break;
    case A_CONFIRM:
        if(value){command(page->confirm_action==A_PURGE?N_PURGE:N_DELETE,page->confirm_id,0,NULL);page->mode=LIST;}
        page->confirm_action=0;break;
    case A_RESTORE:command(N_RESTORE,value,0,NULL);page->mode=LIST;break;
    case A_IMPORT:case A_ATTACH:
        page->attach_mode=action==A_ATTACH;
        if(!epd_file_picker_open(page->screen,page->attach_mode?".png;.jpg;.jpeg":".mp3;.wav;.md;.markdown;.txt",picked_file,page))message(tr("文件选择器暂时无法打开","Cannot open the file picker"));
        return;
    case A_EXPORT:command(N_EXPORT,0,value,NULL);break;
    case A_RESCAN:command(N_SCAN,0,0,NULL);page->mode=LIST;break;
    case A_REC:command(N_REC_START,0,0,NULL);if(page->mode!=EDIT)page->mode=READ;break;
    case A_PAUSE:command(N_REC_PAUSE,0,0,NULL);break;case A_RESUME:command(N_REC_RESUME,0,0,NULL);break;
    case A_STOP:command(N_REC_STOP,0,0,NULL);break;case A_MARK:command(N_REC_MARK,0,0,NULL);break;
    case A_PLAY:if(command(N_PLAY,page->block,value,NULL)&&page->mode==MARKS)page->mode=AUDIO_PLAYER;break;
    case A_SEEK:command(N_PLAY,page->block,value?value:1,NULL);break;
    case A_SKIP:{note_block_t *b=note_find(&page->note,page->block);if(!b)break;
        uint32_t ms=audio_position();ms=value?ms+10000:ms>10000?ms-10000:0;
        if(ms>=b->milliseconds)ms=b->milliseconds>1000?b->milliseconds-1000:0;command(N_PLAY,b->id,ms?ms:1,NULL);break;}
    case A_MARKS:page->offset=0;page->mode=MARKS;break;
    case A_MARK_EDIT:page->mark_index=value;page->mode=MARK_EDIT;break;
    case A_MARK_SAVE:edit.value=page->mark_index;edit.text=lv_textarea_get_text(page->input);if(mutate(edit,false)){page->pending=false;page->mode=MARKS;}break;
    case A_TRANSCRIPT_PLAY:{note_block_t *b=note_find(&page->note,page->block);if(!b)break;uint32_t start=0;
        for(size_t i=0;i<page->note.count;++i){note_block_t *p=&page->note.blocks[i];if(p->kind==NOTE_TRANSCRIPT&&p->source==b->source&&p->milliseconds<b->milliseconds&&p->milliseconds>start)start=p->milliseconds;}
        command(N_PLAY,b->source,start?start:1,NULL);break;}
    case A_PLAY_STOP:command(N_PLAY_STOP,0,0,NULL);break;
    case A_TRANSCRIBE:command(N_TRANSCRIBE,page->block,0,NULL);break;
    case A_AI_MENU:page->return_mode=page->mode==EDIT?EDIT:READ;page->ai_target=value;page->mode=AI_MENU;break;
    case A_AI:if(command(N_AI,0,value,NULL)){page->mode=READ;page->show_original=false;}break;
    case A_GRANT:if(!page->ai_authorized&&!page->grant_ticket){epd_ai_error_t e=epd_ai_authorize_service(notes_owner(),&page->grant_ticket);if(e)message(ai_error_text(e));}break;
    case A_CANCEL_TRANSCRIBE:if(page->status.transcribing)notes_cancel();break;
    case A_PREVIEW:page->offset=0;page->mode=PREVIEW;break;
    case A_TASK_SELECT:{note_block_t *b=note_find(&page->preview,value);if(b)notes_preview_select(value,!b->checked);break;}
    case A_ADOPT:if(command(N_ADOPT,0,0,NULL))page->adopt_pending=true;break;
    case A_REVERT:command(N_REVERT,page->note.id,0,NULL);page->mode=READ;break;
    case A_DISCARD_AI:if(command(N_DISCARD_AI,0,0,NULL))page->mode=READ;break;
    case A_IMAGE:note_free(page->pixels);page->pixels=NULL;command(N_IMAGE,page->block,0,NULL);page->mode=IMAGE;break;
    case A_SELECT:
        if(!value){page->selecting=!page->selecting;page->selected_count=0;page->mode=LIST;}
        else{size_t i;for(i=0;i<page->selected_count&&page->selected[i]!=value;++i){}if(i<page->selected_count)memmove(page->selected+i,page->selected+i+1,(--page->selected_count-i)*sizeof(uint32_t));else if(page->selected_count<64)page->selected[page->selected_count++]=value;}break;
    case A_BATCH_DELETE:page->batch_move=false;page->batch_export=false;page->selecting=false;page->mode=LIST;break;
    default:break;
    }
    page->needs_rebuild=true;
}
static const char *ai_error_text(epd_ai_error_t error)
{
    static const char *zh[]={"","笔记中没有可处理的文字","当前 AI 服务不支持文字处理","请先点击授权服务", "AI 服务正忙，请稍后再试","内存不足",
        "网络未连接","无法找到 AI 服务地址","无法连接 AI 服务","AI 服务安全连接失败","AI 服务账号验证失败，请检查平台配置",
        "AI 服务额度不足或请求受限，请稍后再试","AI 服务暂时不可用","AI 未返回有效的笔记内容，原文已保留","整篇笔记超出 AI 服务支持的容量",
        "AI 服务响应超时","","AI 回答未完成，原文已保留","AI 结果保存失败，请检查 TF 卡","请先在平台 AI 设置中配置文字服务"};
    static const char *en[]={"","No text to process","Text processing is unsupported","Authorize the service first","AI service is busy","Not enough memory",
        "No network connection","AI service address not found","Cannot connect to AI service","Secure connection failed","Check the AI account settings",
        "AI quota or rate limit reached","AI service unavailable","Invalid AI result; original kept","The complete note exceeds the AI service capacity",
        "AI request timed out","","Incomplete AI result; original kept","Cannot save AI result; check the TF card","Configure a text service in platform AI settings"};
    return tr((unsigned)error<sizeof(zh)/sizeof(*zh)?zh[error]:"AI 操作失败",(unsigned)error<sizeof(en)/sizeof(*en)?en[error]:"AI operation failed");
}
static void ai_authorization_refresh(void)
{
    if(page->grant_ticket){epd_ai_error_t e=epd_ai_authorization_poll(page->grant_ticket);
        if(e!=EPD_AI_BUSY){page->grant_ticket=0;page->ai_checked=0;page->needs_rebuild=true;if(e)message(e==EPD_AI_STORAGE?tr("服务授权未保存，请检查平台 AI 设置","Authorization was not saved; check platform AI settings"):ai_error_text(e));}}
    if(page->mode!=AI_MENU || (page->ai_checked && lv_tick_get()-page->ai_checked<1000))return;
    page->ai_checked=lv_tick_get();epd_ai_profile_t *profile=note_alloc(sizeof(*profile));size_t model=0;if(!profile)return;
    epd_ai_error_t e=notes_ai_profile(notes_owner(),profile,&model);note_free(profile);
    if(e==EPD_AI_BUSY)return;
    bool authorized=e==EPD_AI_OK;if(page->ai_authorized!=authorized){page->ai_authorized=authorized;page->needs_rebuild=true;}
}
static void update_notice(void)
{
    notes_status_t *s=&page->status;char text[256];text[0]=0;
    if(s->error){const char *errors[]={"","TF 卡不可用","文件读写失败","文件内容不受支持或已损坏","内存不足","内容超过支持的大小","操作已取消","正文已变化，未覆盖当前内容","正在处理其他操作"};
        snprintf(text,sizeof(text),"%s",s->error<=NOTES_BUSY?errors[s->error]:"操作失败");}
    else if(s->ai_error)snprintf(text,sizeof(text),"%s",ai_error_text(s->ai_error));
    else if(s->speech_error){
        static const char *errors[]={"","参数无效","音频正在被其他功能使用","内存不足","请先配置平台语音服务","网络未连接",
            "语音服务连接失败","语音服务安全连接失败","语音账号验证失败","语音服务额度不足或请求受限","语音服务返回异常",
            "语音服务响应超时","操作已取消","录音文件读写失败","麦克风不可用","录音数据未能及时写入","转写文字超过容量",
            "录音已达到时长限制","音频被其他功能中断","等待音频释放超时","录音不存在","录音格式不受支持"};
        snprintf(text,sizeof(text),"%s",tr((unsigned)s->speech_error<sizeof(errors)/sizeof(errors[0])?errors[s->speech_error]:"录音或转写失败",epd_speech_error_name(s->speech_error)));
    }
    else if(s->message[0])snprintf(text,sizeof(text),"%s",s->message);
    else if(s->export_path[0])snprintf(text,sizeof(text),tr("已导出到 Notes/Exports","Exported to Notes/Exports"));

    if(page->feedback[0]&&(int32_t)(page->feedback_until-lv_tick_get())>0)notice_text(page->feedback);else notice_text(text);
}
static void poll(lv_timer_t *timer)
{
    (void)timer;if(!page||page->retired)return;
    if(!notes_runtime_start())return;
    notes_composer_poll(page->composer);
    ai_authorization_refresh();
    if(page->pending&&(page->mode==EDIT||page->mode==META))commit_editor();
    if(page->mode==SEARCH&&page->pending&&page->search_due&&page->status.ready&&!page->status.busy&&!page->status.online&&(int32_t)(lv_tick_get()-page->search_due)>=0)do_action(A_SAVE,0);
    if(page->action){action_t action=page->action;uint32_t value=page->value;page->action=0;do_action(action,value);}
    bool old_recording=page->status.recording,old_paused=page->status.paused,old_online=page->status.online,old_playing=page->status.playing;
    notes_status_t status=page->status;if(notes_status(&status)){
        bool change=status.revision!=page->revision;
        bool structural=status.catalog_revision!=page->status.catalog_revision || status.document!=page->status.document || status.saved!=page->status.saved ||
            status.busy!=page->status.busy || status.ready!=page->status.ready || status.recording!=page->status.recording ||
            status.paused!=page->status.paused || status.online!=page->status.online || status.playing!=page->status.playing ||
            status.preview!=page->status.preview;
        page->status=status;page->revision=status.revision;
        if(page->book_rename_pending&&!status.busy){
            if(!status.error)strcpy(page->notebook,page->pending_book);
            page->book_rename_pending=false;page->needs_rebuild=true;
        }
        if(page->adopt_pending&&!status.busy){page->adopt_pending=false;if(!status.error)page->mode=READ;page->needs_rebuild=true;}
        if(page->search_waiting&&!status.busy){page->search_waiting=false;if(page->mode==SEARCH&&!page->pending)page->needs_rebuild=true;}
        if(status.ready&&!page->preferences_loaded){unsigned p=notes_preferences();page->sort=p&7u;page->grid=(p&8u)!=0;page->preferences_loaded=true;page->needs_rebuild=true;}
        if(page->pending_document&&!status.busy){
            if(!status.error&&status.document&&(page->pending_document==1||page->pending_document==status.document)){
                notes_document(&page->note);page->mode=page->edit_on_open?EDIT:READ;page->keyboard_wanted=page->edit_on_open;page->edit_on_open=false;page->block=page->note.count?page->note.blocks[page->new_task?page->note.count-1:0].id:0;page->caret=(notes_caret_t){.block=page->block,.scroll=page->new_task?-1:0};page->caret_override=true;page->new_task=false;
                if(page->task_target){for(size_t i=0;i<page->note.count;++i)if(page->note.blocks[i].id==page->task_target){page->caret.block=page->task_target;page->caret.scroll=-1;break;}page->task_target=0;}}
            page->pending_document=0;page->needs_rebuild=true;
            if(page->record_after_open){page->record_after_open=false;if(!status.error)command(N_REC_START,0,0,NULL);}
        }
        if(change && (structural || page->mode==PREVIEW) && page->mode!=EDIT&&page->mode!=META&&page->mode!=SEARCH&&page->mode!=BOOK_EDIT&&page->mode!=FORMAT){notes_document(&page->note);page->needs_rebuild=true;}
        if(page->progress) {
            char progress[96];
            uint32_t recording_ms=status.paused?status.recording_ms:status.recording_ms/5000*5000;
            uint32_t playback_ms=status.playback_ms/5000*5000;
            if(status.recording)snprintf(progress,sizeof(progress),status.paused?tr("已暂停  %02lu:%02lu","Paused  %02lu:%02lu"):tr("录音中  %02lu:%02lu","Recording  %02lu:%02lu"),(unsigned long)(recording_ms/60000),(unsigned long)(recording_ms/1000%60));
            else if(status.playing)snprintf(progress,sizeof(progress),"%02lu:%02lu",(unsigned long)(playback_ms/60000),(unsigned long)(playback_ms/1000%60));
            else progress[0]=0;
            epd_app_text(page->progress,progress);
        }
        if(page->audio_time)design_audio_update();
        if(!page->selecting&&page->selected_count&&!status.busy){
            if(status.error){page->selecting=true;page->batch_pending=0;page->needs_rebuild=true;}
            else{
                if(page->batch_pending){--page->selected_count;page->batch_pending=0;}
                if(page->selected_count){uint32_t id=page->selected[page->selected_count-1];
                    if(notes_command(page->batch_export?N_EXPORT:page->batch_move?N_MOVE_BOOK:N_DELETE,id,page->batch_export?page->batch_format:0,page->batch_move?page->destination:NULL))page->batch_pending=id;}
            }
        }
    }
    if(page->composer&&page->mode==EDIT&&(status.recording!=old_recording||status.paused!=old_paused||status.online!=old_online||status.playing!=old_playing)){notes_document(&page->note);page->needs_rebuild=true;}
    if(page->scale!=epd_ui_scale_revision())page->needs_rebuild=true;
    if(page->needs_rebuild)rebuild();update_notice();
}
static void gesture(lv_event_t *event)
{
    (void)event;if(!page||page->mode!=READ)return;lv_dir_t dir=lv_indev_get_gesture_dir(lv_indev_active());
    if(dir==LV_DIR_LEFT)page->action=A_NEXT;else if(dir==LV_DIR_RIGHT)page->action=A_PREV;
}
static void keys(epd_app_key_t key,epd_app_key_event_t event,void *user)
{(void)user;if(page&&event==EPD_APP_KEY_CLICKED)page->action=key==EPD_APP_KEY2?A_PREV:A_NEXT;}
static bool navigation_key(uint32_t key)
{
    if(key!=LV_KEY_ESC||!page)return false;
    if(page->retired)gui_app_goback();
    else{page->action=A_BACK;page->value=0;}
    return true;
}
static void navigation_init(void)
{
    static const epd_input_ops_t ops={.key=navigation_key};
    epd_input_set_ops(epd_app_input_group(page->screen),&ops);
}
bool notes_ui_present(void) { return page != NULL; }

void notes_ui_retire(void)
{
    if(!page)return;epd_file_picker_cancel(page->screen);commit_editor();keyboard_close();if(page->timer)lv_timer_delete(page->timer);page->timer=NULL;page->retired=true;
    lv_obj_clean(page->body);page->composer=NULL;page->input=page->meta_book=page->progress=NULL;page->pending=false;
    message(tr("笔记服务已停止，请返回后重新打开。","Notes stopped. Go back and reopen."));
}
static void page_message(gui_app_msg_type_t message_type,void *parameter)
{
    (void)parameter;notes_page_t **slot=gui_app_this_page_memory();
    if(message_type==GUI_APP_MSG_ONSTART){
        notes_page_t *next=note_alloc(sizeof(*next));if(!next)return;*slot=page=next;page->mode=LIST;page->needs_rebuild=true;
        page->screen=lv_screen_active();gui_app_close_anim();epd_app_object_init(page->screen);
        lv_obj_set_style_bg_color(page->screen,lv_color_white(),0);lv_obj_set_style_bg_opa(page->screen,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(page->screen,lv_color_black(),0);navigation_init();epd_app_header(page->screen);
        page->title=label(page->screen,tr("笔记","Notes"),epd_ui_px(104),epd_ui_px(81),epd_ui_px(520),false);
        page->body=lv_obj_create(page->screen);epd_app_object_init(page->body);
        page->notice=label(page->screen,"",0,0,100,true);
        lv_obj_add_event_cb(page->body,gesture,LV_EVENT_GESTURE,NULL);epd_app_input_listen(page->screen,keys,NULL);
        bool started=notes_runtime_start();page->timer=lv_timer_create(poll,100,NULL);if(page->timer)lv_timer_pause(page->timer);if(started)rebuild();
    }else if(!*slot)return;
    else if(message_type==GUI_APP_MSG_ONRESUME){page=*slot;epd_app_header_refresh(page->screen);if(page->timer)lv_timer_resume(page->timer);}
    else if(message_type==GUI_APP_MSG_ONPAUSE){page=*slot;commit_editor();notes_save_now();keyboard_close();if(page->timer)lv_timer_pause(page->timer);}
    else if(message_type==GUI_APP_MSG_ONSTOP){
        page=*slot;epd_file_picker_cancel(page->screen);commit_editor();notes_save_now();keyboard_close();if(page->timer)lv_timer_delete(page->timer);
        if(page->clock_font)epd_app_font_destroy(page->clock_font);if(page->clock_fallback)epd_app_font_destroy(page->clock_fallback);
        lv_obj_clean(page->body);page->composer=NULL;if(page->heading)epd_app_font_destroy(page->heading);if(page->heading_fallback)epd_app_font_destroy(page->heading_fallback);note_clear(&page->note);note_clear(&page->preview);note_clear(&page->original);note_free(page->pixels);note_free(page);page=NULL;*slot=NULL;
        notes_runtime_ui_closed();
    }
}
int app_main(intent_t intent)
{
    (void)intent;
    if(!notes_runtime_start()&&!app_service_running("notes"))return -RT_ENOMEM;
    int result=gui_app_regist_msg_handler_ext("notes",page_message,NULL,sizeof(notes_page_t*));
    if(result!=RT_EOK)notes_runtime_ui_closed();
    return result;
}
