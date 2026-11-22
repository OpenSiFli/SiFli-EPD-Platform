/* Deterministic admission boundary for production component tests. The actual
 * helper/core/runtime are exercised separately by queued_run.py. */
#ifndef EPD_ACTIVITY_WORK_H
#define EPD_ACTIVITY_WORK_H
#include "epd_resource.h"
#include <assert.h>
#include <stdlib.h>
size_t epd_app_allocation_peak(size_t size) { return size > SIZE_MAX - 128u ? SIZE_MAX : size + 128u; }
int epd_resource_context_reserve(epd_memory_region_t region, size_t bytes)
{ (void)region; return bytes == SIZE_MAX ? EPD_RESOURCE_INVALID : EPD_RESOURCE_OK; }
size_t epd_resource_file_io_peak(size_t path_bytes) { return 6144u + 2u * path_bytes; }
static epd_memory_owner_t fixture_owner = 1, fixture_next_owner = 100;
epd_memory_owner_t epd_memory_owner_current(void) { return fixture_owner; }
typedef struct epd_activity_work {
    epd_memory_owner_t owner;
    int ready;
    unsigned entered, references;
    bool settled;
    epd_resource_kind_t kind;
    epd_resource_activity_t parent;
    size_t peak, sram;
    void (*wake)(void *);
    void *context;
} epd_activity_work_t;
typedef struct { epd_activity_work_t *work; epd_memory_owner_t previous; } epd_activity_guard_t;
static int activity_admission = 1;
static unsigned activity_queued, activity_closed, activity_tasks, activity_entries, activity_nearby;
epd_activity_work_t *epd_activity_work_queue(epd_resource_kind_t kind,
    epd_resource_activity_t parent, size_t psram, size_t sram, void (*wake)(void *), void *context)
{
    assert(psram);
    if (kind == EPD_RESOURCE_NEARBY && !parent) return NULL;
    epd_activity_work_t *work = calloc(1, sizeof(*work)); assert(work);
    work->owner = ++fixture_next_owner;work->ready = activity_admission; work->kind = kind; work->parent = parent;
    work->references = 1;
    work->peak = psram; work->sram = sram; work->wake = wake; work->context = context;
    ++activity_queued; ++activity_tasks;
    if (kind == EPD_RESOURCE_NEARBY) ++activity_nearby;
    return work;
}
int epd_activity_work_ready(epd_activity_work_t *work) { return work && !work->settled ? work->ready : -1; }
int epd_activity_work_prepare(epd_activity_work_t *work, size_t psram, size_t sram)
{
    int ready = epd_activity_work_ready(work);
    if (ready == 1) { work->peak = psram; work->sram = sram; }
    return ready;
}
bool epd_activity_work_enter(epd_activity_work_t *work, epd_activity_guard_t *guard)
{
    if (epd_activity_work_ready(work) != 1) return false;
    guard->previous=fixture_owner;fixture_owner=work->owner;guard->work = work; ++work->entered; ++activity_entries; return true;
}
void epd_activity_work_leave(epd_activity_guard_t *guard) { assert(guard->work->entered); --guard->work->entered;fixture_owner=guard->previous; }
bool epd_activity_work_reserve(epd_activity_work_t *work, int region, size_t bytes)
{ (void)region; if (!work || work->ready != 1) return false; if (bytes > work->peak) work->peak = bytes; return true; }
epd_resource_activity_t epd_activity_work_activity(const epd_activity_work_t *work)
{ return (epd_resource_activity_t)(uintptr_t)work; }
bool epd_activity_work_settle(epd_activity_work_t *work)
{
    if (!work || work->kind != EPD_RESOURCE_INTERACTIVE) return false;
    assert(!work->entered);
    if (!work->settled) { assert(activity_tasks); --activity_tasks; work->settled = true; }
    return true;
}
void epd_activity_work_close(epd_activity_work_t *work)
{
    if (!work) return;
    if (--work->references) return;
    assert(!work->entered);
    if (!work->settled) { assert(activity_tasks); --activity_tasks; }
    ++activity_closed; free(work);
}
bool epd_activity_work_retain(epd_activity_work_t *work)
{ if (!work) return false; ++work->references; return true; }
#endif
