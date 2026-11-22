#include "support.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef unsigned rt_tick_t;
typedef struct { unsigned record,byte; } document_cursor_t;
typedef struct { document_cursor_t start,cursor; bool eof; unsigned generation; } view_page_t;
typedef struct view_state view_state_t;
typedef struct view_frame {
    struct view_frame *next;
    view_state_t *owner;
    unsigned references,intent,layout,generation,number,handoff_intent;
    bool required,cancelled,render_finished,resources_ready,pins_pending,complete;
    epd_reader_render_t renderer;
    epd_compute_buffer_t pixels;
    view_page_t page;
    void *pins;
    unsigned pin_run,pin_byte,render_run,render_byte;
} view_frame_t;
struct view_state {
    view_state_t *previous,*following;
    unsigned session,intent,layout,wanted,preview_page,navigation_link,navigation_toc;
    bool initialized,displayed,parked,selected,frame_published,target_pending,initial_target_ready;
    bool boundary_target,forward_step,numeric_target,preview_cache_active;
    document_cursor_t initial_anchor;
    int segment_turns,deferred_turns;
    struct { bool ready; } status;
    view_frame_t *rendering;
    view_page_t *building,*display;
    void *back_request,*document;
    int cache;
};
typedef enum { INTENT_NONE,INTENT_SEEK,INTENT_TURN,INTENT_CURSOR,INTENT_CANCEL,
               INTENT_TOC,INTENT_CHAPTER,INTENT_LINK,INTENT_REFLOW,INTENT_PROGRESS } view_intent_t;
typedef struct {
    view_intent_t kind;int value;document_cursor_t cursor;
    unsigned session,serial;view_frame_t *origin;
} view_command_t;
static struct { view_state_t *context; view_command_t command; } font_request;
typedef struct { document_cursor_t start,end; bool eof; } reader_cached_page_t;
static reader_cached_page_t *reader_page_cache_find(int *cache,unsigned number,unsigned layout)
{ (void)cache;(void)number;(void)layout;return NULL; }
static bool frame_window_contains(const view_frame_t *frame,const view_page_t *page,unsigned number)
{ (void)frame;(void)page;(void)number;assert(0);return false; }
static struct {
    bool stop,suspended,paused;unsigned session,intent,requested_at;
    view_command_t command;view_frame_t *ready;
} mailbox;
static struct {
    view_state_t *session;unsigned id,intent,requested_at;int turn_delta;
    view_frame_t *bound,*ready;bool pending;
} ui;
static view_state_t owner,other,*state,*parked_state;
#define view (*state)
static view_frame_t *render_frame;
static epd_reader_render_t render_active;
static epd_compute_wait_t render_wait;
static bool render_required;
static unsigned locks,released,turns,wakes;
static void (*unlock_hook)(void);
static void rt_enter_critical(void) { ++locks; }
static void rt_exit_critical(void) {
    assert(locks);--locks;
    if(!locks && unlock_hook) { void (*hook)(void)=unlock_hook;unlock_hook=NULL;hook(); }
}
static rt_tick_t rt_tick_get(void) { return 100; }
static unsigned elapsed_ms(rt_tick_t start) { (void)start;return 0; }
#define rt_kprintf(...) do { if(0)printf(__VA_ARGS__); } while(0)
static bool cursor_equal(document_cursor_t a,document_cursor_t b)
{ return a.record==b.record && a.byte==b.byte; }
static void frame_ref(view_frame_t *f) { if(f)++f->references; }
static void frame_release(view_frame_t *f) {
    if(!f)return;
    assert(f->references);if(!--f->references) { ++released;if(render_frame==f)render_frame=NULL; }
}
static void index_cancel(void) {}
static void view_cancel(void);
static void wake_view(void) { ++wakes; }
static void cluster_retire(view_state_t *s) { (void)s; }
static void document_record_cancel(void *r) { (void)r; }
static void pagination_seek(unsigned n,view_state_t **s) { (void)n;(void)s; }
static void progress_seek(unsigned n,view_state_t **s) { (void)n;(void)s; }
static void core_seek_cursor(document_cursor_t p) { (void)p; }
static void core_cancel_pending(void) {}
static void core_turn(int n) { assert(n);++turns; }
static view_state_t *chapter_select(view_state_t *s,int n,void *d)
{ (void)s;(void)n;(void)d;return &other; }
typedef void document_t;
#define DOCUMENT_TEXT_LIMIT 1024
static void epd_reader_note_close(void) {}
static bool document_toc_entry(document_t *d,unsigned index,char *title,size_t title_size,
                               char *target,size_t target_size,unsigned *level)
{ (void)d;(void)index;(void)title;(void)title_size;(void)target;(void)target_size;(void)level;assert(0);return false; }
static void target_select(document_t *d,const char *target,view_state_t **current)
{ (void)d;(void)target;(void)current;assert(0); }
static document_t *document_toc_open(void *d,unsigned n,char *error,size_t bytes)
{ (void)n;(void)error;(void)bytes;return d; }
#define READER_ERROR_LINK_MISSING 1
static void fail(int error) { (void)error;assert(0); }
static void follow_link(unsigned n,view_state_t **s) { (void)n;(void)s; }
static void follow_toc(unsigned n,view_state_t **s) { (void)n;(void)s;assert(0); }
static void navigation_clear(void) { view.navigation_link=view.navigation_toc=0; }
static void link_clear(void) { navigation_clear(); }
static unsigned window_contexts(view_state_t *current,view_state_t **out)
{ unsigned count=0;out[count++]=current;if(current->previous)out[count++]=current->previous;if(current->following)out[count++]=current->following;return count; }
static bool core_waiting(void) { return true; }
static view_frame_t *frame_find(document_cursor_t p) { (void)p;return NULL; }
static void frame_publish(view_frame_t *f) { (void)f;assert(0); }
static void frame_trace_note(const view_frame_t *frame,const char *event,uint32_t intent)
{ (void)frame;(void)event;(void)intent; }
/* PRODUCTION */
static void view_cancel(void) { index_cancel();render_cancel(); }

static view_frame_t origin,frame;
static view_page_t target;
static const rr_frame format={32,16,32,RR_FORMAT_L8};
static void draw(unsigned x) {
    rr_command q;rr_command_init(&q,RR_BLIT_A8,format);
    q.x=(int)x;q.y=2;q.width=q.height=q.src_width=q.src_height=q.src_stride=4;
    unsigned char pixels[16];memset(pixels,255,sizeof(pixels));
    assert(render_submit(&frame,&q,pixels,sizeof(pixels))==EPD_COMPUTE_OK);
    memset(pixels,0xcc,sizeof(pixels));
}
static void setup_stage(bool reverse,bool pins_only) {
    memset(&mailbox,0,sizeof(mailbox));memset(&ui,0,sizeof(ui));
    memset(&owner,0,sizeof(owner));memset(&other,0,sizeof(other));
    memset(&frame,0,sizeof(frame));memset(&origin,0,sizeof(origin));memset(&target,0,sizeof(target));
    owner.session=mailbox.session=ui.id=7;owner.intent=mailbox.intent=ui.intent=9;
    owner.layout=33;owner.status.ready=owner.selected=owner.displayed=true;
    owner.building=&target;owner.wanted=6;state=ui.session=&owner;
    origin.owner=frame.owner=&owner;origin.layout=frame.layout=33;origin.references=1;
    origin.number=6;frame.number=reverse?5:7;
    origin.page.start=(document_cursor_t){200,5};origin.page.cursor=(document_cursor_t){300,9};
    frame.page.start=reverse?(document_cursor_t){100,3}:origin.page.cursor;
    frame.page.cursor=reverse?origin.page.start:(document_cursor_t){400,7};
    frame.references=1;frame.intent=9;frame.generation=17;
    frame.pins=malloc(12);assert(frame.pins);memset(frame.pins,0x5a,12);
    frame.pin_run=3;frame.pin_byte=19;
    frame.render_run=pins_only?0:2;frame.render_byte=pins_only?0:11;
    assert(epd_compute_buffer_alloc(&frame.pixels,format.stride*format.height));
    owner.rendering=&frame;render_frame=NULL;ui.bound=&origin;released=turns=wakes=0;
    if(!pins_only) {
        assert(epd_reader_render_open(&frame.pixels,format,EPD_COMPUTE_BACKGROUND,&frame.renderer)==EPD_COMPUTE_OK);
        render_frame=&frame;
        rr_command init;rr_command_init(&init,RR_INIT,format);
        assert(render_submit(&frame,&init,NULL,0)==EPD_COMPUTE_OK);
        draw(2);assert(render_flush(&frame)==EPD_COMPUTE_OK);
        draw(10); /* One copied batch remains queued across the TURN. */
    }
    target=frame.page;target.generation=99;
}
static void setup(bool reverse) { setup_stage(reverse,false); }
static void dispose(void) {
    if(mailbox.command.origin)frame_release(mailbox.command.origin);
    mailbox.command.origin=NULL;
    if(owner.rendering) { frame_release(owner.rendering);owner.rendering=NULL; }
    assert(origin.references==1 && !frame.references);
    assert(epd_reader_render_shutdown(&frame.renderer)==EPD_COMPUTE_OK);
    epd_compute_buffer_free(&frame.pixels);free(frame.pins);frame.pins=NULL;
    assert(!test_host_live && !test_remote_live && !locks && !render_active);
}
static void consume(void) {
    view_command_t command=mailbox.command;
    mailbox.command.kind=INTENT_NONE;mailbox.command.origin=NULL;
    view_state_t *current=&owner;
    command_apply(command,&current);
    frame_release(command.origin);
}
static void matched(bool reverse,bool in_flight) {
    setup(reverse);unsigned opened=test_open_calls,cancelled=test_cancel_calls;
    void *pixels=frame.pixels.data,*pins=frame.pins;epd_reader_render_t renderer=frame.renderer;
    if(in_flight)render_active=frame.renderer;
    ui_command(INTENT_TURN,reverse?-1:1,(document_cursor_t){0,0});
    assert(test_cancel_calls==cancelled && !render_cancelled(&frame));
    assert(frame.handoff_intent==10);
    render_active=NULL;consume();
    assert(owner.rendering==&frame && !released && turns==1);
    assert(frame_adopt(&frame,&target) && frame.intent==10 && frame.generation==99);
    assert(frame.pixels.data==pixels && frame.pins==pins && frame.renderer==renderer);
    assert(frame.pin_run==3 && frame.pin_byte==19 && frame.render_run==2 && frame.render_byte==11);
    assert(((unsigned char*)pins)[11]==0x5a);
    draw(18);rr_command finish;rr_command_init(&finish,RR_FINISH,format);
    assert(render_submit(&frame,&finish,NULL,0)==EPD_COMPUTE_OK);
    assert(test_open_calls==opened && test_cancel_calls==cancelled);
    const uint8_t *out=pixels;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x) {
        bool ink=y>=2 && y<6 && ((x>=2 && x<6)||(x>=10 && x<14)||(x>=18 && x<22));
        assert(out[y*32+x]==(ink?0:255));
    }
    dispose();
}
static void rejected(unsigned why) {
    setup(false);render_active=frame.renderer;unsigned cancelled=test_cancel_calls;
    view_intent_t kind=INTENT_TURN;int value=1;
    if(why==0)value=-1;
    if(why==1)value=2;
    if(why==2)kind=INTENT_CANCEL;
    if(why==3)kind=INTENT_CHAPTER;
    if(why==4)kind=INTENT_CURSOR;
    if(why==5)++origin.layout;
    if(why==6) { ++origin.page.cursor.byte;frame.number=origin.number+1; }
    if(why==7)origin.owner=&other;
    if(why==8)mailbox.stop=true;
    if(why==9)++mailbox.session;
    if(why==10)mailbox.suspended=true;
    if(why==11)mailbox.paused=true;
    ui_command(kind,value,(document_cursor_t){0,0});
    if(why==8 || why==9 || why==10)view_cancel();
    bool consumed=why==0;
    if(consumed)consume();
    assert(render_cancelled(&frame));
    if(why>=1 && why<=7)assert(test_cancel_calls>cancelled);
    render_active=NULL;
    assert(render_flush(&frame)==EPD_COMPUTE_CANCELLED);
    assert(!frame_adopt(&frame,&target));
    if(!consumed)consume();
    assert(!owner.rendering && released==1);
    /* Cancellation retains the borrowed frame until CLOSE actually succeeds. */
    void *pixels=frame.pixels.data,*pins=frame.pins;
    test_close_busy=1;assert(epd_reader_render_shutdown(&frame.renderer)==EPD_COMPUTE_BUSY);
    assert(frame.renderer && frame.pixels.data==pixels && frame.pins==pins);
    test_close_busy=0;dispose();
}
static void superseded(void) {
    setup(false);render_active=frame.renderer;
    ui_command(INTENT_TURN,1,(document_cursor_t){0,0});
    ui_command(INTENT_CANCEL,0,(document_cursor_t){0,0});
    ui_command(INTENT_TURN,1,(document_cursor_t){0,0});
    assert(frame.cancelled && render_cancelled(&frame));
    render_active=NULL;consume();assert(!owner.rendering && released==1);dispose();
}
static void turn_during_submit(void) { ui_command(INTENT_TURN,1,(document_cursor_t){0,0}); }
static void preempted(void) {
    setup(false);unsigned cancelled=test_cancel_calls;
    unlock_hook=turn_during_submit;
    assert(render_flush(&frame)==EPD_COMPUTE_OK && !unlock_hook);
    assert(test_cancel_calls==cancelled);consume();
    assert(owner.rendering==&frame && frame_adopt(&frame,&target));dispose();
}
static void pins_matched(bool reverse,bool resources_ready) {
    setup_stage(reverse,true);
    frame.resources_ready=resources_ready;frame.pins_pending=!resources_ready;
    unsigned opened=test_open_calls,cancelled=test_cancel_calls;
    void *pixels=frame.pixels.data,*pins=frame.pins;
    ui_command(INTENT_TURN,reverse?-1:1,(document_cursor_t){0,0});
    /* The resource loop can poll cancellation before the worker consumes TURN.
     * A pins-only frame has never published render_frame or render_active. */
    assert(!render_frame && !render_active && !frame.renderer);
    if(render_cancelled(&frame)) {
        fprintf(stderr,"FAIL pins-only exact TURN cancelled before command consumption\n");exit(1);
    }
    assert(frame.handoff_intent==10);consume();
    assert(owner.rendering==&frame && !released && turns==1);
    assert(frame_adopt(&frame,&target) && frame.intent==10 && frame.generation==99);
    assert(frame.pixels.data==pixels && frame.pins==pins && !frame.renderer);
    assert(frame.pin_run==3 && frame.pin_byte==19 && !frame.render_run && !frame.render_byte);
    assert(frame.resources_ready==resources_ready && frame.pins_pending==!resources_ready);
    assert(((unsigned char*)pins)[11]==0x5a && test_open_calls==opened && test_cancel_calls==cancelled);
    assert(epd_reader_render_open(&frame.pixels,format,EPD_COMPUTE_FOREGROUND,&frame.renderer)==EPD_COMPUTE_OK);
    render_frame=&frame;frame.required=true;
    rr_command q;rr_command_init(&q,RR_INIT,format);
    assert(render_submit(&frame,&q,NULL,0)==EPD_COMPUTE_OK);draw(2);
    rr_command_init(&q,RR_FINISH,format);
    assert(render_submit(&frame,&q,NULL,0)==EPD_COMPUTE_OK);
    assert(test_open_calls==opened+1 && test_cancel_calls==cancelled);
    const uint8_t *out=pixels;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x)
        assert(out[y*32+x]==(y>=2 && y<6 && x>=2 && x<6?0:255));
    dispose();
}
static void pins_rejected(unsigned why) {
    setup_stage(false,true);
    view_intent_t kind=INTENT_TURN;int value=1;
    if(why==0)value=-1;
    if(why==1)value=2;
    if(why==2)kind=INTENT_CANCEL;
    if(why==3)kind=INTENT_REFLOW;
    if(why==4)kind=INTENT_CURSOR;
    if(why==5)++origin.layout;
    if(why==6) { ++origin.page.cursor.byte;frame.number=origin.number+1; }
    if(why==7)origin.owner=&other;
    if(why==8)++owner.layout;
    if(why==9)origin.page.eof=true;
    if(why==10)frame.cancelled=true;
    if(why==11) { free(frame.pins);frame.pins=NULL; }
    ui_command(kind,value,(document_cursor_t){0,0});
    /* Consume without polling: command_apply must enforce the same match. */
    consume();
    if(owner.rendering || released!=1) {
        fprintf(stderr,"FAIL mismatched pins-only frame retained at command consumption reason=%u\n",why);exit(1);
    }
    assert(!frame.renderer && !render_frame);dispose();
}
static void target_changed(bool eof) {
    setup_stage(false,true);
    ui_command(INTENT_TURN,1,(document_cursor_t){0,0});consume();
    assert(owner.rendering==&frame);
    if(eof)target.eof=!frame.page.eof;else ++target.cursor.byte;
    if(frame_adopt(&frame,&target)) {
        fprintf(stderr,"FAIL pins-only frame adopted after target end changed\n");exit(1);
    }
    dispose();
}
static void pins_terminal(unsigned why) {
    setup_stage(false,true);
    ui_command(INTENT_TURN,1,(document_cursor_t){0,0});
    assert(!render_cancelled(&frame));
    if(why==0)mailbox.stop=true;
    if(why==1)mailbox.suspended=true;
    if(why==2)++mailbox.session;
    if(why==3)mailbox.paused=true;
    if(why==4)ui_command(INTENT_CANCEL,0,(document_cursor_t){0,0});
    assert(render_cancelled(&frame) && frame.cancelled);
    mailbox.stop=mailbox.suspended=mailbox.paused=false;mailbox.session=owner.session;
    ui_command(INTENT_TURN,1,(document_cursor_t){0,0});
    assert(render_cancelled(&frame));consume();
    assert(!owner.rendering && released==1 && !frame.renderer);dispose();
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"forward"))matched(false,false);
    else if(!strcmp(argv[1],"reverse"))matched(true,false);
    else if(!strcmp(argv[1],"inflight"))matched(false,true);
    else if(!strcmp(argv[1],"preempted"))preempted();
    else if(!strcmp(argv[1],"superseded"))superseded();
    else if(!strcmp(argv[1],"pins-forward"))pins_matched(false,false);
    else if(!strcmp(argv[1],"pins-reverse"))pins_matched(true,false);
    else if(!strcmp(argv[1],"pins-resources"))pins_matched(false,true);
    else if(!strcmp(argv[1],"target-end"))target_changed(false);
    else if(!strcmp(argv[1],"target-eof"))target_changed(true);
    else if(!strncmp(argv[1],"pins-reject-",12)) {
        unsigned why=(unsigned)strtoul(argv[1]+12,NULL,10);assert(why<12);pins_rejected(why);
    }
    else if(!strncmp(argv[1],"pins-terminal-",14)) {
        unsigned why=(unsigned)strtoul(argv[1]+14,NULL,10);assert(why<5);pins_terminal(why);
    }
    else { unsigned why=(unsigned)strtoul(argv[1],NULL,10);assert(why<12);rejected(why); }
    printf("PASS handoff %s: production command/cancel/adopt and real render client/kernel\n",argv[1]);
    return 0;
}
