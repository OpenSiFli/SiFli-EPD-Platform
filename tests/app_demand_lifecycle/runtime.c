#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define RT_EOK 0
typedef struct { bool cancelled; } epd_service_t;
typedef struct {
    bool ready, busy, dirty, online, transcribing, recording, playing, preview, generating;
    unsigned status, error, card;
    struct { bool output_active; } speech;
} demo_view_t;
typedef demo_view_t notes_status_t;
#define DEMO_LOADING 1
#define CMD_SEND 100
#define DEMO_RETRY 2
static epd_service_t owner;
static struct {
    void *lock, *mutex, *head, *network, *record, *player;
    epd_service_t *service, *owner;
    bool ready, busy, queued, dirty, save_ok, draft_pending, flush_pending, flushing;
    bool speech_busy, speech_started, idle_stopping, retired, stopping, save_failed;
    unsigned read_commands;
    demo_view_t view;
    struct { unsigned session; } store;
    struct { unsigned kind; } command;
} storage, *state = &storage;
static unsigned checks, stops, requests, starts, disposed, initialized, ui_retired;
static bool running, ui_live, lock_busy, module_present = true, start_failure;
static const int epd_app_background = 0;
static int rt_mutex_take(void *m, int timeout) { (void)m; (void)timeout; return lock_busy ? 1 : 0; }
static void lock(void) { CHECK(!lock_busy); }
static void unlock(void) {}
static void changed(void) {}
static bool demo_ui_present(void) { return ui_live; }
static bool notes_ui_present(void) { return ui_live; }
static void demo_ui_release(void) { ++ui_retired; }
static void notes_ui_retire(void) { ++ui_retired; }
static void epd_service_ui_request(epd_service_t *s, unsigned delay)
{ CHECK(s == &owner && delay <= 100); ++requests; }
static void app_service_stop(const char *id)
{ CHECK(!strcmp(id,"ai_demo") || !strcmp(id,"notes")); ++stops; owner.cancelled = true; }
static bool app_service_running(const char *id) { (void)id; return running; }
static bool epd_service_cancelled(epd_service_t *s) { return s->cancelled; }
static void *dlmodule_find(const char *id) { (void)id; return module_present ? &owner : NULL; }
static epd_service_t *app_service_start(const char *id, const int *definition, void *module)
{
    (void)id; CHECK(!running && definition == &epd_app_background && module);
    ++starts; if (start_failure) return NULL;
    running = true; owner.cancelled = false; state->owner = state->service = &owner;
    return &owner;
}
static void cleanup(void) { CHECK(!running); ++disposed; memset(state,0,sizeof(*state)); }
static void init(void) { ++initialized; state->ready = state->view.ready = state->save_ok = true; }
static void demo_deinit(void) { cleanup(); }
static void demo_init(void) { init(); }
static void notes_runtime_cleanup(void) { cleanup(); }
static void notes_runtime_init(void) { init(); }
#ifdef APP_AI
#define START demo_start
#define CLOSED demo_ui_closed
#else
#define START notes_runtime_start
#define CLOSED notes_runtime_ui_closed
#endif
#include "actual.inc"

static void reset(void)
{
    memset(state,0,sizeof(*state)); owner.cancelled=false;
    state->owner=state->service=&owner;
    state->ready=state->view.ready=state->save_ok=true;
    state->speech_started=true;
    running=true; ui_live=lock_busy=false; module_present=true; start_failure=false;
    stops=requests=starts=disposed=initialized=ui_retired=0;
}
static void blocked(void)
{
    CHECK(!ui_process(&owner,false)); CHECK(!stops && !owner.cancelled && requests==1);
    CHECK(!state->idle_stopping);
}
static void pending_work(void)
{
#ifdef APP_AI
    bool *flags[]={&state->busy,&state->queued,&state->dirty,&state->draft_pending,
        &state->flush_pending,&state->speech_busy,&state->view.speech.output_active};
#else
    bool *flags[]={&state->view.dirty,&state->save_failed,&state->flushing,&state->view.busy,
        &state->view.online,&state->view.transcribing,&state->view.recording,&state->view.playing,&state->view.preview};
#endif
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);++i) { reset(); *flags[i]=true; blocked(); }
    reset(); lock_busy=true; blocked();
#ifdef APP_AI
    reset(); state->save_ok=false; blocked();
    reset(); state->read_commands=256; blocked();
    reset(); state->read_commands=512; blocked();
    reset(); state->ready=false; blocked();
    reset(); state->speech_started=false; state->read_commands=256;
    CHECK(!ui_process(&owner,false) && stops==1);
    /* A completed persistence barrier can leave flushing set until the next command. */
    reset(); state->flushing=true; CHECK(!ui_process(&owner,false)); CHECK(stops==1);
#else
    void **jobs[]={&state->head,&state->network,&state->record,&state->player};
    for(unsigned i=0;i<sizeof(jobs)/sizeof(jobs[0]);++i) { reset(); *jobs[i]=&owner; blocked(); }
    reset(); state->view.ready=false; blocked();
    reset(); state->view.dirty=state->save_failed=true; blocked();
#endif
}
static void reopen(void)
{
    reset(); ui_live=true; CHECK(!ui_process(&owner,false)); CHECK(!stops && !requests);
    ui_live=false; CLOSED(); CHECK(requests==1);
    CHECK(!ui_process(&owner,false)); CHECK(stops==1 && state->idle_stopping);
    ui_live=true;
    CHECK(!START() && !starts && !disposed); /* Old code and state still belong to cleanup. */
    CHECK(ui_process(&owner,true)); CHECK(state->retired && !ui_retired);
    CHECK(!START() && !starts && !disposed); /* Removal, not cleanup callback, ends ownership. */
    running=false;
    CHECK(START() && starts==1 && disposed==1 && initialized==1);
    CHECK(START() && starts==1 && disposed==1);
    CHECK(!state->retired && !state->idle_stopping && !owner.cancelled);
}
static void startup_failure(void)
{
    reset(); running=false; module_present=false; CHECK(!START() && !starts);
    module_present=true; start_failure=true; CHECK(!START() && starts==1 && !running);
    start_failure=false; CHECK(START() && starts==2);
    reset(); ui_live=true; owner.cancelled=true;
    CHECK(ui_process(&owner,true) && ui_retired==1); /* External stop still retires old UI. */
}
static void unpublished_owner(void)
{
    reset(); ui_live=true; state->service=state->owner=NULL;
    state->ready=state->view.ready=false; state->speech_started=false;
    state->view.status=DEMO_LOADING;
    CHECK(START() && !starts && !disposed);
    demo_view_t view;
#ifdef APP_AI
    CHECK(demo_snapshot(&view)); CHECK(view.status==DEMO_LOADING && !view.error && !view.generating);
#else
    CHECK(notes_status(&view)); CHECK(!view.ready && !view.error && !view.card);
#endif
    CHECK(!ui_process(&owner,false) && !stops);
    ui_live=false; blocked(); /* Startup is still in progress, not a terminal failure. */
    state->owner=state->service=&owner;
    state->ready=state->view.ready=true; state->view.error=4;
    CHECK(!ui_process(&owner,false) && stops==1); /* Failed storage initialization has settled. */
    CHECK(ui_process(&owner,true) && state->retired);
}
int main(void)
{
    pending_work(); reopen(); startup_failure(); unpublished_owner();
    printf("PASS application runtime: %u checks\n",checks); return 0;
}
