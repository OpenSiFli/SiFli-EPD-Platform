#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RT_EOK 0
#define RT_ENOMEM 12
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef void *intent_t;
typedef void demo_page_t;
typedef void notes_page_t;
static unsigned checks, starts, registered, closes;
static bool start_ok=true, running;
static int register_result;
static bool demo_start(void) { ++starts; return start_ok; }
static bool notes_runtime_start(void) { ++starts; return start_ok; }
static void demo_ui_closed(void) { ++closes; }
static void notes_runtime_ui_closed(void) { ++closes; }
static bool app_service_running(const char *id) { (void)id; return running; }
static void page_message(void) {}
static int gui_app_regist_msg_handler_ext(const char *id,void (*callback)(void),void *data,size_t size)
{ CHECK(callback==page_message && !data && size==sizeof(void*) && id[0]); ++registered; return register_result; }
#include "actual.inc"
int main(void)
{
    unsigned routes[3]={0,1,2};
    for(unsigned i=0;i<3;++i) { CHECK(!app_main(&routes[i])); CHECK(starts==i+1 && registered==i+1); }
    start_ok=false; CHECK(app_main(NULL)==-RT_ENOMEM && registered==3);
    running=true; CHECK(!app_main(&routes[2]) && registered==4); /* New UI waits for an old service's retirement. */
    register_result=-7; CHECK(app_main(&routes[1])==-7 && closes==1);
    printf("PASS application entry routes: %u checks\n",checks); return 0;
}
