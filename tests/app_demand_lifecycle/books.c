#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RT_EOK 0
#define RT_ENOMEM 12
#define ENOENT 2
#define UI_PAGE_BOOKSHELF 1
#define UI_PAGE_READER 2
#define NAV_IDLE 0
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef int rt_err_t;
typedef struct { bool cancelled; } epd_service_t;
typedef struct { const char *path; } *intent_t;
typedef struct timer { void (*callback)(struct timer *); bool live; } lv_timer_t;
typedef struct { bool foreground; unsigned id,content_generation,locale_revision; void *screen; } books_page_t;
typedef enum { GUI_APP_MSG_ONSTART,GUI_APP_MSG_ONRESUME,GUI_APP_MSG_ONPAUSE,GUI_APP_MSG_ONSTOP } gui_app_msg_type_t;
static unsigned checks,starts,stops,builds,requests,saves,closed,content_calls,resumes,headers;
static bool running,save_ok=true,flush_ok=true,ai_idle=true,display_idle=true,storage_busy;
static bool initialized,initialization_attempted,idle_stopping,reader_session;
static unsigned live_pages,saved_focus[3],storage_generation,content_session,content_generation;
static int pending_page,content_error,register_result;
static char content_path[512],dispatched_path[512];
static epd_service_t owner,*books_service;
static books_page_t page,*current,*waiting_page;
static lv_timer_t timer_storage,*restart_timer;
static const int epd_app_background;
static struct { bool ready; } reading;
static bool books_service_start(void);
static void books_page_resume(books_page_t *p);
static void page_message(gui_app_msg_type_t message,void *parameter);
static void epd_reader_collect(void) {}
static void books_command_ui_process(bool stopping) { (void)stopping; }
static bool lv_refreshing_done(void) { return display_idle; }
static bool books_ai_shutdown(void) { CHECK(ai_idle); return true; }
static void ui_reader_close(void) { ++closed; }
static void document_service_shutdown(void) {}
static void ui_bookshelf_close(void) {}
static void bookshelf_service_deinit(void) {}
static bool storage_changing(void) { return storage_busy; }
static bool books_initialize(void) { CHECK(!running); initialized=initialization_attempted=true; storage_generation=1; return true; }
static bool ui_reader_save(void) { ++saves; return save_ok; }
#define ui_reader_view() (&reading)
static bool ui_bookshelf_flush(void) { ++saves; return flush_ok; }
static bool books_ai_idle(void) { return ai_idle; }
static void app_service_stop(const char *id) { CHECK(!strcmp(id,"books")); ++stops; owner.cancelled=true; }
static void epd_service_ui_request(epd_service_t *s,unsigned delay) { CHECK(s==&owner && delay<=100); ++requests; }
static unsigned storage_revision(void) { return 1; }
static unsigned storage_card_session(void) { return 3; }
static unsigned launcher_current_page(void) { return UI_PAGE_BOOKSHELF; }
static void bookshelf_resume(void) { CHECK(initialized && !owner.cancelled && current==&page && page.foreground); ++resumes; }
static void content_process(void) { ++content_calls; strcpy(dispatched_path,content_path); }
static void process_navigation(void) {}
static void books_menu_process(void) {}
static bool books_process(void) { return false; }
static bool app_service_running(const char *id) { CHECK(!strcmp(id,"books")); return running; }
static bool epd_service_cancelled(epd_service_t *s) { return s->cancelled; }
static void lv_timer_delete(lv_timer_t *t) { CHECK(t->live); t->live=false; }
static lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *),unsigned ms,void *data)
{ CHECK(ms==20 && !data && !timer_storage.live); timer_storage.callback=callback; timer_storage.live=true; return &timer_storage; }
static void *dlmodule_find(const char *id) { CHECK(!strcmp(id,"books")); return &owner; }
static epd_service_t *app_service_start(const char *id,const int *definition,void *module)
{ CHECK(!running && !strcmp(id,"books") && definition==&epd_app_background && module); running=true; owner.cancelled=false; ++starts; return &owner; }
static void books_page_build(books_page_t *p) { CHECK(p==&page && initialized && !owner.cancelled); ++builds; }
static books_page_t *gui_app_this_page_memory(void) { return &page; }
static uintptr_t gui_app_this_page_userdata(void) { return UI_PAGE_BOOKSHELF; }
static unsigned epd_locale_revision(void) { return 1; }
static void gui_app_close_anim(void) {}
static void *lv_screen_active(void) { return &page; }
static void books_scale_refresh(void) { CHECK(!owner.cancelled && builds); }
static void reader_foreground(void) { CHECK(!owner.cancelled); }
static void epd_app_header_refresh(void *s) { CHECK(s==&page && !owner.cancelled); ++headers; }
static unsigned focus_index(void) { CHECK(!owner.cancelled && builds); return 0; }
static void reader_pause(void) {}
static void bookshelf_pause(void) { CHECK(!owner.cancelled && builds); }
static void reader_stop(void) {}
static int app_content_receive(intent_t intent,char *out,size_t capacity)
{ if(!intent || !intent->path) { out[0]=0; return -ENOENT; } snprintf(out,capacity,"%s",intent->path); return 0; }
static int gui_app_regist_msg_handler_ext(const char *id,void (*callback)(gui_app_msg_type_t,void *),void *data,size_t size)
{
    CHECK(!strcmp(id,"books") && callback==page_message && (uintptr_t)data==UI_PAGE_BOOKSHELF && size==sizeof(page));
    if(register_result)return register_result;
    memset(&page,0,sizeof(page)); callback(GUI_APP_MSG_ONSTART,NULL); callback(GUI_APP_MSG_ONRESUME,NULL); return 0;
}
enum { OPEN_UI_DONE, OPEN_RESUMED };
static struct { void *screen; } open_trace;
static void open_trace_mark(unsigned stamp) { CHECK(stamp <= OPEN_RESUMED); }
static void open_trace_poll(void) {}
static void open_trace_stop(void) { open_trace.screen = NULL; }
static void history_notice(void) {}
static void ui_reader_cache_quiesce(void) {}
static void books_cover_cache_cancel(void) {}
static bool ui_reader_cache_busy(void) { return false; }
static bool books_cover_cache_busy(void) { return false; }
static void ui_reader_shutdown(void) { ui_reader_close(); }
static bool ui_bookshelf_history_retry(void) { return flush_ok; }
static bool ui_bookshelf_history_failed(void) { return initialized && !flush_ok; }
#include "actual.inc"
static void leave(void) { page_message(GUI_APP_MSG_ONPAUSE,NULL); page_message(GUI_APP_MSG_ONSTOP,NULL); CHECK(!live_pages); }
int main(void)
{
    struct { const char *path; } file={"/sdcard/first.epub"},notification={NULL};
    CHECK(!app_main(NULL)); CHECK(starts==1 && builds==1 && live_pages==1);
    CHECK(resumes==1 && headers==1 && current==&page && page.foreground);
    CHECK(!service_process(&owner,false) && !stops);
    leave();
    reader_session=reading.ready=true; save_ok=false;
    CHECK(!service_process(&owner,false) && !stops && requests);
    save_ok=true; ai_idle=false; CHECK(!service_process(&owner,false) && !stops);
    ai_idle=true; CHECK(!service_process(&owner,false) && stops==1 && idle_stopping);
    CHECK(!app_main((intent_t)&file)); CHECK(starts==1 && builds==1 && waiting_page==&page);
    CHECK(resumes==1 && headers==1 && current==&page && page.foreground);
    CHECK(!strcmp(content_path,file.path));
    timer_storage.callback(&timer_storage); CHECK(starts==1 && builds==1 && timer_storage.live);
    display_idle=false; CHECK(!service_process(&owner,true) && !closed);
    display_idle=true; CHECK(service_process(&owner,true)); CHECK(closed==1 && !books_service && !initialized);
    CHECK(!strcmp(content_path,file.path));
    timer_storage.callback(&timer_storage); CHECK(starts==1 && builds==1);
    running=false; timer_storage.callback(&timer_storage);
    CHECK(starts==2 && builds==2 && !waiting_page && !restart_timer && !timer_storage.live);
    CHECK(resumes==2 && headers==2 && current==&page && page.foreground);
    CHECK(!service_process(&owner,false)); CHECK(!strcmp(dispatched_path,file.path));
    leave(); reader_session=false; reading.ready=false;
    CHECK(!service_process(&owner,false) && stops==2); CHECK(service_process(&owner,true)); running=false;
    CHECK(!app_main((intent_t)&notification) && starts==3 && builds==3);
    leave(); CHECK(!service_process(&owner,false));
    CHECK(!app_main((intent_t)&file)); CHECK(timer_storage.live && waiting_page);
    leave(); CHECK(!timer_storage.live && !restart_timer && !waiting_page);
    CHECK(service_process(&owner,true)); running=false;
    register_result=-1; CHECK(app_main((intent_t)&file)==-1 && !live_pages);
    CHECK(!service_process(&owner,false)); CHECK(service_process(&owner,true));
    running=false; register_result=0;
    CHECK(!app_main(NULL)); leave(); CHECK(!service_process(&owner,false));
    CHECK(!app_main(NULL) && waiting_page==&page);
    unsigned resumed=resumes,header_count=headers;
    page_message(GUI_APP_MSG_ONPAUSE,NULL); CHECK(!current && !page.foreground);
    CHECK(service_process(&owner,true)); running=false; timer_storage.callback(&timer_storage);
    CHECK(!current && !page.foreground && !waiting_page && !restart_timer);
    CHECK(resumes==resumed && headers==header_count);
    page_message(GUI_APP_MSG_ONRESUME,NULL);
    CHECK(current==&page && page.foreground && resumes==resumed+1 && headers==header_count+1);
    initialized=false; initialization_attempted=true; save_ok=flush_ok=false;
    unsigned active_stops=stops;
    CHECK(!service_process(&owner,false) && stops==active_stops);
    initialized=true; leave(); initialized=false;
    unsigned save_count=saves,stop_count=stops;
    CHECK(!service_process(&owner,false));
    CHECK(idle_stopping && stops==stop_count+1 && saves==save_count);
    CHECK(service_process(&owner,true) && !books_service && !initialization_attempted);
    printf("PASS books entry, file intent, immediate reopen: %u checks\n",checks); return 0;
}
