/* Scheduling and ownership doubles for the production music service helpers. */
#include <stdarg.h>
typedef int32_t rt_int32_t;
#define RT_WAITING_FOREVER (-1)
#define RT_EOK 0
#define EPD_APP_PSRAM 1
typedef struct command_node {
    struct command_node *next;
    music_command_t command;
} command_node_t;
static command_node_t *commands, *commands_tail;
static int guard;
static void *current_page;
static bool initialized = true, accepting, retire_ready, settled;
static bool ui_active, cover_refresh, has_pending_media, media_dirty, flush_ok, flush_ack_pending;
static bool cancelled, sleep_admit = true, cover_busy, mutex_busy;
static bool background_cancelled, ordinary_admit = true;
static uint32_t flush_requested, flush_completed;
static unsigned stops, ui_requests, last_ui_delay, wakes, queue_allocations, view_stops;
static void (*during_alloc)(void);
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
static int rt_mutex_take(int *mutex, rt_int32_t delay)
{
    CHECK(mutex == &guard && delay == 0);
    if (mutex_busy) return -1;
    lock(); return RT_EOK;
}
static void *epd_app_alloc(size_t bytes, int region)
{
    CHECK(region == EPD_APP_PSRAM);
    void *p = calloc(1, bytes); CHECK(p); ++queue_allocations;
    if (during_alloc) { void (*hook)(void) = during_alloc; during_alloc = NULL; hook(); }
    return p;
}
static void epd_app_free(void *p)
{ if (p) { CHECK(queue_allocations); --queue_allocations; free(p); } }
static bool epd_service_cancelled(epd_service_t *service)
{ CHECK(service == worker); return cancelled; }
static bool epd_service_background_cancelled(epd_service_t *service)
{ CHECK(service == worker); return background_cancelled; }
static bool epd_service_sleep_acquire(epd_service_t *service)
{ CHECK(service == worker); return sleep_admit; }
static unsigned work_holds;
static bool epd_service_reclaim_hold(epd_service_t *service)
{ CHECK(service==worker);if(cancelled||!ordinary_admit)return false;++work_holds;return true; }
static void epd_service_reclaim_release(epd_service_t *service)
{ CHECK(service==worker && work_holds);--work_holds; }
static bool epd_service_mark_dirty(epd_service_t *service) { CHECK(service==worker);return true; }
#include "../unified_operation/cleanup_ports.h"
#include "app_work.inc"
#include "app_cleanup.inc"
static app_work_t work;
static bool save_failed;
static void epd_service_sleep_release(epd_service_t *service)
{ CHECK(service == worker || (!worker && cancelled)); }
static void epd_service_wake(epd_service_t *service) { CHECK(service == worker); ++wakes; }
static void epd_service_ui_request(epd_service_t *service, uint32_t delay)
{ CHECK(service == worker); ++ui_requests; last_ui_delay = delay; }
enum {
    EPD_SERVICE_UI_DISPLAY = 1u << 0,
    EPD_SERVICE_UI_SETTINGS = 1u << 4,
    EPD_SERVICE_UI_STORAGE = 1u << 5
};
static void epd_service_ui_watch(epd_service_t *service, uint32_t events)
{
    CHECK(service == worker);
    CHECK(events == (EPD_SERVICE_UI_DISPLAY | EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_STORAGE));
}
void epd_media_note_intent(epd_service_t *service) { CHECK(service == worker); }
static bool music_cover_busy(void) { return cover_busy; }
void music_view_update(void) {}
void music_view_stop(void) { ++view_stops; current_page = NULL; }
static void app_service_stop(const char *id)
{
    CHECK(!strcmp(id, "music") && !lock_depth && !accepting && !retire_ready);
    CHECK(!dirty && !commands && !has_pending_media && !session_active);
    cancelled = true; ++stops;
}
static int rt_kprintf(const char *line, ...)
{ va_list ap; va_start(ap, line); int count = vprintf(line, ap); va_end(ap); return count; }
void music_backend_request_cancel(music_backend_t *b) { CHECK(b == backend); }

#define STORAGE_SD 1
#define STORAGE_FLASH_ROOT "/flash"
static epd_media_command_t pending_media;
static epd_media_t previous_media;
static uint64_t previous_item;
static bool previous_quiet;
#define AUDIO_TYPE_LOCAL_MUSIC 1u
bool epd_media_publish_state(epd_service_t *service, const epd_media_t *media, uint64_t item, bool quiet)
{ CHECK(service == worker); (void)media; (void)item; (void)quiet; return true; }
void epd_media_intent_settled(epd_service_t *service) { CHECK(service == worker); }
static uint32_t card_session;
static unsigned destroyed, cover_stops, wait_phase, run_case;
static void release_wait(epd_service_t *service, rt_int32_t delay);
static uint32_t epd_service_wait(epd_service_t *service, rt_int32_t delay);
static void music_cover_set_wake(void (*notify)(void *), void *context)
{ (void)notify; (void)context; }
static uint32_t storage_card_session(void) { return 1; }
static bool storage_available(int volume) { (void)volume; return true; }
static size_t epd_app_psram_available(void) { return 1000000; }
music_backend_t *music_backend_create(music_backend_wake_fn notify, void *context)
{ (void)notify; (void)context; return (music_backend_t *)&audio; }
void music_backend_destroy(music_backend_t *old) { CHECK(old == (music_backend_t *)&audio); ++destroyed; }
int music_backend_pump(music_backend_t *b) { CHECK(b == backend); return 0; }
bool epd_media_poll(epd_service_t *service, epd_media_command_t *command)
{ CHECK(service == worker); (void)command; return false; }
static void trim_caches(void) {}
music_model_result_t music_library_scan_step(music_library_scan_t *value, music_library_t *out, size_t batch)
{
    CHECK(value && batch == 8);
    if (!scan_limit) return MUSIC_MODEL_END;
    CHECK(!out->entries);
    out->entries = music_model_alloc(sizeof(*out->entries)); CHECK(out->entries);
    out->count = out->capacity = 1;
    out->entries[0].path = music_model_strdup("/sd/incomplete.mp3");
    out->entries[0].name = music_model_strdup("incomplete.mp3");
    CHECK(out->entries[0].path && out->entries[0].name);
    return MUSIC_MODEL_LIMIT;
}
static void music_cover_stop(void) { ++cover_stops; }
