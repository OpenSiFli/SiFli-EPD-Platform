#include "calendar_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define RT_EOK 0
#define RT_ENOMEM 12
typedef const char *intent_t;
typedef enum {GUI_APP_MSG_ONSTART,GUI_APP_MSG_ONRESUME,GUI_APP_MSG_ONPAUSE,GUI_APP_MSG_ONSTOP} gui_app_msg_type_t;
enum {HOME,EDIT};
enum {CAL_DRAFT,NOTES_INPUT_CHARS=2000,NOTES_INPUT_BYTES=NOTES_INPUT_CHARS*4};
typedef struct {unsigned completed;char error[160];bool ready;cal_book_t preferences;} cal_status_t;
typedef struct {
    cal_status_t status;int mode,source;void *screen,*timer,*input;
    epd_date_t selected,calc_date;
    int calc_days,view,filter,pending_id,minute,occurrence,field,offset;
    bool retired,dirty,instance,waiting,draft_waiting,draft_failed,draft_saved;
    unsigned draft_completed,draft_revision,edit_revision;
    char query[241],*notes_input;
    cal_event_t draft,original;
} page_t;
static page_t *page,*slot;
static unsigned checks,starts,status_reads,queries,closed,registrations,field_reads;
static uint32_t intent_id;static int32_t intent_day;
static bool running,ready,fail_alloc;
static unsigned allocation_failures,submits;
static int registration_result;
static char captured[241];
static char *input_text;
static cal_event_t owned;
static cal_status_t backend;
static void message(gui_app_msg_type_t msg,void *parameter);
static bool cal_runtime_start(void){++starts;return ready;}
static bool app_service_running(const char *id){CHECK(!strcmp(id,"calendr"));return running;}
static void cal_runtime_status(cal_status_t *out){++status_reads;*out=backend;out->ready=ready;}
static void cal_runtime_ui_closed(cal_event_t *draft,int32_t day,bool instance)
{++closed;if(draft){CHECK(day==12&&instance);strcpy(captured,draft->title);cal_event_clear(&owned);owned=*draft;memset(draft,0,sizeof(*draft));}}
static bool cal_runtime_submit(int command,const cal_event_t *e,uint32_t id,int32_t day,bool instance,const char *path,const cal_book_t *prefs)
{CHECK(command==CAL_DRAFT&&e==&page->draft);++submits;return false;}
static const char *intent_get_string(intent_t i,const char *key){CHECK(!strcmp(key,"page"));return i;}
static void *gui_app_this_page_memory(void){return &slot;}
static void *lv_screen_active(void){return (void*)1;}
static void gui_app_close_anim(void){}
static void screen_init(void *s){CHECK(s==(void*)1);}
static void epd_app_header(void *s){CHECK(s==(void*)1);}
static void epd_app_header_refresh(void *s){CHECK(s==(void*)1);}
static epd_date_t today(void){return (epd_date_t){2026,10,31};}
static void request_rows(void){++queries;}
static void rebuild(void){CHECK(page);}
static void poll(void *timer){(void)timer;}
static void *lv_timer_create(void (*fn)(void*),unsigned ms,void *data){CHECK(fn==poll&&ms==200&&!data);return (void*)2;}
static void lv_timer_pause(void *t){CHECK(t==(void*)2);}
static void lv_timer_resume(void *t){CHECK(t==(void*)2);}
static void lv_timer_delete(void *t){CHECK(t==(void*)2);}
static const char *lv_textarea_get_text(void *input){CHECK(input==(void*)3&&input_text);++field_reads;return input_text;}
static const char *tr(const char *zh,const char *en){return zh;}
static void notify(const char *text){CHECK(text&&*text);}
static void *ui_alloc(size_t size){if(fail_alloc){++allocation_failures;return NULL;}return cal_alloc(size);}
static void epd_file_picker_cancel(void *s){CHECK(s==(void*)1);}
static void modal_close(void){free(input_text);input_text=NULL;cal_free(page->notes_input);page->notes_input=NULL;page->input=NULL;}
static int gui_app_regist_msg_handler_ext(const char *id,void (*fn)(gui_app_msg_type_t,void*),void *data,size_t size)
{CHECK(!strcmp(id,"calendr")&&fn==message&&!data&&size==sizeof(page_t*));++registrations;return registration_result;}
#define cal_alloc ui_alloc
#include "entry.inc"
#undef cal_alloc
static void notes_exit(bool failed_write,bool pending_draft,bool maximum)
{
    CHECK(!app_main(NULL));message(GUI_APP_MSG_ONSTART,NULL);
    page->mode=EDIT;page->dirty=page->instance=true;page->occurrence=12;page->field=2;
    strcpy(page->draft.title,"notes draft");page->draft.notes=cal_alloc(4);strcpy(page->draft.notes,"old");
    page->notes_input=cal_alloc(NOTES_INPUT_BYTES+1);char *reserve=page->notes_input;CHECK(reserve);
    input_text=malloc(NOTES_INPUT_BYTES+1);CHECK(input_text);
    if(maximum){for(unsigned i=0;i<NOTES_INPUT_CHARS;++i)memcpy(input_text+i*4,"\xf0\x9f\x99\x82",4);input_text[NOTES_INPUT_BYTES]=0;}
    else strcpy(input_text,"最新备注，退出时必须保留");
    char expected[NOTES_INPUT_BYTES+1];strcpy(expected,input_text);page->input=(void*)3;
    page->draft_failed=failed_write;page->draft_waiting=pending_draft;
    unsigned before=submits,failed=allocation_failures;fail_alloc=true;
    message(GUI_APP_MSG_ONPAUSE,NULL);message(GUI_APP_MSG_ONSTOP,NULL);
    CHECK(!page&&!slot&&!input_text&&owned.notes==reserve&&!strcmp(owned.notes,expected));
    CHECK(submits==before);
    if(!failed_write&&!pending_draft&&!maximum)CHECK(allocation_failures==failed+2);
    fail_alloc=false;
    cal_event_t reopened={0};CHECK(cal_event_copy(&reopened,&owned)&&reopened.notes!=owned.notes);
    CHECK(!strcmp(reopened.notes,expected));cal_event_clear(&reopened);cal_event_clear(&owned);
}
int main(void)
{
    CHECK(app_main(NULL)==-RT_ENOMEM&&!registrations);
    running=true;CHECK(!app_main("event:42:99")&&intent_id==42&&intent_day==99);
    message(GUI_APP_MSG_ONSTART,NULL);message(GUI_APP_MSG_ONRESUME,NULL);
    CHECK(page&&page->pending_id==42&&status_reads==1&&queries==1&&page->view==1);
    ready=true;message(GUI_APP_MSG_ONRESUME,NULL);CHECK(status_reads==1&&queries==1);
    page->mode=EDIT;page->dirty=page->instance=true;page->occurrence=12;page->input=(void*)3;
    input_text=malloc(16);strcpy(input_text,"last edit");unsigned reads=status_reads;
    message(GUI_APP_MSG_ONPAUSE,NULL);message(GUI_APP_MSG_ONSTOP,NULL);
    CHECK(!page&&!slot&&closed==1&&field_reads==3&&!strcmp(captured,"last edit"));
    CHECK(status_reads==reads+2);
    backend.preferences.default_view=2;
    CHECK(!app_main(NULL)&&!intent_id&&!intent_day);message(GUI_APP_MSG_ONSTART,NULL);message(GUI_APP_MSG_ONRESUME,NULL);
    CHECK(status_reads==reads+3&&queries==2&&page->view==2);page->mode=EDIT;page->dirty=page->waiting=true;captured[0]=0;
    message(GUI_APP_MSG_ONSTOP,NULL);CHECK(closed==2&&!slot&&!captured[0]);
    registration_result=-1;CHECK(app_main(NULL)==-1&&closed==3);
    registration_result=0;cal_event_clear(&owned);
    notes_exit(false,false,false);notes_exit(true,false,false);notes_exit(false,true,false);notes_exit(false,false,true);
    printf("PASS calendar entry and deferred UI: %u checks\n",checks);return 0;
}
