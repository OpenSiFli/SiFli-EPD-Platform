#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "reader_page_cache.h"
typedef uint32_t rt_tick_t;
typedef int32_t rt_int32_t;
typedef epd_reader_cursor_t document_cursor_t;
typedef struct { unsigned offset, skip_utf8, unit; } document_anchor_t;
typedef struct { EPD_READER_PAGE_FIELDS(document_cursor_t); unsigned generation; } view_page_t;
typedef struct view_state view_state_t;
typedef struct view_frame {
    struct view_frame *next; view_state_t *owner;
    unsigned number; uint32_t layout, generation;
    bool complete, cancelled;
    epd_reader_page_t page;
    struct { void *data; } pixels;
    struct { void *first; size_t raw; } cold;
    struct { void *data; size_t data_size; } image;
} view_frame_t;
struct view_state {
    view_state_t *previous, *following;
    void *document;
    unsigned wanted, preview_page, layout;
    bool preview_cache_active;
    struct { bool ready; } status;
    struct { bool complete; int result; unsigned end,source_begin,chapters,chapter; } progress;
    unsigned display_layout;
    view_page_t *display;
    reader_page_cache_t cache;
    view_frame_t *frames, *rendering;
    uint32_t session, intent, published_intent, published_generation;
    bool frame_published, initialized, target_pending, input_enabled, failure;
};
static view_state_t *primary_state, *state;
static struct { view_state_t *session; uint32_t id; } ui;
static struct { uint32_t session, intent; bool stop,replace,paused,suspended; int wake; } mailbox;
static SRWLOCK lock = SRWLOCK_INIT;
static _Thread_local unsigned lock_depth;
static volatile LONG tick, printed, sleeps;
static rt_int32_t last_timeout;
static void rt_enter_critical(void) {assert(!lock_depth);AcquireSRWLockExclusive(&lock);++lock_depth;}
static void rt_exit_critical(void) {assert(lock_depth==1);--lock_depth;ReleaseSRWLockExclusive(&lock);}
static rt_tick_t rt_tick_get(void) {return (rt_tick_t)InterlockedIncrement(&tick);}
static unsigned elapsed_ms(rt_tick_t at) {return rt_tick_get()-at;}
static void rt_sem_take(int sem, rt_int32_t timeout) {(void)sem;assert(!lock_depth);InterlockedIncrement(&sleeps);last_timeout=timeout;}
static void rt_kprintf(const char *format, ...) {assert(!lock_depth);InterlockedIncrement(&printed);va_list a;va_start(a,format);vprintf(format,a);va_end(a);}
static bool cursor_equal(document_cursor_t a,document_cursor_t b) {return a.record==b.record&&a.byte==b.byte;}
static bool document_source_position(void *doc,unsigned record,unsigned byte,document_anchor_t *out)
{(void)doc;assert(!lock_depth);*out=(document_anchor_t){record,byte,2};return true;}
static int document_target_cursor(void *doc,unsigned *record,unsigned *byte)
{(void)doc;(void)record;(void)byte;assert(!lock_depth);return 0;}
/* TYPES */
/* The diagnostics sink is checked at the boundary; this suite exercises
 * window ownership and publication, not document worker restoration. */
enum { DOC_RESTORE_ROUTE };
static void document_trace_restore(void *doc, unsigned event, const void *context,
    unsigned before, unsigned after, int result)
{
    const view_state_t *owner = context;
    assert(!lock_depth && owner && doc == owner->document && event == DOC_RESTORE_ROUTE);
    assert(before <= 31 && !after && result >= 1 && result <= 3);
}
/* ROUTES */
/* FUNCTIONS */

static void page(view_page_t *p,unsigned record,bool eof)
{memset(p,0,sizeof(*p));p->start.record=record;p->cursor.record=record+1;p->eof=eof;p->generation=1;p->done=p->collect=1;}
static void owner(view_state_t *s,view_page_t *p,unsigned number,unsigned record,bool eof)
{
    memset(s,0,sizeof(*s));s->status.ready=true;s->layout=s->display_layout=1;
    s->wanted=number;s->display=p;s->session=1;s->intent=1;
    s->progress.chapters=10;s->progress.chapter=number;
    s->progress.complete=true;s->progress.end=1000;
    page(p,record,eof);
    s->cache.budget=65536;s->cache.allocate=malloc;s->cache.release=free;
    assert(reader_page_cache_put(&s->cache,number,1,(epd_reader_page_t*)p));
}
static view_frame_t *add_frame(view_state_t *s,unsigned number,unsigned record,bool eof)
{
    view_page_t p;page(&p,record,eof);
    assert(reader_page_cache_put(&s->cache,number,1,(epd_reader_page_t*)&p));
    view_frame_t *f=calloc(1,sizeof(*f));assert(f);f->owner=s;f->number=number;f->layout=1;f->generation=1;
    f->complete=true;f->page=*(epd_reader_page_t*)&p;f->pixels.data=f->image.data=f;f->image.data_size=128;
    f->next=s->frames;s->frames=f;return f;
}
static view_state_t *open_session(void)
{
    view_state_t *s=calloc(1,sizeof(*s)+sizeof(view_window_snapshot_t));assert(s);
    s->session=1;ui.session=s;ui.id=mailbox.session=mailbox.intent=1;return s;
}
static void close_session(void)
{
    rt_enter_critical();view_state_t *old=ui.session;ui.session=NULL;++ui.id;mailbox.session=ui.id;++mailbox.intent;
    rt_exit_critical();free(old);
}
static void clear_owner(view_state_t *s)
{
    reader_page_cache_clear(&s->cache);
    while(s->frames){view_frame_t *f=s->frames;s->frames=f->next;free(f);}
}
static view_window_snapshot_t capture(view_state_t *s)
{
    view_window_snapshot_t copy={0};window_snapshot_publish(s);assert(window_snapshot_copy(&copy));return copy;
}
static void local_window(view_state_t *s,view_page_t *p)
{
    owner(s,p,10,10,false);state=primary_state=s;
    for(unsigned i=7;i<=13;++i)add_frame(s,i,i,false);
}
static void valid_states(void)
{
    view_state_t s;view_page_t p;local_window(&s,&p);open_session();
    s.frame_published=true;s.published_intent=1;s.published_generation=1;
    view_window_snapshot_t v=capture(&s);assert(v.hot==7&&v.published&&v.published_generation==1);
    for(unsigned i=0;i<7;++i)assert(v.pages[i].hot&&v.pages[i].number==7+i&&v.pages[i].cursor.record==7+i);
    epd_reader_window_diagnostic();
    view_frame_t *f=s.frames;
    f->cancelled=true;v=capture(&s);assert(v.hot==6&&v.pages[6].cancelled&&!v.pages[6].pixelsready);
    f->cancelled=false;f->cold.first=f;f->cold.raw=128;v=capture(&s);assert(v.hot==6&&v.pages[6].cold&&!v.pages[6].pixelsready);
    f->cold.raw=64;v=capture(&s);assert(v.hot==6&&v.pages[6].pixelsready&&v.pages[6].cold);
    f->cold.first=NULL;f->complete=false;v=capture(&s);assert(v.hot==6&&v.pages[6].present&&!v.pages[6].complete);
    s.frames=f->next;s.rendering=f;v=capture(&s);assert(v.pages[6].present&&!v.pages[6].complete);
    f->complete=true;v=capture(&s);assert(v.hot==7);s.rendering=NULL;s.frames=f;
    f->image.data=NULL;v=capture(&s);assert(v.hot==6&&!v.pages[6].pixelsready);f->image.data=f;
    f->layout=2;v=capture(&s);assert(v.hot==6&&!v.pages[6].present);f->layout=1;
    f->page.cursor.byte=1;v=capture(&s);assert(v.hot==6&&!v.pages[6].present);f->page.cursor.byte=0;
    f->page.start.byte=1;v=capture(&s);assert(v.hot==6&&!v.pages[6].present);f->page.start.byte=0;
    f->number=14;v=capture(&s);assert(v.hot==6&&!v.pages[6].present);f->number=13;
    f->owner=NULL;v=capture(&s);assert(v.hot==6&&!v.pages[6].present);f->owner=&s;
    add_frame(&s,14,14,false);v=capture(&s);assert(v.hot==7);
    clear_owner(&s);close_session();puts("PASS hot/cold/thaw/pending/cancel/layout/cursors/number/owner/outside");
}
static void cross_chapter(void)
{
    view_state_t s[7];view_page_t p[7];open_session();
    for(unsigned i=0;i<7;++i){owner(&s[i],&p[i],10+i,0,true);add_frame(&s[i],10+i,0,true);}
    state=primary_state=&s[3];s[3].previous=&s[2];s[3].following=&s[4];
    window_slots[0].owner=&s[1];window_slots[0].hop=-2;window_slots[1].owner=&s[0];window_slots[1].hop=-3;
    window_slots[2].owner=&s[5];window_slots[2].hop=2;window_slots[3].owner=&s[6];window_slots[3].hop=3;
    view_window_snapshot_t v=capture(&s[3]);assert(v.hot==7);
    for(unsigned i=0;i<7;++i)assert(v.pages[i].context==(uintptr_t)&s[i]&&v.pages[i].chapter==10+i&&v.pages[i].cursor.record==0);
    window_slots[3].owner=&s[5];v=capture(&s[3]);assert(v.hot==6&&v.pages[6].hot);
    window_slots[3].owner=NULL;v=capture(&s[3]);assert(v.hot==6&&!v.pages[6].mapped);
    window_slots[3].owner=&s[6];reader_page_cache_clear(&s[6].cache);s[6].status.ready=false;
    v=capture(&s[3]);assert(v.hot==6&&v.pages[6].mapped&&!v.pages[6].known);
    for(unsigned i=0;i<7;++i)clear_owner(&s[i]);close_session();memset(window_slots,0,sizeof(window_slots));
    puts("PASS seven short chapters / reverse offsets / unavailable route / metadata pending");
}
static void mixed_chapters(void)
{
    view_state_t s[3];view_page_t p[3];open_session();
    owner(&s[0],&p[0],10,0,false);add_frame(&s[0],10,0,false);add_frame(&s[0],11,1,false);add_frame(&s[0],12,2,true);
    owner(&s[1],&p[1],20,0,false);add_frame(&s[1],20,0,false);add_frame(&s[1],21,1,true);
    owner(&s[2],&p[2],30,0,true);add_frame(&s[2],30,0,true);
    state=primary_state=&s[0];s[0].previous=&s[1];s[0].following=&s[2];
    view_window_snapshot_t v=capture(&s[0]);assert(v.hot==6);
    assert(v.pages[1].context==(uintptr_t)&s[1]&&v.pages[1].number==20);
    assert(v.pages[2].context==(uintptr_t)&s[1]&&v.pages[2].number==21);
    assert(v.pages[4].context==(uintptr_t)&s[0]&&v.pages[4].number==11);
    assert(v.pages[5].context==(uintptr_t)&s[0]&&v.pages[5].number==12);
    assert(v.pages[6].context==(uintptr_t)&s[2]&&v.pages[6].number==30);
    assert(!v.pages[0].mapped);
    for(unsigned i=0;i<3;++i)clear_owner(&s[i]);close_session();puts("PASS mixed local/previous/following pages with real chapter entry selection");
}
static void lifecycle(void)
{
    view_state_t s;view_page_t p;local_window(&s,&p);open_session();view_window_snapshot_t v=capture(&s);
    unsigned serial=v.serial;int old_sleeps=sleeps;
    clear_owner(&s);primary_state=(view_state_t*)(uintptr_t)1;s.frames=(view_frame_t*)(uintptr_t)1;
    for(unsigned i=0;i<10;++i){assert(window_snapshot_copy(&v));assert(v.serial==serial&&v.hot==7);}
    epd_reader_window_diagnostic();assert(sleeps==old_sleeps);s.frames=NULL;state=primary_state=&s;
    ++mailbox.intent;assert(!window_snapshot_copy(&v));--mailbox.intent;
    mailbox.paused=true;assert(!window_snapshot_copy(&v));mailbox.paused=false;
    mailbox.suspended=true;assert(!window_snapshot_copy(&v));mailbox.suspended=false;
    mailbox.replace=true;assert(!window_snapshot_copy(&v));mailbox.replace=false;
    mailbox.stop=true;assert(!window_snapshot_copy(&v));mailbox.stop=false;
    close_session();window_snapshot_publish(&s);assert(!window_snapshot_copy(&v));
    view_state_t *new_ui=open_session();new_ui->session=ui.id=mailbox.session=2;
    window_snapshot_publish(&s);v=(view_window_snapshot_t){0};assert(!window_snapshot_copy(&v)&&v.serial==0);
    s.session=2;s.intent=mailbox.intent;worker_wait(&s,100);assert(sleeps==old_sleeps+1&&last_timeout==100);
    assert(window_snapshot_copy(&v)&&v.hot==0);close_session();puts("PASS immutable copied data / no diagnostic wake / close/reopen/old session/intent/lifecycle");
}
static DWORD WINAPI publish_thread(void *opaque)
{
    view_state_t *s=opaque;
    for(unsigned i=2;i<10002;++i){
        rt_enter_critical();mailbox.intent=i;rt_exit_critical();s->intent=i;s->published_generation=i;
        for(view_frame_t *f=s->frames;f;f=f->next)f->generation=i;
        window_snapshot_publish(s);
    }
    return 0;
}
static void atomic_copy(void)
{
    view_state_t s;view_page_t p;local_window(&s,&p);open_session();s.published_generation=1;capture(&s);
    HANDLE worker=CreateThread(NULL,0,publish_thread,&s,0,NULL);assert(worker);unsigned observed=0;
    do {
        view_window_snapshot_t v={0};window_snapshot_copy(&v);
        assert(v.serial&&v.hot==7&&v.published_generation==v.intent);
        for(unsigned i=0;i<7;++i)assert(v.pages[i].generation==v.intent&&v.pages[i].number==i+7);
        ++observed;
    } while(WaitForSingleObject(worker,0)==WAIT_TIMEOUT);
    CloseHandle(worker);clear_owner(&s);close_session();printf("PASS 10000 publications / concurrent atomic copies %u\n",observed);
}
static void context_guard(void)
{
    view_state_t center, neighbor;view_page_t p;
    local_window(&center,&p);open_session();view_window_snapshot_t v=capture(&center);
    unsigned serial=v.serial;
    state=&neighbor;window_snapshot_publish(&center);
    assert(state==&neighbor&&window_snapshot_copy(&v)&&v.serial==serial);
    state=&center;primary_state=&neighbor;window_snapshot_publish(&center);
    assert(primary_state==&neighbor&&window_snapshot_copy(&v)&&v.serial==serial);
    state=primary_state=&center;v=capture(&center);assert(v.serial==serial+1&&v.hot==7);
    clear_owner(&center);close_session();puts("PASS mismatched state/current/primary rejected without changing worker context");
}
static DWORD WINAPI reopen_thread(void *opaque)
{
    (void)opaque;
    for(unsigned i=0;i<5000;++i) {
        rt_enter_critical();view_state_t *old=ui.session;ui.session=NULL;
        uint32_t id=++ui.id;mailbox.session=id;++mailbox.intent;rt_exit_critical();free(old);
        view_state_t *next=calloc(1,sizeof(*next)+sizeof(view_window_snapshot_t));assert(next);next->session=id;
        rt_enter_critical();ui.session=next;rt_exit_critical();SwitchToThread();
    }
    return 0;
}
static void lifetime_race(void)
{
    view_state_t center;view_page_t p;local_window(&center,&p);open_session();capture(&center);
    HANDLE closer=CreateThread(NULL,0,reopen_thread,NULL,0,NULL);assert(closer);unsigned copies=0;
    do {
        view_window_snapshot_t v={0};window_snapshot_publish(&center);window_snapshot_copy(&v);
        if(v.serial)assert(v.session==1&&v.hot==7);
        ++copies;
    } while(WaitForSingleObject(closer,0)==WAIT_TIMEOUT);
    CloseHandle(closer);view_window_snapshot_t v={0};window_snapshot_publish(&center);
    assert(!window_snapshot_copy(&v)&&v.serial==0);clear_owner(&center);close_session();
    printf("PASS 5000 close/reopen cycles / concurrent publish+copy %u / retired session cannot publish\n",copies);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"states"))valid_states();
    else if(!strcmp(argv[1],"cross"))cross_chapter();
    else if(!strcmp(argv[1],"mixed"))mixed_chapters();
    else if(!strcmp(argv[1],"lifecycle"))lifecycle();
    else if(!strcmp(argv[1],"atomic"))atomic_copy();
    else if(!strcmp(argv[1],"context"))context_guard();
    else if(!strcmp(argv[1],"lifetime-race"))lifetime_race();
    else assert(false);
    return 0;
}
