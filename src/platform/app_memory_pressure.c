#include "app_memory_pressure.h"
#include "epd_memory.h"
#include "platform/epd_power.h"
#include "app_service_internal.h"
#include "ui/platform/app_catalog.h"
#include <rthw.h>
#include <rtm.h>
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#define PRESSURE_TIMEOUT_MS APP_MEMORY_PRESSURE_TIMEOUT_MS
#define PRESSURE_RETRY_MS 1000u
#define PRESSURE_RESUME_MS 5000u
#define PRESSURE_HEADROOM (128u * 1024u)

static app_memory_pressure_status_t pressure;
/* A rejected diagnostic must not replace an in-flight feasible reclamation. */
static app_memory_pressure_status_t rejected_pressure;
static uint32_t next_generation, rejected_completed;
static bool query_rejected;
static uint32_t pressure_started, pressure_completed, resume_started;
static bool background_blocked, resume_tracking;
static bool force_release, force_stopping, forced_round;
static uint32_t reported_generation;
static uint32_t reported_rejected_generation;

static uint32_t pressure_ticks(uint32_t ms)
{
    return (uint32_t)(((uint64_t)ms * RT_TICK_PER_SECOND + 999u) / 1000u);
}

/* Called under the publication guard; zero remains the satisfied sentinel. */
static uint32_t pressure_generation(void)
{
    if (!++next_generation) ++next_generation;
    return next_generation;
}

static uint32_t pressure_request(size_t min_available, bool force)
{
    size_t available = epd_app_psram_available();
    if (!min_available || (!force && available >= min_available)) return 0;
    size_t capacity = epd_app_psram_capacity();
    uint32_t now = (uint32_t)rt_tick_get();
    bool notify = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (min_available > capacity)
    {
        if (!rejected_pressure.generation ||
            (uint32_t)(now - rejected_completed) >= pressure_ticks(PRESSURE_RETRY_MS))
        {
            rejected_pressure.generation = pressure_generation();
            rejected_completed = now;
            notify = true;
        }
        rejected_pressure.result = APP_MEMORY_PRESSURE_EXHAUSTED;
        rejected_pressure.min_available = min_available;
        rejected_pressure.available = available;
        query_rejected = true;
        uint32_t generation = rejected_pressure.generation;
        rt_hw_interrupt_enable(level);
        if (notify) epd_power_notify();
        return generation;
    }
    query_rejected = false;
    if (pressure.pending)
    {
        if (min_available > pressure.min_available) pressure.min_available = min_available;
        if (force) force_release = forced_round = true;
    }
    else if (!pressure.generation || (force && !forced_round) ||
             (uint32_t)(now - pressure_completed) >= pressure_ticks(PRESSURE_RETRY_MS))
    {
        pressure.generation = pressure_generation();
        pressure.pending = true;
        pressure.result = APP_MEMORY_PRESSURE_PENDING;
        pressure.min_available = min_available;
        pressure.available = available;
        pressure_started = now;
        force_release = force;
        forced_round = force;
        force_stopping = false;
        background_blocked = true;
        resume_tracking = false;
        notify = true;
    }
    uint32_t generation = pressure.generation;
    rt_hw_interrupt_enable(level);
    if (notify) epd_power_notify();
    return generation;
}

uint32_t app_memory_pressure_request(size_t min_available)
{
    return pressure_request(min_available, false);
}
RTM_EXPORT(app_memory_pressure_request);

uint32_t app_memory_pressure_allocation_failed(size_t min_available)
{
    return pressure_request(min_available, true);
}
RTM_EXPORT(app_memory_pressure_allocation_failed);

app_memory_pressure_status_t app_memory_pressure_query(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    app_memory_pressure_status_t snapshot = query_rejected ? rejected_pressure : pressure;
    rt_hw_interrupt_enable(level);
    return snapshot;
}
RTM_EXPORT(app_memory_pressure_query);

bool app_memory_pressure_background_blocked(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool blocked = background_blocked;
    rt_hw_interrupt_enable(level);
    return blocked;
}

static void pressure_finish(app_memory_pressure_result_t result, uint32_t now)
{
    pressure.pending = false;
    pressure.result = result;
    pressure_completed = now;
    resume_tracking = false;
    force_release = force_stopping = false;
}

static void pressure_report_snapshot(app_memory_pressure_status_t snapshot, uint32_t *reported)
{
    if (snapshot.pending || !snapshot.generation || snapshot.generation == *reported) return;
    *reported = snapshot.generation;
    const char *result = snapshot.result == APP_MEMORY_PRESSURE_SATISFIED ? "satisfied" :
                         snapshot.result == APP_MEMORY_PRESSURE_EXHAUSTED ? "exhausted" : "timeout";
    rt_kprintf("mem pressure gen=%lu %s need=%lu free=%lu\n",
               (unsigned long)snapshot.generation, result,
               (unsigned long)snapshot.min_available, (unsigned long)snapshot.available);
}

static void pressure_report(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    app_memory_pressure_status_t active = pressure, rejected = rejected_pressure;
    rt_hw_interrupt_enable(level);
    pressure_report_snapshot(active, &reported_generation);
    pressure_report_snapshot(rejected, &reported_rejected_generation);
}

void app_memory_pressure_process(const char *foreground_id, bool allow_background_resume)
{
    uint32_t now = (uint32_t)rt_tick_get();
    size_t available = epd_app_psram_available();
    size_t capacity = epd_app_psram_capacity();
    bool stopping = app_service_stopping();
    rt_base_t level = rt_hw_interrupt_disable();
    pressure.available = available;
    /* A stop request is not a release: deferred cleanup retires module pins. */
    if (force_stopping && !stopping) force_release = force_stopping = false;
    if (pressure.pending && !force_release && available >= pressure.min_available)
        pressure_finish(APP_MEMORY_PRESSURE_SATISFIED, now);
    if (pressure.pending && (uint32_t)(now - pressure_started) >= pressure_ticks(PRESSURE_TIMEOUT_MS))
        pressure_finish(APP_MEMORY_PRESSURE_TIMED_OUT, now);
    bool pending = pressure.pending;
    bool waiting_release = force_release && force_stopping;
    uint32_t generation = pressure.generation;
    rt_hw_interrupt_enable(level);
    if (pending)
    {
        app_service_reclaim_result_t result = waiting_release ? APP_SERVICE_RECLAIM_WAITING :
            app_service_reclaim_step(foreground_id, generation);
        level = rt_hw_interrupt_disable();
        if (result == APP_SERVICE_RECLAIM_PROGRESS) force_stopping = true;
        if (result == APP_SERVICE_RECLAIM_NONE)
            pressure_finish(APP_MEMORY_PRESSURE_EXHAUSTED, now);
        rt_hw_interrupt_enable(level);
        pressure_report();
        return;
    }
    pressure_report();
    bool can_resume = allow_background_resume && foreground_id && *foreground_id &&
                      !epd_power_sleeping() && !app_service_stopping();
    level = rt_hw_interrupt_disable();
    size_t target = pressure.min_available < capacity ? pressure.min_available : capacity;
    size_t headroom = capacity - target;
    if (headroom > PRESSURE_HEADROOM) headroom = PRESSURE_HEADROOM;
    size_t watermark = target + headroom;
    bool resume = false;
    if (!background_blocked || pressure.pending || !can_resume || available < watermark)
        resume_tracking = false;
    else if (!resume_tracking)
    {
        resume_started = now;
        resume_tracking = true;
    }
    else if ((uint32_t)(now - resume_started) >= pressure_ticks(PRESSURE_RESUME_MS))
    {
        background_blocked = resume_tracking = false;
        resume = true;
    }
    rt_hw_interrupt_enable(level);
    if (resume) app_catalog_background_resume();
}

#ifdef RT_USING_FINSH
static int memory_pressure(int argc, char **argv)
{
    if (argc < 1 || argc > 2) goto invalid;
    if (argc == 2)
    {
        const char *text = argv[1];
        size_t minimum = 0;
        if (!text || !*text) goto invalid;
        for (; *text; ++text)
        {
            if (*text < '0' || *text > '9') goto invalid;
            unsigned digit = (unsigned)(*text - '0');
            if (minimum > (SIZE_MAX - digit) / 10u) goto invalid;
            minimum = minimum * 10u + digit;
        }
        uint32_t generation = app_memory_pressure_request(minimum);
        rt_kprintf("memory_pressure request=%lu\n", (unsigned long)generation);
    }
    app_memory_pressure_status_t snapshot = app_memory_pressure_query();
    rt_kprintf("memory_pressure gen=%lu pending=%u result=%u need=%lu observed=%lu\n",
               (unsigned long)snapshot.generation, (unsigned)snapshot.pending, (unsigned)snapshot.result,
               (unsigned long)snapshot.min_available, (unsigned long)snapshot.available);
    rt_kprintf("memory_pressure available=%lu blocked=%u capacity=%lu\n",
               (unsigned long)epd_app_psram_available(),
               (unsigned)app_memory_pressure_background_blocked(),
               (unsigned long)epd_app_psram_capacity());
    return RT_EOK;
invalid:
    rt_kprintf("usage: memory_pressure [bytes] (unsigned decimal, no overflow)\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(memory_pressure, Query or request cooperative memory reclamation);
#endif
