#include "epd_resource.h"
#include "epd_resource_feedback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned locks, retained[16], loads, destroys, cancels, notifications, notices;
static epd_memory_owner_t current = 1;
static epd_resource_lane_t lane;
static uint64_t clock_now = 100, ticket_next;
static bool tickets[2048];
static unsigned tickets_live, apply_calls, admit_calls;
static int apply_result, admit_result, load_result = EPD_RESOURCE_PENDING, destroy_result;
static bool immediate_load, reenter_end, reenter_update, notify_release;
static epd_resource_activity_t reenter_activity;
static epd_resource_activity_spec_t reenter_spec;
static epd_resource_t last_loaded, destroyed_ids[256];
static epd_resource_binding_t binding;
static epd_resource_provider_t provider;
static int notification_result;

static void state_lock(void *ctx) { (void)ctx; CHECK(!locks); ++locks; }
static void state_unlock(void *ctx) { (void)ctx; CHECK(locks == 1); --locks; }
static void unlocked(void) { CHECK(!locks); }
static epd_memory_owner_t owner_current(void *ctx) { (void)ctx; unlocked(); return current; }
static int owner_retain(void *ctx, epd_memory_owner_t owner)
{ (void)ctx; unlocked(); CHECK(owner < 16); ++retained[owner]; return 0; }
static void owner_release(void *ctx, epd_memory_owner_t owner)
{ (void)ctx; unlocked(); CHECK(owner < 16 && retained[owner]); --retained[owner]; }
static epd_memory_owner_t owner_enter(void *ctx, epd_memory_owner_t owner)
{ epd_memory_owner_t old = current; (void)ctx; unlocked(); CHECK(retained[owner]); current = owner; return old; }
static void owner_leave(void *ctx, epd_memory_owner_t old) { (void)ctx; unlocked(); current = old; }
static uint64_t now(void *ctx) { (void)ctx; unlocked(); return clock_now; }
static void schedule(void *ctx, epd_resource_lane_t requested) { (void)ctx; (void)requested; unlocked(); }
static uint64_t ticket(void) { CHECK(++ticket_next < 2048); tickets[ticket_next] = true; ++tickets_live; return ticket_next; }
static void credit_release(void *ctx, epd_resource_credit_t id)
{ (void)ctx; unlocked(); CHECK(id < 2048 && tickets[id]); tickets[id] = false; --tickets_live; }
static int apply(void *ctx, const epd_resource_activity_snapshot_t *request, epd_resource_grant_t *out)
{
    (void)ctx; unlocked(); CHECK(lane == EPD_RESOURCE_WORKER); ++apply_calls;
    if (reenter_end) { reenter_end = false; CHECK(epd_resource_activity_end(request->activity) == EPD_RESOURCE_ACCEPTED); }
    if (reenter_update) {
        uint64_t rev;
        reenter_update = false;
        CHECK(epd_resource_activity_update(request->activity, &reenter_spec, &rev) == EPD_RESOURCE_ACCEPTED);
    }
    if (apply_result) return apply_result;
    out->credit = ticket(); out->guaranteed_bytes = request->requested.target_bytes;
    out->unused_bytes = out->guaranteed_bytes > request->associated_bytes ? out->guaranteed_bytes - request->associated_bytes : 0;
    return 0;
}
static int admit(void *ctx, epd_resource_t resource, const epd_resource_descriptor_t *desc,
                 const epd_resource_credit_t *credits, size_t count, epd_resource_credit_t *out)
{
    size_t i;
    (void)ctx; (void)resource; (void)desc; unlocked(); ++admit_calls;
    CHECK(count);
    for (i = 0; i < count; ++i) CHECK(tickets[credits[i]]);
    if (reenter_activity) {
        CHECK(epd_resource_activity_end(reenter_activity) == EPD_RESOURCE_ACCEPTED);
        for (i = 0; i < count; ++i) CHECK(tickets[credits[i]]);
        reenter_activity = 0;
    }
    if (admit_result) return admit_result;
    *out = ticket(); return 0;
}
static int load(void *ctx, epd_resource_t resource, const epd_resource_descriptor_t *desc, epd_resource_credit_t credit)
{
    void *data;
    (void)ctx; unlocked(); CHECK(lane == EPD_RESOURCE_WORKER); CHECK(current == 2); CHECK(tickets[credit]);
    ++loads; last_loaded = resource;
    if (immediate_load) {
        data = malloc(desc->estimated_bytes); CHECK(data);
        memset(data, 0x5a, desc->estimated_bytes);
        CHECK(epd_resource_load_complete(resource, 0, data, desc->estimated_bytes) == 0);
        return 0;
    }
    return load_result;
}
static void cancel(void *ctx, epd_resource_t resource)
{ (void)ctx; (void)resource; unlocked(); CHECK(lane == EPD_RESOURCE_WORKER && current == 2); ++cancels; }
static int destroy(void *ctx, epd_resource_t resource, void *data, size_t bytes)
{
    (void)ctx; (void)bytes; unlocked(); CHECK(lane == EPD_RESOURCE_UI && current == 2);
    CHECK(destroys < 256); destroyed_ids[destroys++] = resource;
    if (!destroy_result) free(data);
    return destroy_result;
}
static void notification(void *ctx, epd_resource_ref_t ref, int result)
{
    (void)ctx; unlocked(); CHECK(lane == EPD_RESOURCE_UI && current == 1);
    ++notifications; notification_result = result;
    if (notify_release) CHECK(epd_resource_release(ref) == EPD_RESOURCE_PENDING);
}
static void notice(void *ctx, const epd_resource_activity_snapshot_t *s)
{ (void)ctx; (void)s; unlocked(); CHECK(lane == EPD_RESOURCE_UI && current == 1); ++notices; }
static size_t pump(epd_resource_lane_t which, size_t count)
{ lane = which; return epd_resource_process(which, count); }
static void drain(void)
{
    unsigned i;
    for (i = 0; i < 100; ++i) {
        size_t work = pump(EPD_RESOURCE_WORKER, 100);
        work += pump(EPD_RESOURCE_UI, 100);
        if (!work) return;
    }
    CHECK(!"pump did not quiesce");
}
static unsigned control_calls;
static void *allocate_control(void *context, size_t bytes)
{
    (void)context; unlocked(); CHECK(bytes && bytes < 8192);
    if (!control_calls++) return NULL;
    CHECK(control_calls == 2);
    return calloc(1, bytes);
}
static void setup(void)
{
    epd_resource_runtime_t rt = {0};
    epd_resource_provider_spec_t p = {0};
    rt.lock = state_lock; rt.unlock = state_unlock; rt.current_owner = owner_current;
    rt.owner_retain = owner_retain; rt.owner_release = owner_release;
    rt.owner_enter = owner_enter; rt.owner_leave = owner_leave; rt.now = now; rt.wake = schedule;
    rt.activity_apply = apply; rt.resource_admit = admit; rt.credit_release = credit_release;
    rt.control_allocate = allocate_control;
    CHECK(epd_resource_init(&rt) == EPD_RESOURCE_CAPACITY);
    CHECK(epd_resource_init(&rt) == 0);
    CHECK(epd_resource_binding_open(1, 42, 99, 15, &binding) == 0);
    p.code_owner = 2; p.implementation_version = 7; p.load_lane = EPD_RESOURCE_WORKER;
    p.destroy_lane = EPD_RESOURCE_UI; p.load = load; p.cancel = cancel; p.destroy = destroy;
    CHECK(epd_resource_provider_register(&p, &provider) == 0);
}
static epd_resource_activity_spec_t spec_for(epd_resource_kind_t kind)
{
    epd_resource_activity_spec_t s = {0};
    s.binding = binding; s.kind = kind; s.backend = 1; s.minimum_bytes = 64; s.target_bytes = 128;
    return s;
}
static epd_resource_activity_t begin(epd_resource_kind_t kind)
{
    epd_resource_activity_spec_t s = spec_for(kind);
    epd_resource_observer_t o = {notice, NULL, EPD_RESOURCE_UI};
    epd_resource_activity_t a;
    CHECK(epd_resource_activity_begin(&s, &o, &a) == EPD_RESOURCE_ACCEPTED); return a;
}
static epd_resource_descriptor_t descriptor(unsigned key, unsigned backend)
{
    epd_resource_descriptor_t d = {0};
    d.key.bytes[0] = (uint8_t)key; d.key.length = 1; d.key.version = 1;
    d.backend = backend; d.estimated_bytes = 64; d.discardable = true;
    return d;
}
static epd_resource_ref_t acquire(epd_resource_activity_t a, unsigned key)
{
    epd_resource_descriptor_t d = descriptor(key, 1);
    epd_resource_ref_t ref;
    CHECK(epd_resource_acquire(a, provider, &d, notification, NULL, EPD_RESOURCE_UI, &ref) == EPD_RESOURCE_ACCEPTED);
    return ref;
}
static epd_resource_t resource(epd_resource_ref_t ref)
{ epd_resource_t id; CHECK(epd_resource_ref_get(ref, &id) == 0); return id; }
static void complete(epd_resource_t id)
{ void *data = malloc(64); CHECK(data); CHECK(epd_resource_load_complete(id, 0, data, 64) == 0); }
static void finish(epd_resource_activity_t a)
{
    epd_resource_activity_snapshot_t s;
    CHECK(epd_resource_activity_end(a) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_ENDED);
    CHECK(epd_resource_activity_forget(a) == 0);
}
static void clean(void)
{
    int result;
    epd_resource_stats_t s;
    result = epd_resource_provider_unregister(provider);
    CHECK(result == 0 || result == EPD_RESOURCE_PENDING); drain();
    if (result == EPD_RESOURCE_PENDING) CHECK(epd_resource_provider_unregister(provider) == 0);
    result = epd_resource_binding_end(binding); CHECK(result == 0 || result == EPD_RESOURCE_STALE);
    drain(); epd_resource_stats(&s);
    CHECK(!s.activities && !s.providers && !s.resources && !s.references && !s.physical_bytes);
    CHECK(!retained[1] && !retained[2] && !tickets_live && !locks);
}
static void accepted_and_failed_increase(void)
{
    epd_resource_activity_t a; epd_resource_activity_snapshot_t old, s;
    epd_resource_activity_spec_t update; uint64_t revision;
    setup(); a = begin(EPD_RESOURCE_CONTINUOUS);
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_WAITING && !s.credit && !apply_calls);
    drain(); CHECK(epd_resource_activity_get(a, &old) == 0 && old.effective_revision == 1);
    update = spec_for(EPD_RESOURCE_CONTINUOUS); update.minimum_bytes = update.target_bytes = 1000;
    apply_result = EPD_RESOURCE_DENIED;
    CHECK(epd_resource_activity_update(a, &update, &revision) == EPD_RESOURCE_ACCEPTED && revision == 2);
    drain(); CHECK(epd_resource_activity_get(a, &s) == 0);
    CHECK(s.state == EPD_RESOURCE_REJECTED && s.credit == old.credit && s.effective_revision == 1 && s.guaranteed_bytes == old.guaranteed_bytes);
    finish(a); clean();
}
static void shared_pins_and_ending(void)
{
    epd_resource_activity_t a, b; epd_resource_ref_t x, y; epd_resource_t r;
    epd_resource_stats_t stats; epd_resource_activity_snapshot_t s; void *px, *py;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); b = begin(EPD_RESOURCE_INTERACTIVE);
    x = acquire(a, 1); y = acquire(b, 1); r = resource(x); CHECK(r == resource(y));
    drain(); CHECK(loads == 1 && admit_calls == 1); complete(r); drain();
    CHECK(notifications == 2); CHECK(epd_resource_pin(x, &px) == 0 && epd_resource_pin(y, &py) == 0 && px == py);
    epd_resource_stats(&stats); CHECK(stats.physical_bytes == 64);
    CHECK(epd_resource_activity_end(a) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_ENDING);
    CHECK(!s.credit && !s.unused_bytes && s.references);
    CHECK(epd_resource_reclaim(1, 64, 1) == 0);
    CHECK(epd_resource_unpin(x) == 0); drain(); finish(a);
    CHECK(epd_resource_unpin(y) == 0); finish(b); clean();
}
static void cancel_one_waiter(void)
{
    epd_resource_activity_t a, b; epd_resource_ref_t x, y; epd_resource_t r;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); b = begin(EPD_RESOURCE_INTERACTIVE);
    x = acquire(a, 3); y = acquire(b, 3); r = resource(x); drain();
    CHECK(epd_resource_release(x) == 0); finish(a); CHECK(!cancels);
    complete(r); drain(); CHECK(notifications == 1 && resource(y) == r && loads == 1);
    finish(b); clean();
}
static void single_failure(void)
{
    epd_resource_activity_t a, b; epd_resource_ref_t x, y; void *data;
    setup(); load_result = EPD_RESOURCE_FAILED;
    a = begin(EPD_RESOURCE_OPTIONAL); b = begin(EPD_RESOURCE_OPTIONAL);
    x = acquire(a, 4); y = acquire(b, 4); drain();
    CHECK(loads == 1 && notifications == 2 && notification_result == EPD_RESOURCE_FAILED);
    CHECK(epd_resource_pin(x, &data) == EPD_RESOURCE_FAILED);
    CHECK(epd_resource_pin(y, &data) == EPD_RESOURCE_FAILED);
    epd_resource_retry(); drain(); CHECK(loads == 1 && notifications == 2);
    finish(a); finish(b); clean();
}
static void provider_drain(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref, extra; epd_resource_t r;
    epd_resource_descriptor_t d = descriptor(5, 1); void *data;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_INTERACTIVE); ref = acquire(a, 5); drain(); r = resource(ref);
    CHECK(epd_resource_pin(ref, &data) == 0);
    CHECK(epd_resource_provider_unregister(provider) == EPD_RESOURCE_PENDING); drain();
    CHECK(retained[2] && !destroys);
    CHECK(epd_resource_acquire(a, provider, &d, NULL, NULL, EPD_RESOURCE_UI, &extra) == EPD_RESOURCE_STOPPED);
    CHECK(epd_resource_unpin(ref) == 0); drain(); CHECK(!destroys);
    CHECK(epd_resource_get(r, &(epd_resource_snapshot_t){0}) == 0);
    finish(a); CHECK(destroys == 1); clean();
}
static void dirty_revisions(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref; epd_resource_t r;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_OPTIONAL); ref = acquire(a, 6); drain(); r = resource(ref);
    CHECK(epd_resource_mark_dirty(r, 1) == 0); CHECK(epd_resource_mark_dirty(r, 2) == 0);
    finish(a); CHECK(epd_resource_reclaim(1, 64, 1) == 0);
    CHECK(epd_resource_provider_unregister(provider) == EPD_RESOURCE_PENDING); drain(); CHECK(!destroys);
    CHECK(epd_resource_saved(r, 1, 0) == 0); drain(); CHECK(!destroys);
    CHECK(epd_resource_saved(r, 2, EPD_RESOURCE_FAILED) == 0); drain(); CHECK(!destroys && retained[2]);
    CHECK(epd_resource_saved(r, 2, 0) == 0); drain(); CHECK(destroys == 1); clean();
}
static void invalidation_and_late_completion(void)
{
    epd_resource_activity_t a; epd_resource_ref_t x, y; epd_resource_t old, fresh; void *data;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); x = acquire(a, 7); old = resource(x); drain();
    CHECK(epd_resource_invalidate(old) == 0); y = acquire(a, 7); fresh = resource(y); CHECK(old != fresh);
    drain(); CHECK(loads == 2 && cancels == 1);
    complete(old); complete(fresh); drain(); CHECK(notifications == 2);
    CHECK(epd_resource_pin(x, &data) == EPD_RESOURCE_CANCELLED);
    CHECK(epd_resource_load_complete(old, EPD_RESOURCE_FAILED, NULL, 0) == EPD_RESOURCE_STALE);
    finish(a); clean();
}
static void incremental_reclaim(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref; epd_resource_t ids[12];
    epd_resource_descriptor_t d; epd_resource_stats_t s; unsigned i;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_OPTIONAL);
    for (i = 0; i < 12; ++i) { ref = acquire(a, i + 20); ids[i] = resource(ref); drain(); CHECK(epd_resource_release(ref) == 0); }
    d = descriptor(1, 1); (void)d;
    CHECK(epd_resource_set_local_value(ids[0], 100) == 0);
    CHECK(epd_resource_reclaim(7, 64, 1) == 0);
    CHECK(epd_resource_reclaim(1, 64, 1) == 64); drain();
    CHECK(destroys == 1 && destroyed_ids[0] == ids[1]);
    epd_resource_stats(&s); CHECK(s.candidate_visits == 1 && s.reclaimed_bytes == 64 && s.physical_bytes == 11 * 64);
    finish(a); clean();
}
static void cancellation_during_admission(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); ref = acquire(a, 50); (void)ref;
    reenter_activity = a; drain(); CHECK(!loads); finish(a); clean();
}
static void stale_revision_completion(void)
{
    epd_resource_activity_t a; epd_resource_activity_snapshot_t s;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); reenter_spec = spec_for(EPD_RESOURCE_INTERACTIVE);
    reenter_update = true; drain(); CHECK(apply_calls == 2 && tickets_live == 1);
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.effective_revision == 2 && s.effective.kind == EPD_RESOURCE_INTERACTIVE);
    finish(a); clean();
}
static void end_during_apply(void)
{
    epd_resource_activity_t a;
    setup(); a = begin(EPD_RESOURCE_OPTIONAL); reenter_end = true; drain(); CHECK(!tickets_live); finish(a); clean();
}
static void destructor_failure(void)
{
    epd_resource_activity_t a; epd_resource_stats_t s;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_OPTIONAL); acquire(a, 60); drain(); finish(a);
    destroy_result = EPD_RESOURCE_FAILED; CHECK(epd_resource_reclaim(1, 64, 1) == 64); drain();
    epd_resource_stats(&s); CHECK(s.physical_bytes == 64 && !s.reclaimed_bytes && destroys == 1);
    drain(); CHECK(destroys == 1); destroy_result = 0; epd_resource_retry(); drain();
    CHECK(destroys == 2); clean();
}
static void runtime_binding_authority(void)
{
    epd_resource_activity_t a, b; epd_resource_activity_spec_t s; epd_resource_activity_snapshot_t snapshot;
    epd_resource_ref_t ref; void *data;
    setup(); s = spec_for(EPD_RESOURCE_INTERACTIVE); current = 3;
    CHECK(epd_resource_activity_begin(&s, NULL, &a) == EPD_RESOURCE_DENIED); current = 1;
    a = begin(EPD_RESOURCE_INTERACTIVE); s = spec_for(EPD_RESOURCE_NEARBY); s.interactive_parent = a;
    CHECK(epd_resource_activity_begin(&s, NULL, &b) == EPD_RESOURCE_ACCEPTED);
    immediate_load = true; ref = acquire(a, 66); drain(); CHECK(epd_resource_pin(ref, &data) == 0);
    CHECK(epd_resource_binding_restrict(binding, 1) == 0); drain();
    CHECK(epd_resource_activity_get(a, &snapshot) == 0 && snapshot.effective.kind == EPD_RESOURCE_OPTIONAL);
    CHECK(epd_resource_reclaim(1, 64, 1) == 0); CHECK(epd_resource_unpin(ref) == 0);
    finish(a); finish(b); clean();
}
static void notify_reentrancy(void)
{
    epd_resource_activity_t a;
    setup(); immediate_load = true; notify_release = true; a = begin(EPD_RESOURCE_OPTIONAL);
    acquire(a, 70); drain(); CHECK(notifications == 1); finish(a); clean();
}
static void waiting_deadline_and_event_retry(void)
{
    epd_resource_activity_t a; epd_resource_activity_snapshot_t s;
    epd_resource_activity_spec_t spec;
    setup(); apply_result = EPD_RESOURCE_PENDING; a = begin(EPD_RESOURCE_OPTIONAL); drain();
    CHECK(apply_calls == 1); drain(); CHECK(apply_calls == 1);
    apply_result = 0; epd_resource_retry(); drain(); CHECK(apply_calls == 2); finish(a);
    spec = spec_for(EPD_RESOURCE_OPTIONAL); spec.deadline = 99;
    CHECK(epd_resource_activity_begin(&spec, NULL, &a) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(epd_resource_activity_get(a, &s) == 0 && s.state == EPD_RESOURCE_REJECTED && apply_calls == 2);
    finish(a); clean();
}
static void close_owner_keeps_shared_provider(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref; epd_resource_stats_t s; void *data;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_INTERACTIVE);
    ref = acquire(a, 80); drain(); CHECK(epd_resource_pin(ref, &data) == 0);
    CHECK(epd_resource_close_owner(1) == EPD_RESOURCE_ACCEPTED);
    CHECK(epd_resource_owner_busy(1) && !destroys);
    drain(); CHECK(epd_resource_owner_busy(1) && retained[1]);
    CHECK(epd_resource_unpin(ref) == 0); drain();
    CHECK(!epd_resource_owner_busy(1) && !retained[1] && retained[2]);
    epd_resource_stats(&s); CHECK(!s.activities && s.providers == 1 && s.resources == 1);
    clean();
}
static void close_provider_owner(void)
{
    epd_resource_activity_t a; epd_resource_ref_t ref; void *data;
    setup(); immediate_load = true; a = begin(EPD_RESOURCE_INTERACTIVE);
    ref = acquire(a, 81); drain(); CHECK(epd_resource_pin(ref, &data) == 0);
    CHECK(epd_resource_close_owner(2) == EPD_RESOURCE_ACCEPTED);
    CHECK(epd_resource_owner_busy(2) && !destroys); drain(); CHECK(retained[2] && !destroys);
    CHECK(epd_resource_unpin(ref) == 0); finish(a); drain();
    CHECK(!epd_resource_owner_busy(2) && !retained[2] && destroys == 1);
    CHECK(epd_resource_binding_end(binding) == 0);
    CHECK(!retained[1] && !tickets_live);
}
static epd_resource_provider_t bank;
static size_t bank_total, bank_free;
static unsigned bank_calls;
static int bank_shrink(void *ctx, uint32_t backend, size_t bytes, size_t *released)
{
    (void)ctx; unlocked(); CHECK(lane == EPD_RESOURCE_UI && current == 2 && backend == 1);
    ++bank_calls; *released = bytes < bank_free ? bytes : bank_free;
    if (*released > 32) *released = 32;
    bank_total -= *released; bank_free -= *released;
    CHECK(epd_resource_provider_cache_update(bank, 1, bank_total, bank_free, 1, 0) == 0);
    return 0;
}
static void cache_bank_partial_and_drain(void)
{
    epd_resource_provider_spec_t spec = {0}; epd_resource_reclaim_snapshot_t reclaim;
    epd_resource_stats_t stats;
    setup(); spec.code_owner = 2; spec.implementation_version = 1;
    spec.destroy_lane = EPD_RESOURCE_UI; spec.shrink = bank_shrink;
    CHECK(epd_resource_provider_register(&spec, &bank) == 0);
    bank_total = 128; bank_free = 64;
    CHECK(epd_resource_provider_cache_update(bank, 1, bank_total, bank_free, 1, 0) == 0);
    CHECK(epd_resource_reclaim(1, 32, 1) == 32); CHECK(!bank_calls);
    drain(); epd_resource_reclaim_status(1, &reclaim);
    CHECK(bank_calls == 1 && bank_total == 96 && reclaim.reclaimed_bytes == 32 && !reclaim.pending_objects);
    epd_resource_stats(&stats); CHECK(stats.physical_bytes == 96);
    CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING); drain();
    CHECK(bank_total == 64 && bank_free == 0 && retained[2] == 2);
    CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING); drain();
    CHECK(bank_calls == 2); /* In-use bytes neither freed nor polled. */
    bank_free = bank_total;
    CHECK(epd_resource_provider_cache_update(bank, 1, bank_total, bank_free, 1, 0) == 0);
    drain(); CHECK(!bank_total && bank_calls == 4);
    CHECK(epd_resource_provider_unregister(bank) == 0); clean();
}
static void cache_bank_offer_withdrawn(unsigned mode)
{
    epd_resource_provider_spec_t spec = {0}; epd_resource_reclaim_snapshot_t status;
    epd_resource_stats_t stats;
    setup(); spec.code_owner = 2; spec.implementation_version = 1;
    spec.destroy_lane = EPD_RESOURCE_UI; spec.shrink = bank_shrink;
    CHECK(epd_resource_provider_register(&spec, &bank) == 0);
    bank_total = bank_free = 64;
    CHECK(epd_resource_provider_cache_update(bank, 1, 64, 64, 1, 0) == 0);
    CHECK(epd_resource_reclaim(1, 32, 1) == 32);
    bank_free = 0;
    if (mode == 1) bank_total = 0;
    CHECK(epd_resource_provider_cache_update(bank, 1, bank_total, 0, 1, 0) == 0);
    if (mode == 2) CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING);
    drain(); epd_resource_reclaim_status(1, &status);
    CHECK(!status.pending_objects && !status.pending_bytes && !status.reclaimed_bytes);
    CHECK(!status.failed_objects && !bank_calls);
    epd_resource_stats(&stats); CHECK(stats.physical_bytes == bank_total);
    drain(); CHECK(!bank_calls); /* No polling of the held bytes. */
    if (mode == 2) {
        CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING);
        CHECK(retained[2] == 2);
    }
    bank_total = bank_free = 64;
    CHECK(epd_resource_provider_cache_update(bank, 1, 64, 64, 1, 0) == 0);
    if (mode != 2) CHECK(epd_resource_reclaim(1, 32, 1) == 32);
    drain(); CHECK(bank_calls && bank_total < 64);
    int result = epd_resource_provider_unregister(bank); drain();
    CHECK(result == 0 || result == EPD_RESOURCE_PENDING);
    if (result == EPD_RESOURCE_PENDING) CHECK(epd_resource_provider_unregister(bank) == 0);
    epd_resource_reclaim_status(1, &status);
    CHECK(!status.pending_objects && !status.pending_bytes && !bank_total);
    clean();
}
static void cache_bank_repin_queued(void) { cache_bank_offer_withdrawn(0); }
static void cache_bank_empty_queued(void) { cache_bank_offer_withdrawn(1); }
static void cache_bank_stop_held_queued(void) { cache_bank_offer_withdrawn(2); }
static bool bank_complete_inline;
static int async_bank_shrink(void *ctx, uint32_t backend, size_t bytes, size_t *released)
{
    (void)ctx; (void)bytes; unlocked(); ++bank_calls; *released = 0;
    if (bank_complete_inline) {
        CHECK(epd_resource_provider_cache_update(bank, backend, 0, 0, 1, 0) == 0);
        CHECK(epd_resource_provider_cache_complete(bank, backend, 64, EPD_RESOURCE_OK) == 0);
    }
    return EPD_RESOURCE_PENDING;
}
static void cache_bank_async_common(bool early)
{
    epd_resource_provider_spec_t spec = {0}; epd_resource_reclaim_snapshot_t status;
    setup(); bank_complete_inline = early; spec.code_owner = 2; spec.implementation_version = 1;
    spec.destroy_lane = EPD_RESOURCE_UI; spec.shrink = async_bank_shrink;
    CHECK(epd_resource_provider_register(&spec, &bank) == 0);
    CHECK(epd_resource_provider_cache_update(bank, 1, 64, 64, 1, 0) == 0);
    CHECK(epd_resource_reclaim(1, 64, 1) == 64); drain();
    CHECK(bank_calls == 1); epd_resource_reclaim_status(1, &status);
    if (!early) {
        CHECK(status.pending_objects == 1 && status.pending_bytes == 64 && !status.reclaimed_bytes);
        CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING); drain(); CHECK(bank_calls == 1);
        CHECK(epd_resource_provider_cache_update(bank, 1, 0, 0, 1, 0) == 0);
        drain(); epd_resource_reclaim_status(1, &status);
        CHECK(status.pending_objects == 1 && status.pending_bytes == 64 && bank_calls == 1);
        CHECK(epd_resource_provider_cache_complete(bank, 1, 64, EPD_RESOURCE_OK) == 0); drain();
    }
    epd_resource_reclaim_status(1, &status);
    CHECK(!status.pending_objects && status.reclaimed_bytes == 64);
    CHECK(epd_resource_provider_cache_complete(bank, 1, 64, EPD_RESOURCE_OK) == EPD_RESOURCE_STALE);
    int rc = epd_resource_provider_unregister(bank); drain();
    if (rc == EPD_RESOURCE_PENDING) CHECK(epd_resource_provider_unregister(bank) == 0);
    else CHECK(rc == 0);
    clean();
}
static void cache_bank_async(void) { cache_bank_async_common(false); }
static void cache_bank_inline_completion(void) { cache_bank_async_common(true); }
static void feedback_history(void)
{
    epd_resource_feedback_t h = {0};
    epd_resource_feedback_reclaimed(&h, 3, 4, 10, 12);
    CHECK(!epd_resource_feedback_weight(&h, 3, 4, 11));
    epd_resource_feedback_rebuilt(&h, 3, 4, 12);
    CHECK(h.rebuilds == 1 && h.rebuild_cost == 12);
    CHECK(epd_resource_feedback_weight(&h, 3, 4, 12) == 13);
    epd_resource_feedback_rebuilt(&h, 3, 4, 13);
    CHECK(h.rebuilds == 1); /* A cache hit is not another reconstruction. */
    CHECK(!epd_resource_feedback_weight(&h, 4, 4, 13));
    CHECK(!epd_resource_feedback_weight(&h, 3, 5, 13));
    CHECK(!epd_resource_feedback_weight(&h, 3, 4, 30013));
    for (unsigned i = 0; i < 10000; ++i) {
        epd_resource_feedback_reclaimed(&h, i + 1, i, i + 40000, UINT32_MAX);
        epd_resource_feedback_rebuilt(&h, i + 1, i, i + 40001);
        CHECK(epd_resource_feedback_weight(&h, i + 1, i, i + 40001) <= 4u * 65536u);
    }
    _Static_assert(sizeof(h.records) / sizeof(h.records[0]) == 32, "bounded feedback history");
}
static void feedback_selection_common(bool expire)
{
    setup(); immediate_load = true;
    epd_resource_activity_t a = begin(EPD_RESOURCE_OPTIONAL);
    epd_resource_ref_t ref = acquire(a, 1); drain();
    CHECK(epd_resource_release(ref) == 0);
    CHECK(epd_resource_reclaim(1, 64, 1) == 64); drain();
    ref = acquire(a, 1); drain(); epd_resource_t rebuilt = resource(ref);
    CHECK(epd_resource_release(ref) == 0);
    ref = acquire(a, 2); drain(); epd_resource_t fresh = resource(ref);
    CHECK(epd_resource_release(ref) == 0);
    if (expire) clock_now += EPD_RESOURCE_FEEDBACK_WINDOW_MS + 1u;
    CHECK(epd_resource_reclaim(1, 64, 1) == 64); drain();
    CHECK(destroyed_ids[1] == (expire ? rebuilt : fresh));
    finish(a); clean();
}
static void feedback_selection(void) { feedback_selection_common(false); }
static void feedback_expiry(void) { feedback_selection_common(true); }
static void feedback_class_protection(void)
{
    setup();immediate_load=true;
    epd_resource_activity_t optional=begin(EPD_RESOURCE_OPTIONAL);
    epd_resource_ref_t ref=acquire(optional,1);drain();CHECK(epd_resource_release(ref)==0);
    CHECK(epd_resource_reclaim(1,64,1)==64);drain();
    ref=acquire(optional,1);drain();epd_resource_t rebuilt=resource(ref);
    CHECK(epd_resource_release(ref)==0);
    epd_resource_activity_t foreground=begin(EPD_RESOURCE_INTERACTIVE);
    ref=acquire(foreground,2);drain();void *pixels;
    CHECK(epd_resource_pin(ref,&pixels)==0 && pixels);
    CHECK(epd_resource_reclaim(1,128,2)==64);drain();
    CHECK(destroyed_ids[1]==rebuilt);
    CHECK(epd_resource_unpin(ref)==0);finish(foreground);finish(optional);clean();
}
static void feedback_bank(void)
{
    setup(); epd_resource_provider_spec_t spec = {0};
    spec.code_owner = 2; spec.implementation_version = 1;
    spec.destroy_lane = EPD_RESOURCE_UI; spec.shrink = bank_shrink;
    CHECK(epd_resource_provider_register(&spec, &bank) == 0);
    bank_total = bank_free = 64;
    CHECK(epd_resource_provider_cache_update(bank, 1, 64, 64, 1, 0) == 0);
    CHECK(epd_resource_reclaim(1, 32, 1) == 32); drain();
    bank_total = bank_free = 64;
    CHECK(epd_resource_provider_cache_update(bank, 1, 64, 64, 1, 0) == 0);
    immediate_load = true; epd_resource_activity_t a = begin(EPD_RESOURCE_OPTIONAL);
    epd_resource_ref_t ref = acquire(a, 3); drain(); epd_resource_t other = resource(ref);
    CHECK(epd_resource_release(ref) == 0);
    CHECK(epd_resource_reclaim(1, 32, 1) == 64); drain();
    CHECK(bank_calls == 1 && destroys == 1 && destroyed_ids[0] == other);
    CHECK(epd_resource_provider_unregister(bank) == EPD_RESOURCE_PENDING); drain();
    CHECK(epd_resource_provider_unregister(bank) == 0);
    finish(a); clean();
}
static const struct { const char *name; void (*run)(void); } cases[] = {
    {"accepted_failed_increase", accepted_and_failed_increase},
    {"shared_pins_ending", shared_pins_and_ending},
    {"cancel_one_waiter", cancel_one_waiter}, {"single_failure", single_failure},
    {"provider_drain", provider_drain}, {"dirty_revisions", dirty_revisions},
    {"invalidation_late_completion", invalidation_and_late_completion},
    {"incremental_reclaim", incremental_reclaim},
    {"cancel_during_admission", cancellation_during_admission},
    {"stale_revision", stale_revision_completion}, {"end_during_apply", end_during_apply},
    {"destructor_failure", destructor_failure}, {"binding_authority", runtime_binding_authority},
    {"notify_reentrancy", notify_reentrancy}, {"event_retry_deadline", waiting_deadline_and_event_retry},
    {"close_owner_shared_provider", close_owner_keeps_shared_provider},
    {"close_provider_owner", close_provider_owner},
    {"cache_bank_partial_drain", cache_bank_partial_and_drain},
    {"cache_bank_repin_queued", cache_bank_repin_queued},
    {"cache_bank_empty_queued", cache_bank_empty_queued},
    {"cache_bank_stop_held_queued", cache_bank_stop_held_queued},
    {"cache_bank_async", cache_bank_async}, {"cache_bank_inline_completion", cache_bank_inline_completion},
    {"feedback_history", feedback_history}, {"feedback_selection", feedback_selection},
    {"feedback_expiry", feedback_expiry}, {"feedback_bank", feedback_bank},
    {"feedback_class_protection", feedback_class_protection}
};
int main(int argc, char **argv)
{
    size_t i;
    CHECK(argc == 2);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(argv[1], "--list")) puts(cases[i].name);
        else if (!strcmp(argv[1], cases[i].name)) { cases[i].run(); printf("PASS %s\n", cases[i].name); return 0; }
    }
    return strcmp(argv[1], "--list") ? 2 : 0;
}
