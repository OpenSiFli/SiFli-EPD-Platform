#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
typedef int lv_obj_t;
typedef bool (*epd_file_selected_t)(const char *,void *);
typedef struct {char pick_extensions[96];bool pick_json;lv_obj_t *title;} files_view_t;
static files_view_t view,*active_picker;
static struct {unsigned text_binding_count;struct {lv_obj_t *label;int id;} text_bindings[1];} screen;
#define current (&screen)
#define LV_EXT_STR_ID(x) 19
#define epd_get_str(x) "Choose a file"
static unsigned opens;static bool fail_open,is_interrupt;
static int rt_interrupt_get_nest(void){return is_interrupt;}
static bool picker_open_for(lv_obj_t *owner,epd_file_selected_t selected,void *context){(void)context;opens++;if(!owner||!selected||fail_open)return false;memset(&view,0,sizeof(view));active_picker=&view;return true;}
static void text_update(lv_obj_t *object,const char *text){(void)object;(void)text;}
static void files_render(void){}
#include "picker_actual.inc"
static bool selected(const char *path,void *context){(void)path;(void)context;return true;}
int main(void){
    lv_obj_t owner=0;assert(epd_file_picker_open(&owner,".mp3;.wav;.md;.markdown;.txt",selected,NULL));assert(opens==1);
    const char *accepted[]={"a.mp3","录音.WAV","笔记.mD","notes.Markdown","a.TXT"};for(unsigned i=0;i<5;i++)assert(picker_accepts(&view,accepted[i]));
    const char *rejected[]={"a.jpg","a.png","a.zip","a.exe","notes.txt.exe","audio.mp3.tmp","no-extension","a.m4a"};for(unsigned i=0;i<8;i++)assert(!picker_accepts(&view,rejected[i]));
    const char *bad[]={"","mp3",".mp3;",".;.txt",".*",".txt;;.wav",".txt/.wav"};unsigned before=opens;
    for(unsigned i=0;i<7;i++)assert(!epd_file_picker_open(&owner,bad[i],selected,NULL));assert(opens==before);
    is_interrupt=true;assert(!epd_file_picker_open(&owner,".txt",selected,NULL)&&opens==before);is_interrupt=false;
    fail_open=true;assert(!epd_file_picker_open(&owner,".txt",selected,NULL));fail_open=false;
    assert(epd_file_picker_open(&owner,".png;.jpg;.jpeg",selected,NULL)&&picker_accepts(&view,"image.JPG")&&!picker_accepts(&view,"audio.wav"));
    memset(&view,0,sizeof(view));assert(picker_accepts(&view,"image.PNG")&&!picker_accepts(&view,"config.json"));view.pick_json=true;assert(picker_accepts(&view,"config.JSON")&&!picker_accepts(&view,"image.png"));
    puts("file picker: audio/text filters, invalid extensions, open failure, existing image/JSON modes passed");return 0;
}
