/* Production queue admission and worker exit with deterministic request I/O. */
typedef uint32_t rt_tick_t;
#include "weather.h"
#include "weather_retry.h"
#define UPDATE_INTERVAL_MS WEATHER_UPDATE_INTERVAL_MS
typedef struct {
    weather_job_t job;
    uint32_t ticket;
    char city_id[WEATHER_CITY_ID_MAX];
} weather_request_t;
static epd_service_t *service;
static struct rt_mutex weather_mutex;
static struct rt_mutex *lock = &weather_mutex;
static int requests = 1;
static bool queue_full, queue_fail, retry_initialized = true;
static weather_request_t queued_request;
static weather_result_t result;
static uint32_t next_ticket;
static rt_tick_t last_period;
static weather_retry_t retries[WEATHER_RETRY_COUNT];
static unsigned executed_jobs, saved_retries;
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return pressure_ticks(ms); }
static bool weather_cancelled(void) { return service && epd_service_cancelled(service); }
bool weather_city_id_valid(const char *id) { return id && *id; }
static int rt_mq_send(int queue, const void *data, size_t size)
{
    (void)queue; CHECK(size == sizeof(queued_request));
    if (queue_full || queue_fail) return -RT_EBUSY;
    memcpy(&queued_request, data, size); queue_full = true; return RT_EOK;
}
static int rt_mq_recv(int queue, void *data, size_t size, int wait)
{
    (void)queue; (void)wait; CHECK(size == sizeof(queued_request));
    if (!queue_full) return -RT_EBUSY;
    memcpy(data, &queued_request, size); queue_full = false; return RT_EOK;
}
static void schedule_refresh(bool explicit_request) { (void)explicit_request; }
bool weather_retry_due(weather_retry_t *state, rt_tick_t now) { (void)state; (void)now; return false; }
static bool network_ready(void) { return false; }
static bool weather_config_valid(const weather_config_t *cfg) { (void)cfg; return true; }
void weather_get_config(weather_config_t *cfg) { memset(cfg, 0, sizeof(*cfg)); }
static bool refresh(const weather_config_t *cfg) { (void)cfg; return true; }
static bool save_retries(void) { ++saved_retries; return true; }
static bool run_request(const weather_request_t *request)
{
    CHECK(request->job == WEATHER_JOB_IMPORT);
    ++executed_jobs;
    CHECK(service->reclaim_holds == 1);
    app_memory_pressure_process("books", false);
    CHECK(!service->stopping); /* Saving cannot be cancelled by pressure. */
    app_service_stop("weather"); /* Explicit shutdown after the save finishes. */
    return true;
}
#include "weather.inc"

static void weather_queued_save(void)
{
    service = reclaimable("weather", &ai_definition);
    epd_service_t *owner = service;
    queue_fail = true;
    CHECK(weather_request_job(WEATHER_JOB_IMPORT, NULL, NULL) == -RT_EBUSY);
    CHECK(!owner->reclaim_holds && !result.busy);
    queue_fail = false;
    uint32_t ticket;
    CHECK(weather_request_job(WEATHER_JOB_IMPORT, NULL, &ticket) == RT_EOK && ticket);
    CHECK(owner->reclaim_holds == 1);
    CHECK(weather_request_job(WEATHER_JOB_IMPORT, NULL, NULL) == -RT_EBUSY);
    CHECK(owner->reclaim_holds == 1);
    low_memory(); app_memory_pressure_request(1000);
    app_memory_pressure_process("books", false);
    CHECK(!owner->stopping);
    worker_entry(owner);
    CHECK(executed_jobs == 1 && saved_retries == 1 && !owner->reclaim_holds && !service);
    retire_stopped();
    CHECK(!services && !pins && !allocations && !subscriptions);
}

static void weather_abandoned_job(void)
{
    service = reclaimable("weather", &ai_definition);
    epd_service_t *owner = service;
    CHECK(weather_request_job(WEATHER_JOB_IMPORT, NULL, NULL) == RT_EOK);
    app_service_stop("weather");
    CHECK(weather_request_job(WEATHER_JOB_IMPORT, NULL, NULL) == -RT_EBUSY);
    worker_entry(owner);
    CHECK(!executed_jobs && saved_retries == 1 && !owner->reclaim_holds && !queue_full);
    retire_stopped();
    CHECK(!services && !pins && !allocations && !subscriptions);
}
