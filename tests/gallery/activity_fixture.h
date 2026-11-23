#ifndef GALLERY_ACTIVITY_FIXTURE_H
#define GALLERY_ACTIVITY_FIXTURE_H
typedef enum { EPD_RESOURCE_OPTIONAL, EPD_RESOURCE_CONTINUOUS, EPD_RESOURCE_NEARBY, EPD_RESOURCE_INTERACTIVE } epd_resource_kind_t;
typedef unsigned epd_resource_activity_t;
typedef struct { unsigned refs, id; epd_resource_kind_t kind; bool settled; } epd_activity_work_t;
typedef struct { unsigned unused; } epd_activity_guard_t;
#define EPD_RESOURCE_OK 0
#define EPD_MEMORY_REGION_HCPU_PSRAM 1
#define EPD_MEMORY_REGION_SRAM 0
static unsigned activity_count, activity_next;
static size_t epd_app_allocation_peak(size_t n) { return n + 32; }
static size_t epd_resource_file_io_peak(size_t n) { return n + 8192; }
static int epd_resource_context_reserve(int region, size_t n) { (void)region; (void)n; return EPD_RESOURCE_OK; }
static epd_activity_work_t *epd_activity_work_queue(epd_resource_kind_t kind, epd_resource_activity_t parent,
    size_t psram, size_t sram, void (*wake)(void *), void *context)
{
    (void)psram; (void)sram; (void)wake; (void)context;
    CHECK(kind != EPD_RESOURCE_NEARBY || parent);
    epd_activity_work_t *w = calloc(1, sizeof(*w)); CHECK(w);
    w->refs = 1; w->id = ++activity_next; w->kind = kind; ++activity_count; return w;
}
static int epd_activity_work_ready(epd_activity_work_t *w) { return w && !w->settled ? 1 : -1; }
static bool epd_activity_work_enter(epd_activity_work_t *w, epd_activity_guard_t *g)
{ (void)g; return epd_activity_work_ready(w) == 1; }
static void epd_activity_work_leave(epd_activity_guard_t *g) { (void)g; }
static void epd_activity_work_close(epd_activity_work_t *w)
{ if (w && !--w->refs) { CHECK(activity_count); --activity_count; free(w); } }
static bool epd_activity_work_retain(epd_activity_work_t *w) { if (!w) return false; ++w->refs; return true; }
static bool epd_activity_work_settle(epd_activity_work_t *w) { w->settled = true; return true; }
static epd_resource_activity_t epd_activity_work_activity(epd_activity_work_t *w) { return w->id; }
#endif
