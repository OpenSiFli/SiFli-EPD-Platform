/* Real rejection policy: optional work cannot invalidate readable body data. */
#define document_t reject_document_t
#define document_reader_destroy reject_reader_destroy
#define worker_changed reject_changed
enum { DOCUMENT_NAV_ACTIVE=1, DOCUMENT_NAV_FAILED=2, BOOK_DOCUMENT_SOURCE_BODY_UTF8=7 };
typedef struct {
    bool work_deferred, converting, nav_requested, chapter_restore, target_required;
    bool body_first, segmented, target_ready, claimed, cancelled;
    uint32_t body_end, chapter_scan, work_revision;
    struct { unsigned unit; } requested;
    document_progress_t status;
    document_reader_t *chapter_reader;
    int nav_result, nav_state;
} reject_document_t;
static unsigned reject_pauses, reject_prefetch, reject_publications, reject_closes;
static void navigation_pause_locked(reject_document_t *d)
{ CHECK(irq && d->nav_state==DOCUMENT_NAV_ACTIVE); ++reject_pauses; }
static void prefetch_release(reject_document_t *d)
{ (void)d; CHECK(!irq); ++reject_prefetch; }
static void convert_finish(reject_document_t *d,int result)
{ (void)d; (void)result; CHECK(false); }
static void document_reader_destroy(document_reader_t *reader)
{ CHECK(!irq); if(reader){++reject_closes;epd_app_free(reader);} }
static void result_locked(reject_document_t *d,int result)
{ CHECK(irq); d->status.result=result; }
static void worker_changed(void) { CHECK(irq); ++reject_publications; }
#include "document_reject_functions.inc"
static void document_reject_case(const char *name)
{
    reject_document_t d={0};
    d.status.complete=true;d.status.end=8192;d.body_first=true;d.body_end=4096;
    d.chapter_restore=true;d.target_ready=true;
    d.chapter_reader=epd_app_alloc(sizeof(document_reader_t),EPD_APP_PSRAM);CHECK(d.chapter_reader);
    if(!strcmp(name,"document_stale")) {
        d.work_revision=2;
        document_work_rejected(&d,true,1);
        CHECK(!d.work_deferred&&d.status.result==BOOK_OK&&d.chapter_reader&&d.chapter_restore);
        CHECK(!reject_pauses&&!reject_prefetch&&!reject_publications&&!reject_closes);
        epd_app_free(d.chapter_reader);
    } else if(!strcmp(name,"document_optional_unfinished")) {
        d.status.complete=false;d.converting=true;
        document_work_rejected(&d,false,d.work_revision);
        CHECK(d.work_deferred&&!d.cancelled&&d.converting&&!d.status.complete&&d.status.result==BOOK_OK);
        CHECK(d.chapter_reader&&d.status.end==8192&&!reject_pauses&&reject_prefetch==1&&!reject_closes);
        epd_app_free(d.chapter_reader);
    } else if(!strcmp(name,"document_optional")) {
        d.nav_state=DOCUMENT_NAV_ACTIVE;
        document_work_rejected(&d,false,d.work_revision);
        CHECK(d.work_deferred&&!d.cancelled&&d.status.result==BOOK_OK&&d.status.end==8192);
        CHECK(d.chapter_reader&&d.chapter_restore&&reject_pauses==1&&reject_prefetch==1&&!reject_closes);
        epd_app_free(d.chapter_reader);
    } else {
        if(!strcmp(name,"document_initial")) d.status.complete=false;
        else if(!strcmp(name,"document_navigation")) d.nav_requested=true;
        else {CHECK(!strcmp(name,"document_restore"));d.target_required=true;}
        document_work_rejected(&d,true,d.work_revision);
        CHECK(d.work_deferred&&!d.cancelled&&!d.chapter_reader&&!d.chapter_restore&&!d.nav_requested);
        CHECK(reject_closes==1&&reject_publications==1&&!reject_prefetch&&d.status.end==8192);
        if(!strcmp(name,"document_navigation"))
            CHECK(d.status.result==BOOK_OK&&d.status.complete&&d.nav_result==BOOK_NO_MEMORY&&d.nav_state==DOCUMENT_NAV_FAILED);
        else CHECK(d.status.result==BOOK_NO_MEMORY&&d.status.complete&&d.claimed);
    }
}
#undef document_t
#undef document_reader_destroy
#undef worker_changed
