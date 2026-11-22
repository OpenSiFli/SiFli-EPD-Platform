#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t rt_tick_t;
typedef struct { uint32_t record,byte; } document_cursor_t;
typedef struct view_state { uint32_t session; unsigned references; } view_state_t;
typedef struct view_frame { unsigned unused; } view_frame_t;
typedef struct epd_activity_work { unsigned unused; } epd_activity_work_t;
typedef struct { unsigned unused; } reader_view_t;
static struct {
    view_state_t *session;uint32_t id;view_frame_t *bound,*ready;
    void *save,*user;bool pending;int turn_delta;reader_view_t info;
} ui;
static struct {
    epd_activity_work_t *activity;view_state_t *incoming,*configuration;
    view_frame_t *ready,*ack;bool replace,stop,paused,suspended,rejected;
    uint32_t session,intent;unsigned error,work;
    struct {view_frame_t *origin;unsigned kind;} command;
} mailbox;
static struct {
    view_state_t *context;bool opening;view_frame_t *origin;epd_activity_work_t *activity;
} font_request;
#define INTENT_NONE 0
#define READER_ERROR_NONE 0
#define READER_WORK_IDLE 0
/* TYPES */
static unsigned locks,disposed,reads_after_free,locked_detaches;
static bool window_snapshot_copy(view_window_snapshot_t *snapshot);
static void rt_enter_critical(void) {assert(!locks);++locks;}
static void rt_exit_critical(void) {assert(locks==1);if(!ui.session)++locked_detaches;--locks;}
static void font_trace_stop(void) {}
static void first_font_cancel(void) {}
static bool epd_reader_save(void) {return true;}
static void epd_reader_detach(void) {}
static void frame_release(view_frame_t *f) {(void)f;}
static void epd_activity_work_close(epd_activity_work_t *w) {(void)w;}
static void view_cancel(void) {}
static void wake_view(void) {}
static void epd_reader_collect(void) {}
static void state_dispose(view_state_t *context)
{
    assert(!locks);free(context);++disposed;
    /* Model service preemption between disposal and the next close statement. */
    view_window_snapshot_t snapshot={0};assert(!window_snapshot_copy(&snapshot));++reads_after_free;
}
/* PRODUCTION */
int main(int argc,char **argv)
{
    assert(argc==2);
    view_state_t *context=calloc(1,sizeof(*context)+sizeof(view_window_snapshot_t));assert(context);
    context->session=ui.id=mailbox.session=1;mailbox.intent=1;ui.session=context;
    view_window_snapshot_t *stored=(view_window_snapshot_t*)(context+1);
    stored->serial=stored->session=stored->intent=1;
    view_window_snapshot_t snapshot={0};assert(window_snapshot_copy(&snapshot));
    if(!strcmp(argv[1],"incoming")) {
        context->references=1;mailbox.incoming=context;epd_reader_close();
    } else if(!strcmp(argv[1],"font")) {
        font_request.context=context;font_request.opening=true;font_request_cancel();
    } else assert(false);
    assert(!ui.session&&!locks&&disposed==1&&reads_after_free==1&&locked_detaches);
    assert(!window_snapshot_copy(&snapshot));
    printf("PASS production %s close detaches UI snapshot under critical before freeing initial context\n",argv[1]);
    return 0;
}
