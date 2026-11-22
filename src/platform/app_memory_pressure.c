#include "app_memory_pressure.h"
#include "epd_memory.h"
#include "epd_resource.h"
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
static uint32_t resource_generation;
static size_t resource_reclaimed_before;
static size_t pressure_contiguous;
extern struct rt_memheap app_image_psram_memheap;

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

static uint32_t pressure_request(size_t min_available, size_t min_contiguous, bool force)
{
    size_t available = epd_app_psram_available();
    if (!min_available || (!force && available >= min_available)) return 0;
    size_t capacity = epd_app_psram_capacity();
    uint32_t now = (uint32_t)rt_tick_get();
    bool notify = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (min_available > capacity || min_contiguous > capacity)
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
        if (min_contiguous > pressure_contiguous) pressure_contiguous = min_contiguous;
        if (force) force_release = forced_round = true;
    }
    else if (!pressure.generation || (force && !forced_round) ||
             pressure.result == APP_MEMORY_PRESSURE_SATISFIED ||
             (uint32_t)(now - pressure_completed) >= pressure_ticks(PRESSURE_RETRY_MS))
    {
        pressure.generation = pressure_generation();
        pressure.pending = true;
        pressure.result = APP_MEMORY_PRESSURE_PENDING;
        pressure.min_available = min_available;
        pressure_contiguous = min_contiguous;
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
    return pressure_request(min_available, 0, false);
}
RTM_EXPORT(app_memory_pressure_request);

uint32_t app_memory_pressure_allocation_failed(size_t min_available)
{
    return pressure_request(min_available, 0, true);
}
RTM_EXPORT(app_memory_pressure_allocation_failed);

/* Exact free-block span for the application PSRAM allocator. Credit admission
 * separately reserves epd_app_allocation_peak(), including unsplit slack. */
size_t app_memory_pressure_allocation_span(size_t payload)
{
    const size_t header = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    if (!payload || payload > SIZE_MAX - header - 31u) return 0;
    size_t rounded;
#if defined(SOLUTION)
    if (!rt_memcmp(app_image_psram_memheap.parent.name, "psram_memheap", sizeof("psram_memheap")) ||
        !rt_memcmp(app_image_psram_memheap.parent.name, "heap", sizeof("heap")))
        rounded = RT_ALIGN(payload + sizeof(struct rt_memheap_item), 32u) - sizeof(struct rt_memheap_item);
    else
#endif
        rounded = RT_ALIGN(payload, RT_ALIGN_SIZE);
    if (rounded < 12u) rounded = 12u;
    return header + rounded;
}
RTM_EXPORT(app_memory_pressure_allocation_span);

uint32_t app_memory_pressure_allocation_failed_block(size_t min_available, size_t min_contiguous)
{
    return pressure_request(min_available, min_contiguous, true);
}
RTM_EXPORT(app_memory_pressure_allocation_failed_block);

/* Only allocation-failure recovery needs this walk. Never wait for the heap or
 * hold the IRQ guard while looking for a fitting physical span. */
static bool pressure_block_fits(size_t minimum, bool *known)
{
    *known = false;
    if (rt_sem_take(&app_image_psram_memheap.lock, 0) != RT_EOK) return false;
    const struct rt_memheap_item *sentinel = app_image_psram_memheap.free_list;
    bool fits = false;
    for (const struct rt_memheap_item *item = sentinel->next_free;
         item != sentinel; item = item->next_free) {
        size_t span = (size_t)((const unsigned char *)item->next - (const unsigned char *)item);
        if (span >= minimum) { fits = true; break; }
    }
    rt_sem_release(&app_image_psram_memheap.lock);
    *known = true;
    return fits;
}

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
    epd_memory_backend_t backend;
    epd_resource_reclaim_snapshot_t resources = {0};
    bool resource_backend = epd_memory_backend_find(&app_image_psram_memheap, &backend);
    if (resource_backend) epd_resource_reclaim_status(backend, &resources);
    rt_base_t level = rt_hw_interrupt_disable();
    size_t checked_contiguous = pressure.pending ? pressure_contiguous : 0;
    uint32_t checked_generation = pressure.generation;
    rt_hw_interrupt_enable(level);
    bool block_known = true;
    bool block_fits = !checked_contiguous || pressure_block_fits(checked_contiguous, &block_known);
    level = rt_hw_interrupt_disable();
    pressure.available = available;
    if (resource_generation != pressure.generation) {
        resource_generation = pressure.generation; resource_reclaimed_before = resources.reclaimed_bytes;
    }
    if (resources.reclaimed_bytes != resource_reclaimed_before) {
        resource_reclaimed_before = resources.reclaimed_bytes;
        force_release = false;
    }
    /* A stop request is not a release: deferred cleanup retires module pins. */
    if (force_stopping && !stopping) force_release = force_stopping = false;
    bool block_checked = checked_generation == pressure.generation &&
                         checked_contiguous >= pressure_contiguous && block_known;
    if (pressure_contiguous && block_checked && block_fits) force_release = false;
    if (pressure.pending && !force_release && available >= pressure.min_available &&
        (!pressure_contiguous || (block_checked && block_fits)))
        pressure_finish(APP_MEMORY_PRESSURE_SATISFIED, now);
    if (pressure.pending && (uint32_t)(now - pressure_started) >= pressure_ticks(PRESSURE_TIMEOUT_MS))
        pressure_finish(APP_MEMORY_PRESSURE_TIMED_OUT, now);
    bool pending = pressure.pending;
    bool waiting_release = force_release && force_stopping;
    uint32_t generation = pressure.generation;
    size_t minimum = pressure.min_available;
    rt_hw_interrupt_enable(level);
    if (pending)
    {
        if (pressure_contiguous && !block_checked) { pressure_report(); return; }
        /* Selection only queues legal-lane destruction. Re-read the allocator on
         * the following pass before treating this as released capacity. */
        if (resource_backend && resources.pending_objects) { pressure_report(); return; }
        if (resource_backend && !waiting_release) {
            size_t wanted = minimum > available ? minimum - available : 1;
            /* Fragmentation needs a useful batch even when total free space
             * is sufficient. Each callback stays bounded; check the actual
             * contiguous span again before selecting another provider. */
            if (checked_contiguous && !block_fits && wanted < checked_contiguous)
                wanted = checked_contiguous;
            if (epd_resource_reclaim(backend, wanted, 1)) { pressure_report(); return; }
        }
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
