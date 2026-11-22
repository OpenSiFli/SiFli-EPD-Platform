#include "epd_resource.h"
#include "epd_resource_feedback.h"
#include <string.h>
#include <limits.h>
#include <rtm.h>
#include <mem_section.h>

/* Bounded control storage, independent of the managed payload allocator. */
#define ACTIVITY_COUNT 64
#define BINDING_COUNT 64
#define PROVIDER_COUNT 32
#define RESOURCE_COUNT 128
#define REFERENCE_COUNT 256
#define BACKEND_COUNT 16
#define HASH_COUNT 127
#define NONE (-1)

typedef struct job {
    struct job *next;
    unsigned type;
    int index;
    bool queued, running;
} job_t;

typedef struct {
    uint64_t id;
    epd_memory_owner_t owner, account;
    uint64_t generation, scope;
    uint32_t mask, authorized_mask;
    unsigned activities;
    bool stopped;
} binding_t;

typedef struct {
    epd_resource_activity_snapshot_t view;
    epd_resource_observer_t observer;
    epd_memory_owner_t account;
    job_t work, notice;
    int refs;
    uint16_t borrowers, executions;
    size_t resume_unused[EPD_MEMORY_BACKEND_COUNT];
    bool busy, notifying, retry, owner_held, auto_close;
} activity_t;

typedef struct {
    uint64_t id;
    epd_resource_provider_spec_t spec;
    epd_memory_owner_t account;
    job_t drain;
    epd_resource_t caches[BACKEND_COUNT];
    unsigned resources, callbacks;
    bool stopped, auto_close, owner_held, lifetime_held;
} provider_t;

typedef struct {
    epd_resource_snapshot_t view;
    epd_resource_provider_t provider;
    epd_resource_credit_t credit;
    void *data;
    job_t work;
    uint64_t age;
    int refs, hash_next, heap_pos, backend;
    unsigned busy;
    uint64_t offer_revision;
    size_t pending_bytes;
    size_t associated_bytes;
    epd_memory_owner_t physical_account;
    bool association_active;
    bool external_waiting;
    bool started, completed, cancel_sent, retry, destroy_blocked, evicted, published, settled, reclaim_pending, external;
} resource_t;

typedef struct {
    uint64_t id;
    int activity, resource, next_activity, next_resource;
    unsigned pins;
    epd_resource_notify_t notify;
    void *context;
    epd_resource_lane_t lane;
    job_t notice;
    bool closing, delivered, notifying, association_holder;
} reference_t;

typedef struct {
    bool used;
    uint32_t id;
    unsigned resources, count;
    epd_resource_reclaim_snapshot_t reclaim;
    uint16_t heap[RESOURCE_COUNT];
} backend_t;

_Static_assert(RESOURCE_COUNT <= UINT16_MAX, "resource heap index width");

typedef struct {
    uint64_t key, load_started;
    uint32_t weight, load_cost;
} resource_feedback_t;
typedef struct {
    epd_resource_feedback_t history;
    resource_feedback_t resources[RESOURCE_COUNT];
} feedback_control_t;

L2_RET_BSS_SECT_BEGIN(epd_resources)
static struct {
    epd_resource_runtime_t rt;
    bool pumping[2];
    uint64_t serial, age, clock;
    binding_t bindings[BINDING_COUNT];
    activity_t activities[ACTIVITY_COUNT];
    provider_t providers[PROVIDER_COUNT];
    resource_t resources[RESOURCE_COUNT];
    reference_t references[REFERENCE_COUNT];
    backend_t backends[BACKEND_COUNT];
    int hash[HASH_COUNT];
    job_t *head[2], *tail[2];
    epd_resource_stats_t stats;
    feedback_control_t *feedback;
    job_t binding_cleanup;
} g L2_RET_BSS_SECT(epd_resources);
L2_RET_BSS_SECT_END
static bool metadata_initialized;

enum { JOB_ACTIVITY, JOB_ACTIVITY_NOTICE, JOB_RESOURCE, JOB_REFERENCE_NOTICE, JOB_PROVIDER, JOB_BINDINGS };
#define binding_cleanup g.binding_cleanup
static resource_feedback_t *feedback_of(const resource_t *r)
{ return &g.feedback->resources[r - g.resources]; }

static void lock_state(void)
{
    uint64_t now = g.rt.now(g.rt.context);
    g.rt.lock(g.rt.context);
    if (now > g.clock) g.clock = now;
}
static void unlock_state(void) { g.rt.unlock(g.rt.context); }
static uint64_t new_id(int slot) { return (++g.serial << 32) | (uint32_t)(slot + 1); }
static int slot_of(uint64_t id, unsigned count)
{
    uint32_t slot = (uint32_t)id;
    return slot && slot <= count ? (int)slot - 1 : NONE;
}
static activity_t *activity_of(uint64_t id)
{
    int i = slot_of(id, ACTIVITY_COUNT);
    return i >= 0 && g.activities[i].view.activity == id ? &g.activities[i] : NULL;
}
static binding_t *binding_of(uint64_t id)
{
    int i = slot_of(id, BINDING_COUNT);
    return i >= 0 && g.bindings[i].id == id ? &g.bindings[i] : NULL;
}
static provider_t *provider_of(uint64_t id)
{
    int i = slot_of(id, PROVIDER_COUNT);
    return i >= 0 && g.providers[i].id == id ? &g.providers[i] : NULL;
}
static resource_t *resource_of(uint64_t id)
{
    int i = slot_of(id, RESOURCE_COUNT);
    return i >= 0 && g.resources[i].view.resource == id ? &g.resources[i] : NULL;
}
static reference_t *reference_of(uint64_t id)
{
    int i = slot_of(id, REFERENCE_COUNT);
    return i >= 0 && g.references[i].id == id ? &g.references[i] : NULL;
}
static void init_job(job_t *j, unsigned type, int index)
{
    j->type = type;
    j->index = index;
}
static void queue_job(job_t *j, epd_resource_lane_t lane)
{
    if (j->queued) return;
    j->queued = true;
    j->next = NULL;
    if (g.tail[lane]) g.tail[lane]->next = j;
    else g.head[lane] = j;
    g.tail[lane] = j;
}
static void unqueue_job(job_t *j)
{
    unsigned lane;
    if (!j->queued) return;
    for (lane = 0; lane < 2; ++lane) {
        job_t **link = &g.head[lane], *previous = NULL;
        while (*link && *link != j) { previous = *link; link = &(*link)->next; }
        if (*link) {
            *link = j->next;
            if (g.tail[lane] == j) g.tail[lane] = previous;
            j->next = NULL; j->queued = false;
            return;
        }
    }
}
static void wake(void)
{
    bool lanes[2];
    lock_state();
    lanes[0] = g.head[0] != NULL;
    lanes[1] = g.head[1] != NULL;
    unlock_state();
    if (lanes[0]) g.rt.wake(g.rt.context, EPD_RESOURCE_WORKER);
    if (lanes[1]) g.rt.wake(g.rt.context, EPD_RESOURCE_UI);
}
static void release_credit(epd_resource_credit_t credit)
{
    if (credit) g.rt.credit_release(g.rt.context, credit);
}
static epd_memory_owner_t account_of(epd_memory_owner_t owner)
{
    return g.rt.owner_account ? g.rt.owner_account(g.rt.context, owner) : owner;
}
static bool within(epd_memory_owner_t owner, epd_memory_owner_t ancestor)
{
    return g.rt.owner_is_within ? g.rt.owner_is_within(g.rt.context, owner, ancestor) : owner == ancestor;
}
static void activity_notice(activity_t *a)
{
    if (a->observer.notify) queue_job(&a->notice, a->observer.lane);
}
static bool ending(const activity_t *a)
{
    return a->view.state == EPD_RESOURCE_ENDING || a->view.state == EPD_RESOURCE_ENDED;
}
static bool paused(const activity_t *a)
{
    return a->view.state == EPD_RESOURCE_PAUSING || a->view.state == EPD_RESOURCE_PAUSED ||
        a->view.state == EPD_RESOURCE_RESUMING;
}
static epd_resource_kind_t priority_of(const activity_t *a)
{
    return a->view.effective_revision ? a->view.effective.kind : a->view.requested.kind;
}
static bool valid_spec(const epd_resource_activity_spec_t *s, epd_memory_owner_t owner)
{
    binding_t *b;
    activity_t *parent;
    if (!s || (unsigned)s->kind > EPD_RESOURCE_INTERACTIVE ||
        s->backend >= EPD_MEMORY_BACKEND_COUNT || s->companion_bytes[s->backend] ||
        s->minimum_bytes > s->target_bytes) return false;
    b = binding_of(s->binding);
    if (!b || b->stopped || b->owner != owner || !(b->mask & EPD_RESOURCE_KIND_MASK(s->kind)))
        return false;
    if (s->kind != EPD_RESOURCE_NEARBY) return true;
    parent = activity_of(s->interactive_parent);
    return parent && !ending(parent) && parent->view.owner == owner &&
        parent->view.instance_generation == b->generation &&
        parent->view.requested.kind == EPD_RESOURCE_INTERACTIVE &&
        binding_of(parent->view.requested.binding)->scope == b->scope;
}

static unsigned key_hash(epd_resource_provider_t provider, const epd_resource_key_t *key)
{
    uint64_t h = provider ^ key->version;
    unsigned i;
    for (i = 0; i < key->length; ++i) h = (h ^ key->bytes[i]) * UINT64_C(1099511628211);
    return (unsigned)(h % HASH_COUNT);
}
static bool key_equal(const epd_resource_key_t *a, const epd_resource_key_t *b)
{
    return a->version == b->version && a->length == b->length &&
        !memcmp(a->bytes, b->bytes, a->length);
}
static void hash_remove(resource_t *r)
{
    unsigned bucket = key_hash(r->provider, &r->view.descriptor.key);
    int *p = &g.hash[bucket];
    int index = (int)(r - g.resources);
    while (*p != NONE && *p != index) p = &g.resources[*p].hash_next;
    if (*p == index) *p = r->hash_next;
    r->hash_next = NONE;
}

/* A total order: class, age cohort, rebuild cost, provider-local value, exact age.
 * Local units are compared only for the same provider. */
static bool before(int x, int y)
{
    resource_t *a = &g.resources[x], *b = &g.resources[y];
    if (a->view.priority != b->view.priority) return a->view.priority < b->view.priority;
    if (feedback_of(a)->weight != feedback_of(b)->weight) return feedback_of(a)->weight < feedback_of(b)->weight;
    if ((a->age >> 6) != (b->age >> 6)) return (a->age >> 6) < (b->age >> 6);
    if (a->view.descriptor.rebuild_cost != b->view.descriptor.rebuild_cost)
        return a->view.descriptor.rebuild_cost < b->view.descriptor.rebuild_cost;
    if (a->provider != b->provider) return a->provider < b->provider;
    if (a->view.descriptor.local_value != b->view.descriptor.local_value)
        return a->view.descriptor.local_value < b->view.descriptor.local_value;
    return a->age != b->age ? a->age < b->age : x < y;
}
static void heap_swap(backend_t *b, unsigned x, unsigned y)
{
    uint16_t t = b->heap[x]; b->heap[x] = b->heap[y]; b->heap[y] = t;
    g.resources[b->heap[x]].heap_pos = (int)x;
    g.resources[b->heap[y]].heap_pos = (int)y;
}
static void heap_fix(backend_t *b, unsigned p)
{
    unsigned next;
    while (p && before(b->heap[p], b->heap[(p - 1) / 2])) {
        next = (p - 1) / 2; heap_swap(b, p, next); p = next;
    }
    for (;;) {
        next = p * 2 + 1;
        if (next >= b->count) break;
        if (next + 1 < b->count && before(b->heap[next + 1], b->heap[next])) ++next;
        if (!before(b->heap[next], b->heap[p])) break;
        heap_swap(b, p, next); p = next;
    }
}
static void candidate_remove(resource_t *r)
{
    backend_t *b = &g.backends[r->backend];
    unsigned p;
    if (r->heap_pos < 0) return;
    p = (unsigned)r->heap_pos;
    --b->count;
    r->heap_pos = NONE;
    if (p < b->count) {
        b->heap[p] = b->heap[b->count];
        g.resources[b->heap[p]].heap_pos = (int)p;
        heap_fix(b, p);
    }
}
static void resource_refresh(resource_t *r)
{
    int i;
    bool guarantee = false;
    epd_resource_kind_t priority = r->view.references ? EPD_RESOURCE_OPTIONAL : r->view.priority;
    backend_t *b = &g.backends[r->backend];
    candidate_remove(r);
    feedback_of(r)->weight = epd_resource_feedback_weight(&g.feedback->history, r->provider,
        feedback_of(r)->key, g.clock);
    for (i = r->refs; i != NONE; i = g.references[i].next_resource) {
        reference_t *ref = &g.references[i];
        activity_t *a = &g.activities[ref->activity];
        epd_resource_kind_t kind;
        if (ref->closing && !ref->pins) continue;
        kind = priority_of(a);
        if (kind > priority) priority = kind;
        if (!ref->closing && a->view.effective_revision &&
            (kind == EPD_RESOURCE_CONTINUOUS || kind == EPD_RESOURCE_INTERACTIVE)) guarantee = true;
    }
    r->view.priority = priority;
    if (r->view.state == EPD_RESOURCE_READY && !r->view.invalidated &&
        !r->view.pins && !r->view.dirty && r->view.descriptor.discardable &&
        !guarantee && !r->busy && !r->destroy_blocked &&
        (!r->external || r->view.descriptor.estimated_bytes)) {
        r->heap_pos = (int)b->count;
        b->heap[b->count++] = (uint16_t)(r - g.resources);
        heap_fix(b, (unsigned)r->heap_pos);
    }
}
static void resource_schedule(resource_t *r)
{
    provider_t *p = provider_of(r->provider);
    epd_resource_lane_t lane;
    if (!p) return;
    lane = r->view.state == EPD_RESOURCE_LOADING ? p->spec.load_lane : p->spec.destroy_lane;
    queue_job(&r->work, lane);
}
static void refresh_activity_resources(activity_t *a)
{
    int i;
    for (i = a->refs; i != NONE; i = g.references[i].next_activity) {
        resource_t *r = &g.resources[g.references[i].resource];
        resource_refresh(r);
        if (r->view.state == EPD_RESOURCE_LOADING) { r->retry = true; resource_schedule(r); }
    }
}
static void retire(resource_t *r)
{
    hash_remove(r);
    r->view.invalidated = true;
    candidate_remove(r);
    if (r->view.state != EPD_RESOURCE_LOADING && r->view.state != EPD_RESOURCE_DESTROYING)
        r->view.state = EPD_RESOURCE_RETIRING;
    resource_schedule(r);
}
static void finish_reclaim(resource_t *r, size_t reclaimed, bool failed)
{
    epd_resource_reclaim_snapshot_t *s = &g.backends[r->backend].reclaim;
    if (!r->reclaim_pending) return;
    if (reclaimed) epd_resource_feedback_reclaimed(&g.feedback->history, r->provider, feedback_of(r)->key,
        g.clock, feedback_of(r)->load_cost ? feedback_of(r)->load_cost : r->view.descriptor.rebuild_cost);
    r->reclaim_pending = false;
    --s->pending_objects; s->pending_bytes -= r->pending_bytes;
    s->reclaimed_bytes += reclaimed;
    r->pending_bytes = 0;
    if (failed) ++s->failed_objects;
}
static bool other_activity_ref(resource_t *r, int activity, int except)
{
    int i;
    for (i = r->refs; i != NONE; i = g.references[i].next_resource)
        if (i != except && g.references[i].activity == activity) return true;
    return false;
}
static int other_account_ref(resource_t *r, epd_memory_owner_t account, int except)
{
    int i;
    for (i = r->refs; i != NONE; i = g.references[i].next_resource)
        if (i != except && g.activities[g.references[i].activity].account == account) return i;
    return NONE;
}
static int association_change(resource_t *r, epd_memory_owner_t account, size_t old_bytes, size_t bytes)
{
    epd_memory_association_t change = {account, old_bytes, bytes};
    if (!g.rt.association_change || account == EPD_MEMORY_OWNER_SYSTEM || account == r->physical_account)
        return EPD_RESOURCE_OK;
    return g.rt.association_change(g.rt.context, r->view.descriptor.backend, &change, 1);
}
/* One leader per account carries the logical charge. Every reference, including
 * an ending activity's still-pinned reference, keeps that charge alive. */
static int association_publish(resource_t *r, size_t bytes, epd_resource_credit_t commit)
{
    epd_memory_association_t changes[ACTIVITY_COUNT];
    int leaders[ACTIVITY_COUNT];
    unsigned count = 0, j;
    int i, result = EPD_RESOURCE_OK;
    for (i = r->refs; i != NONE; i = g.references[i].next_resource) {
        epd_memory_owner_t account = g.activities[g.references[i].activity].account;
        if (account == EPD_MEMORY_OWNER_SYSTEM || account == r->physical_account) continue;
        for (j = 0; j < count && changes[j].account != account; ++j) {}
        if (j < count) continue;
        changes[count] = (epd_memory_association_t){account, r->association_active ? r->associated_bytes : 0, bytes};
        leaders[count++] = i;
    }
    if (commit && g.rt.shared_commit)
        result = g.rt.shared_commit(g.rt.context, commit, r->view.descriptor.backend, changes, count);
    else if (commit && g.rt.credit_commit) result = g.rt.credit_commit(g.rt.context, commit);
    else if (g.rt.association_change && count)
        result = g.rt.association_change(g.rt.context, r->view.descriptor.backend, changes, count);
    if (result != EPD_RESOURCE_OK) return result;
    for (i = r->refs; i != NONE; i = g.references[i].next_resource) g.references[i].association_holder = false;
    for (j = 0; j < count; ++j) g.references[leaders[j]].association_holder = true;
    r->associated_bytes = bytes; r->association_active = true;
    return EPD_RESOURCE_OK;
}
static void ref_finalize(reference_t *ref)
{
    activity_t *a = &g.activities[ref->activity];
    resource_t *r = &g.resources[ref->resource];
    int index = (int)(ref - g.references), *p;
    if (!ref->closing || ref->pins || ref->notifying || ref->notice.queued || ref->notice.running) return;
    if (ref->association_holder) {
        int next = other_account_ref(r, a->account, index);
        if (next != NONE) g.references[next].association_holder = true;
        else if (association_change(r, a->account, r->associated_bytes, 0) != EPD_RESOURCE_OK) return;
        ref->association_holder = false;
    }
    if (!other_activity_ref(r, ref->activity, index)) a->view.associated_bytes -= r->view.physical_bytes;
    p = &a->refs;
    while (*p != index) p = &g.references[*p].next_activity;
    *p = ref->next_activity;
    p = &r->refs;
    while (*p != index) p = &g.references[*p].next_resource;
    *p = ref->next_resource;
    --a->view.references;
    --r->view.references;
    --g.stats.references;
    ref->id = 0;
    resource_refresh(r);
    if (!r->view.references && (r->view.state == EPD_RESOURCE_LOADING ||
        r->view.invalidated || r->view.state == EPD_RESOURCE_LOAD_FAILED)) retire(r);
    if (ending(a)) queue_job(&a->work, EPD_RESOURCE_WORKER);
}
static void ref_close(reference_t *ref)
{
    ref->closing = true;
    ref_finalize(ref);
}
static void end_activity(activity_t *a)
{
    int i, next;
    if (ending(a)) return;
    a->view.state = EPD_RESOURCE_ENDING;
    ++a->view.requested_revision;
    a->retry = false;
    for (i = a->refs; i != NONE; i = next) {
        next = g.references[i].next_activity;
        ref_close(&g.references[i]);
    }
    refresh_activity_resources(a);
    activity_notice(a);
    queue_job(&a->work, EPD_RESOURCE_WORKER);
    for (i = 0; i < ACTIVITY_COUNT; ++i) {
        activity_t *child = &g.activities[i];
        if (child->view.activity && !ending(child) &&
            child->view.requested.kind == EPD_RESOURCE_NEARBY &&
            child->view.requested.interactive_parent == a->view.activity) end_activity(child);
    }
}

int epd_resource_init(const epd_resource_runtime_t *runtime)
{
    unsigned i;
    if (!runtime || !runtime->lock || !runtime->unlock || !runtime->current_owner ||
        !runtime->owner_retain || !runtime->owner_release || !runtime->owner_enter ||
        !runtime->owner_leave || !runtime->now || !runtime->wake ||
        !runtime->activity_apply || !runtime->resource_admit || !runtime->credit_release ||
        !runtime->control_allocate)
        return EPD_RESOURCE_INVALID;
    if (metadata_initialized) return EPD_RESOURCE_BUSY;
    feedback_control_t *feedback = runtime->control_allocate(runtime->context, sizeof(*feedback));
    if (!feedback) return EPD_RESOURCE_CAPACITY;
    memset(feedback, 0, sizeof(*feedback));
    memset(&g, 0, sizeof(g));
    g.feedback = feedback;
    g.rt = *runtime;
    for (i = 0; i < HASH_COUNT; ++i) g.hash[i] = NONE;
    memset(&binding_cleanup, 0, sizeof(binding_cleanup));
    init_job(&binding_cleanup, JOB_BINDINGS, 0);
    metadata_initialized = true;
    return EPD_RESOURCE_OK;
}

int epd_resource_binding_open(epd_memory_owner_t owner, uint64_t generation,
                              uint64_t scope, uint32_t mask, epd_resource_binding_t *out)
{
    int i;
    epd_memory_owner_t account;
    if (!metadata_initialized || !out || !generation || !scope || !mask || (mask & ~15u))
        return EPD_RESOURCE_INVALID;
    if (g.rt.owner_retain(g.rt.context, owner) != EPD_RESOURCE_OK) return EPD_RESOURCE_STOPPED;
    account = account_of(owner);
    lock_state();
    for (i = 0; i < BINDING_COUNT && g.bindings[i].id; ++i) {}
    if (i < BINDING_COUNT) {
        binding_t *b = &g.bindings[i];
        memset(b, 0, sizeof(*b));
        b->id = new_id(i); b->owner = owner; b->account = account; b->generation = generation;
        b->scope = scope; b->mask = b->authorized_mask = mask; *out = b->id;
    }
    unlock_state();
    if (i == BINDING_COUNT) g.rt.owner_release(g.rt.context, owner);
    return i < BINDING_COUNT ? EPD_RESOURCE_OK : EPD_RESOURCE_CAPACITY;
}

int epd_resource_binding_restrict(epd_resource_binding_t id, uint32_t mask)
{
    binding_t *b;
    int i;
    if (!metadata_initialized || (mask & ~15u)) return EPD_RESOURCE_INVALID;
    lock_state(); b = binding_of(id);
    if (!b || b->stopped || (mask & ~b->authorized_mask)) { unlock_state(); return EPD_RESOURCE_INVALID; }
    b->mask = mask;
    for (i = 0; i < ACTIVITY_COUNT; ++i) {
        activity_t *a = &g.activities[i];
        if (!a->view.activity || a->view.requested.binding != id || ending(a)) continue;
        if (!(mask & EPD_RESOURCE_KIND_MASK(a->view.requested.kind))) {
            if (paused(a) && (mask & EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_OPTIONAL))) {
                /* Removing task proof cannot replace or end a suspended token. */
                if (a->view.state == EPD_RESOURCE_RESUMING) {
                    a->view.state = EPD_RESOURCE_PAUSED;
                    ++a->view.requested_revision;
                    a->view.reason = EPD_RESOURCE_DENIED;
                    a->retry = false;
                    queue_job(&a->work, EPD_RESOURCE_WORKER);
                }
                a->view.effective.kind = EPD_RESOURCE_OPTIONAL;
                activity_notice(a);
                continue;
            }
            if (mask & EPD_RESOURCE_KIND_MASK(EPD_RESOURCE_OPTIONAL)) {
                a->view.requested.kind = EPD_RESOURCE_OPTIONAL;
                a->view.requested.minimum_bytes = 0;
                ++a->view.requested_revision;
                /* Lifecycle demotion removes priority immediately, while live pins survive. */
                a->view.effective.kind = EPD_RESOURCE_OPTIONAL;
                a->view.state = EPD_RESOURCE_WAITING;
                a->retry = true;
                refresh_activity_resources(a);
                queue_job(&a->work, EPD_RESOURCE_WORKER);
                activity_notice(a);
            } else end_activity(a);
        }
    }
    unlock_state(); wake(); return EPD_RESOURCE_OK;
}
int epd_resource_binding_end(epd_resource_binding_t id)
{
    binding_t *b;
    epd_memory_owner_t owner = 0;
    bool release = false;
    int i, result;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); b = binding_of(id);
    if (!b) { unlock_state(); return EPD_RESOURCE_STALE; }
    b->stopped = true;
    for (i = 0; i < ACTIVITY_COUNT; ++i)
        if (g.activities[i].view.activity && g.activities[i].view.requested.binding == id)
            end_activity(&g.activities[i]);
    result = b->activities ? EPD_RESOURCE_PENDING : EPD_RESOURCE_OK;
    if (!b->activities) { owner = b->owner; b->id = 0; release = true; }
    unlock_state();
    if (release) g.rt.owner_release(g.rt.context, owner);
    wake(); return result;
}

int epd_resource_activity_begin(const epd_resource_activity_spec_t *spec,
                               const epd_resource_observer_t *observer,
                               epd_resource_activity_t *out)
{
    epd_memory_owner_t owner;
    int i, result = EPD_RESOURCE_ACCEPTED;
    if (!metadata_initialized || !out || (observer && observer->lane > EPD_RESOURCE_UI))
        return EPD_RESOURCE_INVALID;
    owner = g.rt.current_owner(g.rt.context);
    if (g.rt.owner_retain(g.rt.context, owner) != EPD_RESOURCE_OK) return EPD_RESOURCE_STOPPED;
    lock_state();
    if (!valid_spec(spec, owner)) result = EPD_RESOURCE_DENIED;
    else {
        for (i = 0; i < ACTIVITY_COUNT && g.activities[i].view.activity; ++i) {}
        if (i == ACTIVITY_COUNT) result = EPD_RESOURCE_CAPACITY;
        else {
            activity_t *a = &g.activities[i];
            binding_t *b = binding_of(spec->binding);
            memset(a, 0, sizeof(*a));
            a->view.activity = new_id(i); a->view.owner = owner;
            a->account = b->account;
            a->view.instance_generation = b->generation;
            a->view.requested_revision = 1; a->view.requested = *spec;
            a->view.state = EPD_RESOURCE_WAITING; a->refs = NONE;
            a->retry = true; a->owner_held = true;
            if (observer) a->observer = *observer;
            init_job(&a->work, JOB_ACTIVITY, i); init_job(&a->notice, JOB_ACTIVITY_NOTICE, i);
            ++b->activities; ++g.stats.activities; *out = a->view.activity;
            queue_job(&a->work, EPD_RESOURCE_WORKER); activity_notice(a);
        }
    }
    unlock_state();
    if (result != EPD_RESOURCE_ACCEPTED) g.rt.owner_release(g.rt.context, owner);
    wake(); return result;
}
int epd_resource_activity_update(epd_resource_activity_t id,
                                const epd_resource_activity_spec_t *spec, uint64_t *revision)
{
    activity_t *a;
    epd_memory_owner_t owner;
    if (!metadata_initialized || !revision) return EPD_RESOURCE_INVALID;
    owner = g.rt.current_owner(g.rt.context);
    lock_state(); a = activity_of(id);
    if (!a) { unlock_state(); return EPD_RESOURCE_STALE; }
    if (ending(a)) { unlock_state(); return EPD_RESOURCE_STOPPED; }
    if (paused(a)) { unlock_state(); return EPD_RESOURCE_BUSY; }
    if (a->view.owner != owner || !valid_spec(spec, owner) ||
        spec->binding != a->view.requested.binding) { unlock_state(); return EPD_RESOURCE_DENIED; }
    a->view.requested = *spec; *revision = ++a->view.requested_revision;
    a->view.state = EPD_RESOURCE_WAITING; a->view.reason = 0; a->retry = true;
    queue_job(&a->work, EPD_RESOURCE_WORKER); activity_notice(a);
    unlock_state(); wake(); return EPD_RESOURCE_ACCEPTED;
}
int epd_resource_activity_get(epd_resource_activity_t id, epd_resource_activity_snapshot_t *out)
{
    activity_t *a;
    if (!metadata_initialized || !out) return EPD_RESOURCE_INVALID;
    lock_state(); a = activity_of(id);
    if (a) *out = a->view;
    unlock_state(); return a ? EPD_RESOURCE_OK : EPD_RESOURCE_STALE;
}
/* A running slice may finish on its admitted credit. Once a replacement
 * is queued, further slices wait for the old ones to leave. */
epd_resource_credit_t epd_resource_execution_acquire(epd_resource_activity_t id)
{
    if (!metadata_initialized) return 0;
    epd_memory_owner_t owner = g.rt.current_owner(g.rt.context);
    epd_resource_credit_t credit = 0;
    lock_state();
    activity_t *a = activity_of(id);
    if (a && !a->busy && !a->retry && !ending(a) && !paused(a) &&
        a->view.effective_revision && a->view.credit && a->executions < UINT16_MAX &&
        (owner == a->view.owner || (g.rt.owner_is_within &&
         g.rt.owner_is_within(g.rt.context, owner, a->view.owner)))) {
        credit = a->view.credit;
        ++a->executions;
    }
    unlock_state();
    return credit;
}

static activity_t *execution_of(epd_resource_credit_t credit)
{
    for (unsigned i = 0; i < ACTIVITY_COUNT; ++i)
        if (g.activities[i].executions && g.activities[i].view.credit == credit)
            return &g.activities[i];
    return NULL;
}

void epd_resource_execution_release(epd_resource_credit_t credit)
{
    if (!metadata_initialized || !credit) return;
    lock_state();
    activity_t *a = execution_of(credit);
    bool scheduled = a && !--a->executions && (a->retry || ending(a));
    if (scheduled) queue_job(&a->work, EPD_RESOURCE_WORKER);
    unlock_state();
    if (scheduled) wake();
}

int epd_resource_activity_pause(epd_resource_activity_t id)
{
    activity_t *a;
    epd_memory_owner_t owner;
    if (!metadata_initialized || !g.rt.activity_suspend) return EPD_RESOURCE_INVALID;
    owner = g.rt.current_owner(g.rt.context);
    lock_state(); a = activity_of(id);
    if (!a) { unlock_state(); return EPD_RESOURCE_STALE; }
    if (ending(a)) { unlock_state(); return EPD_RESOURCE_STOPPED; }
    if (owner != a->view.owner && (!g.rt.owner_is_within ||
        !g.rt.owner_is_within(g.rt.context, owner, a->view.owner))) {
        unlock_state(); return EPD_RESOURCE_DENIED;
    }
    if (a->view.state == EPD_RESOURCE_PAUSED) { unlock_state(); return EPD_RESOURCE_OK; }
    if (a->view.state == EPD_RESOURCE_PAUSING) { unlock_state(); return EPD_RESOURCE_PENDING; }
    if (!a->view.credit || a->busy || a->borrowers || a->executions || a->view.references || a->view.pins) {
        unlock_state(); return EPD_RESOURCE_BUSY;
    }
    a->view.state = EPD_RESOURCE_PAUSING; a->view.reason = 0;
    ++a->view.requested_revision; a->retry = true;
    queue_job(&a->work, EPD_RESOURCE_WORKER); activity_notice(a);
    unlock_state(); wake(); return EPD_RESOURCE_ACCEPTED;
}
int epd_resource_activity_resume(epd_resource_activity_t id,
    const epd_resource_activity_spec_t *spec, const size_t unused[EPD_MEMORY_BACKEND_COUNT], uint64_t *revision)
{
    activity_t *a;
    epd_memory_owner_t owner;
    if (!metadata_initialized || !g.rt.activity_resume || !g.rt.credit_resume_commit ||
        !spec || !unused || !revision || spec->backend >= EPD_MEMORY_BACKEND_COUNT ||
        unused[spec->backend] != spec->target_bytes) return EPD_RESOURCE_INVALID;
    owner = g.rt.current_owner(g.rt.context);
    lock_state(); a = activity_of(id);
    if (!a) { unlock_state(); return EPD_RESOURCE_STALE; }
    if (ending(a)) { unlock_state(); return EPD_RESOURCE_STOPPED; }
    if (a->view.state != EPD_RESOURCE_PAUSED || a->busy) { unlock_state(); return EPD_RESOURCE_BUSY; }
    if (a->view.owner != owner || !valid_spec(spec, owner) || spec->binding != a->view.requested.binding) {
        unlock_state(); return EPD_RESOURCE_DENIED;
    }
    memcpy(a->resume_unused, unused, sizeof(a->resume_unused));
    a->view.requested = *spec; *revision = ++a->view.requested_revision;
    a->view.state = EPD_RESOURCE_RESUMING; a->view.reason = 0; a->retry = true;
    queue_job(&a->work, EPD_RESOURCE_WORKER); activity_notice(a);
    unlock_state(); wake(); return EPD_RESOURCE_ACCEPTED;
}
int epd_resource_activity_end(epd_resource_activity_t id)
{
    activity_t *a;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); a = activity_of(id);
    if (a) end_activity(a);
    unlock_state(); wake(); return a ? EPD_RESOURCE_ACCEPTED : EPD_RESOURCE_STALE;
}
int epd_resource_activity_forget(epd_resource_activity_t id)
{
    activity_t *a;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); a = activity_of(id);
    if (!a) { unlock_state(); return EPD_RESOURCE_STALE; }
    if (a->view.state != EPD_RESOURCE_ENDED || a->busy || a->notifying ||
        a->work.queued || a->notice.queued || a->work.running || a->notice.running || a->owner_held) {
        unlock_state(); return EPD_RESOURCE_PENDING;
    }
    a->view.activity = 0; --g.stats.activities;
    unlock_state(); return EPD_RESOURCE_OK;
}
int epd_resource_activity_dispose(epd_resource_activity_t id)
{
    activity_t *a;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); a = activity_of(id);
    if (a) { a->auto_close = true; end_activity(a); queue_job(&a->work, EPD_RESOURCE_WORKER); }
    unlock_state(); wake(); return a ? EPD_RESOURCE_ACCEPTED : EPD_RESOURCE_STALE;
}

int epd_resource_provider_register(const epd_resource_provider_spec_t *spec,
                                   epd_resource_provider_t *out)
{
    int i;
    epd_memory_owner_t account;
    epd_memory_owner_t lifetime;
    if (!metadata_initialized || !out || !spec || (!spec->shrink && (!spec->load || !spec->destroy)) ||
        spec->load_lane > EPD_RESOURCE_UI || spec->destroy_lane > EPD_RESOURCE_UI)
        return EPD_RESOURCE_INVALID;
    if (g.rt.owner_retain(g.rt.context, spec->code_owner) != EPD_RESOURCE_OK)
        return EPD_RESOURCE_STOPPED;
    account = account_of(spec->code_owner);
    lifetime = spec->lifetime_owner ? spec->lifetime_owner : spec->code_owner;
    if (lifetime != spec->code_owner && g.rt.owner_retain(g.rt.context, lifetime) != EPD_RESOURCE_OK) {
        g.rt.owner_release(g.rt.context, spec->code_owner); return EPD_RESOURCE_STOPPED;
    }
    lock_state();
    for (i = 0; i < PROVIDER_COUNT && (g.providers[i].id || g.providers[i].drain.running); ++i) {}
    if (i < PROVIDER_COUNT) {
        provider_t *p = &g.providers[i];
        memset(p, 0, sizeof(*p)); p->id = new_id(i); p->spec = *spec;
        p->account = account; p->owner_held = true;
        p->spec.lifetime_owner = lifetime; p->lifetime_held = lifetime != spec->code_owner;
        init_job(&p->drain, JOB_PROVIDER, i);
        *out = p->id; ++g.stats.providers;
    }
    unlock_state();
    if (i == PROVIDER_COUNT) {
        g.rt.owner_release(g.rt.context, spec->code_owner);
        if (lifetime != spec->code_owner) g.rt.owner_release(g.rt.context, lifetime);
    }
    return i < PROVIDER_COUNT ? EPD_RESOURCE_OK : EPD_RESOURCE_CAPACITY;
}
int epd_resource_provider_unregister(epd_resource_provider_t id)
{
    provider_t *p;
    int i, result;
    epd_memory_owner_t owner = 0;
    epd_memory_owner_t lifetime = 0;
    bool release_lifetime = false;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); p = provider_of(id);
    if (!p) { unlock_state(); return EPD_RESOURCE_STALE; }
    p->stopped = true;
    for (i = 0; i < RESOURCE_COUNT; ++i) {
        resource_t *r = &g.resources[i];
        if (!r->view.resource || r->provider != id) continue;
        /* An empty external cache has no payload callback to defer. */
        if (r->external && !r->view.physical_bytes && !r->external_waiting && !r->work.running) {
            candidate_remove(r); unqueue_job(&r->work);
            finish_reclaim(r, 0, false);
            --g.backends[r->backend].resources; --g.stats.resources; --p->resources;
            p->caches[r->backend] = 0; r->view.resource = 0;
        } else retire(r);
    }
    result = p->resources || p->callbacks || p->drain.running || p->drain.queued ? EPD_RESOURCE_PENDING : EPD_RESOURCE_OK;
    if (result == EPD_RESOURCE_OK) {
        owner = p->spec.code_owner; lifetime = p->spec.lifetime_owner; release_lifetime = p->lifetime_held;
        p->id = 0; --g.stats.providers;
    }
    unlock_state();
    if (result == EPD_RESOURCE_OK) g.rt.owner_release(g.rt.context, owner);
    if (release_lifetime) g.rt.owner_release(g.rt.context, lifetime);
    wake(); return result;
}

int epd_resource_provider_cache_update(epd_resource_provider_t id, uint32_t backend,
                                       size_t total, size_t reclaimable, uint32_t cost, uint32_t local)
{
    provider_t *p;
    resource_t *r = NULL;
    int i, bi;
    if (!metadata_initialized || reclaimable > total) return EPD_RESOURCE_INVALID;
    lock_state(); p = provider_of(id);
    if (!p || !p->spec.shrink) { unlock_state(); return EPD_RESOURCE_STALE; }
    for (bi = 0; bi < BACKEND_COUNT; ++bi)
        if (g.backends[bi].used && g.backends[bi].id == backend) break;
    if (bi < BACKEND_COUNT) r = resource_of(p->caches[bi]);
    if (!r) {
        if (p->stopped) { unlock_state(); return EPD_RESOURCE_STOPPED; }
        if (!total) { unlock_state(); return EPD_RESOURCE_OK; }
        if (bi == BACKEND_COUNT) {
            for (bi = 0; bi < BACKEND_COUNT && g.backends[bi].used; ++bi) {}
            if (bi == BACKEND_COUNT) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
        }
        for (i = 0; i < RESOURCE_COUNT && (g.resources[i].view.resource || g.resources[i].work.running); ++i) {}
        if (i == RESOURCE_COUNT) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
        g.backends[bi].used = true; g.backends[bi].id = backend; ++g.backends[bi].resources;
        r = &g.resources[i]; memset(r, 0, sizeof(*r));
        memset(feedback_of(r), 0, sizeof(*feedback_of(r)));
        r->view.resource = new_id(i); r->provider = id; r->backend = bi;
        feedback_of(r)->key = (uint64_t)backend << 32;
        r->refs = r->hash_next = r->heap_pos = NONE; r->external = true;
        r->view.state = EPD_RESOURCE_READY; r->view.descriptor.discardable = true;
        r->view.descriptor.backend = backend; r->age = ++g.age;
        init_job(&r->work, JOB_RESOURCE, i);
        p->caches[bi] = r->view.resource; ++p->resources; ++g.stats.resources;
    }
    if (total > r->view.physical_bytes && total - r->view.physical_bytes > SIZE_MAX - g.stats.physical_bytes) {
        unlock_state(); return EPD_RESOURCE_CAPACITY;
    }
    /* A bank refill is aggregate evidence, not a claim of an identical page hit. */
    if (total > r->view.physical_bytes && !r->reclaim_pending)
        epd_resource_feedback_rebuilt(&g.feedback->history, id, feedback_of(r)->key, g.clock);
    g.stats.physical_bytes -= r->view.physical_bytes;
    r->view.physical_bytes = total; g.stats.physical_bytes += total;
    r->view.descriptor.estimated_bytes = reclaimable;
    r->view.descriptor.rebuild_cost = cost; r->view.descriptor.local_value = local;
    ++r->offer_revision; r->destroy_blocked = false;
    resource_refresh(r);
    if (p->stopped) resource_schedule(r);
    unlock_state(); wake(); return EPD_RESOURCE_OK;
}

int epd_resource_provider_cache_complete(epd_resource_provider_t id, uint32_t backend,
                                         size_t released, int result)
{
    provider_t *p; resource_t *r = NULL; unsigned i;
    if (!metadata_initialized || result == EPD_RESOURCE_PENDING) return EPD_RESOURCE_INVALID;
    lock_state(); p = provider_of(id);
    if (p) for (i = 0; i < BACKEND_COUNT; ++i)
        if (g.backends[i].used && g.backends[i].id == backend) { r = resource_of(p->caches[i]); break; }
    if (!r || !r->external_waiting) { unlock_state(); return EPD_RESOURCE_STALE; }
    r->external_waiting = false;
    finish_reclaim(r, released, result != EPD_RESOURCE_OK); g.stats.reclaimed_bytes += released;
    r->evicted = false; r->view.invalidated = p->stopped;
    r->view.state = p->stopped ? EPD_RESOURCE_RETIRING : EPD_RESOURCE_READY;
    r->destroy_blocked = result != EPD_RESOURCE_OK;
    if (p->stopped && (!r->view.physical_bytes || (released && r->view.descriptor.estimated_bytes))) resource_schedule(r);
    resource_refresh(r);
    unlock_state();
    if (released && g.rt.payload_released) g.rt.payload_released(g.rt.context, backend, released);
    wake(); return EPD_RESOURCE_OK;
}

static int acquire_resource(epd_resource_activity_t aid, epd_resource_provider_t pid,
                         const epd_resource_descriptor_t *desc,
                         epd_resource_notify_t notify, void *context,
                         epd_resource_lane_t lane, epd_resource_ref_t *out,
                         void *adopted, size_t adopted_bytes)
{
    activity_t *a;
    provider_t *p;
    resource_t *r;
    reference_t *ref;
    epd_memory_owner_t owner, physical_account;
    bool fresh, holder = false;
    int result;
    int i, ri, bi;
    unsigned bucket;
    if (!metadata_initialized || !desc || !out || lane > EPD_RESOURCE_UI ||
        !desc->key.length || desc->key.length > EPD_RESOURCE_KEY_BYTES) return EPD_RESOURCE_INVALID;
    owner = g.rt.current_owner(g.rt.context);
    physical_account = adopted ? account_of(owner) : EPD_MEMORY_OWNER_SYSTEM;
    if (adopted && g.rt.payload_info) {
        size_t physical = 0;
        result = g.rt.payload_info(g.rt.context, adopted, desc->backend, &physical_account, &physical);
        if (result != EPD_RESOURCE_OK) return result;
        if (physical < adopted_bytes) return EPD_RESOURCE_INVALID;
        adopted_bytes = physical;
    }
    lock_state(); a = activity_of(aid); p = provider_of(pid);
    if (!a || !p) { unlock_state(); return EPD_RESOURCE_STALE; }
    if (!p->spec.destroy || (!adopted && !p->spec.load)) { unlock_state(); return EPD_RESOURCE_INVALID; }
    if (ending(a) || p->stopped) { unlock_state(); return EPD_RESOURCE_STOPPED; }
    if (paused(a)) { unlock_state(); return EPD_RESOURCE_BUSY; }
    if (owner != a->view.owner || desc->backend != a->view.requested.backend) {
        unlock_state(); return EPD_RESOURCE_DENIED;
    }
    for (i = 0; i < REFERENCE_COUNT && g.references[i].id; ++i) {}
    if (i == REFERENCE_COUNT) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
    bucket = key_hash(pid, &desc->key);
    for (ri = g.hash[bucket]; ri != NONE; ri = g.resources[ri].hash_next)
        if (g.resources[ri].provider == pid && key_equal(&g.resources[ri].view.descriptor.key, &desc->key)) break;
    fresh = ri == NONE;
    if (fresh) {
        for (ri = 0; ri < RESOURCE_COUNT && (g.resources[ri].view.resource || g.resources[ri].work.running); ++ri) {}
        if (ri == RESOURCE_COUNT) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
        for (bi = 0; bi < BACKEND_COUNT; ++bi)
            if (g.backends[bi].used && g.backends[bi].id == desc->backend) break;
        if (bi == BACKEND_COUNT) {
            for (bi = 0; bi < BACKEND_COUNT && g.backends[bi].used; ++bi) {}
            if (bi == BACKEND_COUNT) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
            g.backends[bi].used = true; g.backends[bi].id = desc->backend;
        }
        r = &g.resources[ri]; memset(r, 0, sizeof(*r));
        memset(feedback_of(r), 0, sizeof(*feedback_of(r)));
        r->view.resource = new_id(ri); r->view.state = EPD_RESOURCE_LOADING;
        r->view.descriptor = *desc; r->provider = pid;
        feedback_of(r)->key = UINT64_C(14695981039346656037) ^ desc->key.version ^ desc->backend;
        for (unsigned k = 0; k < desc->key.length; ++k)
            feedback_of(r)->key = (feedback_of(r)->key ^ desc->key.bytes[k]) * UINT64_C(1099511628211);
        r->physical_account = physical_account;
        r->refs = NONE; r->heap_pos = NONE; r->backend = bi; r->retry = true;
        r->hash_next = g.hash[bucket]; g.hash[bucket] = ri;
        init_job(&r->work, JOB_RESOURCE, ri);
        ++p->resources; ++g.backends[bi].resources; ++g.stats.resources;
        if (adopted) {
            if (adopted_bytes > SIZE_MAX - g.stats.physical_bytes) {
                hash_remove(r); --p->resources; --g.backends[r->backend].resources; --g.stats.resources;
                r->view.resource = 0; unlock_state(); return EPD_RESOURCE_CAPACITY;
            }
            r->associated_bytes = adopted_bytes; r->association_active = true;
            r->data = adopted; r->view.physical_bytes = adopted_bytes;
            r->view.state = EPD_RESOURCE_READY;
            r->started = r->completed = r->published = r->settled = true;
            g.stats.physical_bytes += adopted_bytes;
        }
    } else {
        r = &g.resources[ri];
        if (adopted) { unlock_state(); return EPD_RESOURCE_BUSY; }
        if (r->view.descriptor.backend != desc->backend ||
            r->view.descriptor.discardable != desc->discardable) {
            unlock_state(); return EPD_RESOURCE_INVALID;
        }
    }
    if (r->association_active && a->account != r->physical_account) {
        epd_memory_association_t change = {a->account, 0, r->associated_bytes};
        holder = a->account != EPD_MEMORY_OWNER_SYSTEM && other_account_ref(r, a->account, NONE) == NONE;
        size_t consumed = other_activity_ref(r, (int)(a - g.activities), NONE) ? 0 : r->associated_bytes;
        if (g.rt.reference_commit)
            result = g.rt.reference_commit(g.rt.context, a->view.credit, desc->backend, consumed, &change, holder ? 1u : 0u);
        else result = holder ? association_change(r, a->account, 0, r->associated_bytes) : EPD_RESOURCE_OK;
        if (result != EPD_RESOURCE_OK) {
            if (fresh) {
                hash_remove(r); --p->resources; --g.backends[r->backend].resources; --g.stats.resources;
                g.stats.physical_bytes -= r->view.physical_bytes; r->view.resource = 0;
            }
            unlock_state(); return result;
        }
    }
    ref = &g.references[i]; memset(ref, 0, sizeof(*ref));
    ref->association_holder = holder;
    ref->id = new_id(i); ref->activity = (int)(a - g.activities); ref->resource = ri;
    ref->notify = notify; ref->context = context; ref->lane = lane;
    init_job(&ref->notice, JOB_REFERENCE_NOTICE, i);
    if (!other_activity_ref(r, ref->activity, NONE)) a->view.associated_bytes += r->view.physical_bytes;
    ref->next_activity = a->refs; a->refs = i;
    ref->next_resource = r->refs; r->refs = i;
    ++a->view.references; ++r->view.references; ++g.stats.references;
    r->age = ++g.age; *out = ref->id;
    resource_refresh(r);
    if (r->view.state == EPD_RESOURCE_LOADING) { r->retry = true; resource_schedule(r); }
    else if (notify) queue_job(&ref->notice, lane);
    unlock_state(); wake(); return EPD_RESOURCE_ACCEPTED;
}

int epd_resource_acquire(epd_resource_activity_t activity, epd_resource_provider_t provider,
                         const epd_resource_descriptor_t *descriptor,
                         epd_resource_notify_t notify, void *context,
                         epd_resource_lane_t lane, epd_resource_ref_t *out)
{
    return acquire_resource(activity, provider, descriptor, notify, context, lane, out, NULL, 0);
}
int epd_resource_adopt(epd_resource_activity_t activity, epd_resource_provider_t provider,
                       const epd_resource_descriptor_t *descriptor, void *data,
                       size_t bytes, epd_resource_ref_t *out)
{
    if (!data || !bytes) return EPD_RESOURCE_INVALID;
    return acquire_resource(activity, provider, descriptor, NULL, NULL, EPD_RESOURCE_UI, out, data, bytes);
}
int epd_resource_take(epd_resource_ref_t id, void **out)
{
    reference_t *ref;
    resource_t *r;
    provider_t *p;
    backend_t *b;
    epd_resource_credit_t credit;
    if (!metadata_initialized || !out) return EPD_RESOURCE_INVALID;
    lock_state(); ref = reference_of(id);
    if (!ref) { unlock_state(); return EPD_RESOURCE_STALE; }
    r = &g.resources[ref->resource]; p = provider_of(r->provider);
    if (r->view.references != 1 || r->view.pins || r->view.dirty || r->busy ||
        r->work.running || ref->notice.queued || ref->notice.running || ref->notifying ||
        !r->data || r->view.state == EPD_RESOURCE_DESTROYING) {
        unlock_state(); return EPD_RESOURCE_PENDING;
    }
    *out = r->data; credit = r->credit;
    ref->closing = true; ref_finalize(ref);
    unqueue_job(&r->work); hash_remove(r); candidate_remove(r);
    b = &g.backends[r->backend];
    --b->resources; finish_reclaim(r, 0, false);
    --p->resources;
    if (p->auto_close && !p->resources) queue_job(&p->drain, EPD_RESOURCE_WORKER);
    --g.stats.resources; g.stats.physical_bytes -= r->view.physical_bytes;
    r->view.resource = 0;
    unlock_state(); release_credit(credit); wake(); return EPD_RESOURCE_OK;
}

int epd_resource_pin(epd_resource_ref_t id, void **out)
{
    reference_t *ref;
    resource_t *r;
    int result;
    if (!metadata_initialized || !out) return EPD_RESOURCE_INVALID;
    lock_state(); ref = reference_of(id);
    if (!ref) { unlock_state(); return EPD_RESOURCE_STALE; }
    r = &g.resources[ref->resource];
    if (ref->closing || r->evicted) result = EPD_RESOURCE_STOPPED;
    else if (r->view.state == EPD_RESOURCE_LOADING) result = EPD_RESOURCE_PENDING;
    else if (r->view.state == EPD_RESOURCE_LOAD_FAILED) result = r->view.result;
    else if (!r->published) result = r->view.result < 0 ? r->view.result : EPD_RESOURCE_STALE;
    else if (r->view.state == EPD_RESOURCE_DESTROYING || !r->data) result = EPD_RESOURCE_STALE;
    else if (ref->pins == UINT_MAX || r->view.pins == UINT32_MAX ||
             g.activities[ref->activity].view.pins == UINT32_MAX) result = EPD_RESOURCE_CAPACITY;
    else {
        ++ref->pins; ++r->view.pins; ++g.activities[ref->activity].view.pins;
        candidate_remove(r); *out = r->data; result = EPD_RESOURCE_OK;
    }
    unlock_state(); return result;
}
int epd_resource_unpin(epd_resource_ref_t id)
{
    reference_t *ref;
    resource_t *r;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); ref = reference_of(id);
    if (!ref || !ref->pins) { unlock_state(); return EPD_RESOURCE_INVALID; }
    r = &g.resources[ref->resource];
    --ref->pins; --r->view.pins; --g.activities[ref->activity].view.pins;
    r->age = ++g.age; ref_finalize(ref); resource_refresh(r);
    if (r->view.invalidated) resource_schedule(r);
    unlock_state(); wake(); return EPD_RESOURCE_OK;
}
int epd_resource_release(epd_resource_ref_t id)
{
    reference_t *ref;
    int result;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); ref = reference_of(id);
    if (!ref) { unlock_state(); return EPD_RESOURCE_STALE; }
    result = ref->pins || ref->notifying || ref->notice.queued ? EPD_RESOURCE_PENDING : EPD_RESOURCE_OK;
    ref_close(ref); unlock_state(); wake(); return result;
}
int epd_resource_ref_get(epd_resource_ref_t id, epd_resource_t *out)
{
    reference_t *ref;
    if (!metadata_initialized || !out) return EPD_RESOURCE_INVALID;
    lock_state(); ref = reference_of(id);
    if (ref) *out = g.resources[ref->resource].view.resource;
    unlock_state(); return ref ? EPD_RESOURCE_OK : EPD_RESOURCE_STALE;
}
int epd_resource_get(epd_resource_t id, epd_resource_snapshot_t *out)
{
    resource_t *r;
    if (!metadata_initialized || !out) return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id); if (r) *out = r->view;
    unlock_state(); return r ? EPD_RESOURCE_OK : EPD_RESOURCE_STALE;
}
int epd_resource_load_complete(epd_resource_t id, int result, void *data, size_t bytes)
{
    resource_t *r;
    int i;
    bool seen[ACTIVITY_COUNT] = {false};
    if (!metadata_initialized || (result != EPD_RESOURCE_OK && result >= 0) ||
        (result == EPD_RESOURCE_OK && !data) || (result < 0 && (data || bytes)))
        return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id);
    if (!r || !r->started || r->completed || r->view.state != EPD_RESOURCE_LOADING) {
        unlock_state(); return EPD_RESOURCE_STALE;
    }
    if (result == EPD_RESOURCE_OK && r->credit && g.rt.credit_physical) {
        size_t physical;
        int status = g.rt.credit_physical(g.rt.context, r->credit, r->view.descriptor.backend, &physical);
        if (status != EPD_RESOURCE_OK) { unlock_state(); return status; }
        if (physical < bytes) { unlock_state(); return EPD_RESOURCE_INVALID; }
        bytes = physical;
    }
    if (bytes > SIZE_MAX - g.stats.physical_bytes) { unlock_state(); return EPD_RESOURCE_CAPACITY; }
    if (result == EPD_RESOURCE_OK && !r->view.invalidated) {
        int admitted = association_publish(r, bytes, 0);
        if (admitted != EPD_RESOURCE_OK) result = admitted;
    } else if (r->association_active) (void)association_publish(r, 0, 0);
    r->completed = true; r->data = data; r->view.physical_bytes = bytes;
    r->published = result == EPD_RESOURCE_OK && !r->view.invalidated;
    if (r->published) {
        uint64_t now = g.clock, elapsed = now - feedback_of(r)->load_started;
        epd_resource_feedback_rebuilt(&g.feedback->history, r->provider, feedback_of(r)->key, now);
        feedback_of(r)->load_cost = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    }
    r->view.result = r->view.invalidated ? EPD_RESOURCE_CANCELLED : result;
    r->view.state = r->view.invalidated ? EPD_RESOURCE_RETIRING :
        (result == EPD_RESOURCE_OK ? EPD_RESOURCE_READY : EPD_RESOURCE_LOAD_FAILED);
    g.stats.physical_bytes += bytes;
    if (result < 0) ++g.stats.load_failures;
    for (i = r->refs; i != NONE; i = g.references[i].next_resource) {
        reference_t *ref = &g.references[i];
        if (!seen[ref->activity]) {
            g.activities[ref->activity].view.associated_bytes += bytes;
            seen[ref->activity] = true;
        }
        if (ref->notify && !ref->closing && !ref->delivered) queue_job(&ref->notice, ref->lane);
    }
    resource_refresh(r); resource_schedule(r);
    unlock_state(); wake(); return EPD_RESOURCE_OK;
}
int epd_resource_invalidate(epd_resource_t id)
{
    resource_t *r;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id); if (r) retire(r);
    unlock_state(); wake(); return r ? EPD_RESOURCE_OK : EPD_RESOURCE_STALE;
}
int epd_resource_mark_dirty(epd_resource_t id, uint64_t revision)
{
    resource_t *r;
    if (!metadata_initialized || !revision) return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id);
    if (!r || !r->data || r->view.state == EPD_RESOURCE_DESTROYING || revision <= r->view.data_revision) {
        unlock_state(); return EPD_RESOURCE_STALE;
    }
    r->view.data_revision = revision; r->view.dirty = true; candidate_remove(r);
    unlock_state(); return EPD_RESOURCE_OK;
}
int epd_resource_saved(epd_resource_t id, uint64_t revision, int result)
{
    resource_t *r;
    if (!metadata_initialized || !revision || result > 0) return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id);
    if (!r || revision > r->view.data_revision || revision < r->view.saved_revision) {
        unlock_state(); return EPD_RESOURCE_STALE;
    }
    if (result == EPD_RESOURCE_OK) {
        r->view.saved_revision = revision;
        r->view.dirty = revision != r->view.data_revision;
        r->destroy_blocked = false; resource_refresh(r);
        if (r->view.invalidated) resource_schedule(r);
    }
    r->view.result = result;
    unlock_state(); wake(); return EPD_RESOURCE_OK;
}
int epd_resource_set_local_value(epd_resource_t id, uint32_t value)
{
    resource_t *r;
    if (!metadata_initialized) return EPD_RESOURCE_INVALID;
    lock_state(); r = resource_of(id);
    if (r) { r->view.descriptor.local_value = value; resource_refresh(r); }
    unlock_state(); return r ? EPD_RESOURCE_OK : EPD_RESOURCE_STALE;
}

size_t epd_resource_reclaim_for(uint32_t backend, size_t target, size_t limit,
                                epd_resource_kind_t highest_kind)
{
    size_t selected = 0, count = 0;
    int bi;
    if (!metadata_initialized) return 0;
    lock_state();
    for (bi = 0; bi < BACKEND_COUNT; ++bi)
        if (g.backends[bi].used && g.backends[bi].id == backend) break;
    if (bi < BACKEND_COUNT) {
        backend_t *b = &g.backends[bi];
        /* Age only the bounded candidate index, on pressure, never the heap or
         * an input/cache-hit path. Expired feedback cannot protect a bank forever. */
        uint64_t now = g.clock;
        for (unsigned at = 0; at < b->count; ++at) {
            resource_t *r = &g.resources[b->heap[at]];
            feedback_of(r)->weight = epd_resource_feedback_weight(&g.feedback->history, r->provider, feedback_of(r)->key, now);
        }
        for (unsigned at = b->count; at; --at) heap_fix(b, at - 1u);
        while (b->count && selected < target && count < limit) {
            resource_t *r = &g.resources[b->heap[0]];
            size_t bytes = r->external ? r->view.descriptor.estimated_bytes : r->view.physical_bytes;
            if (r->view.priority > highest_kind) break;
            ++g.stats.candidate_visits; ++count;
            if (r->external && bytes > target - selected) bytes = target - selected;
            selected = bytes > SIZE_MAX - selected ? SIZE_MAX : selected + bytes;
            r->evicted = r->reclaim_pending = true;
            r->pending_bytes = bytes;
            ++b->reclaim.pending_objects; b->reclaim.pending_bytes += bytes;
            retire(r);
        }
    }
    unlock_state(); wake(); return selected;
}
size_t epd_resource_reclaim(uint32_t backend, size_t target, size_t limit)
{ return epd_resource_reclaim_for(backend, target, limit, EPD_RESOURCE_INTERACTIVE); }
void epd_resource_reclaim_status(uint32_t backend, epd_resource_reclaim_snapshot_t *out)
{
    int i;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!metadata_initialized) return;
    lock_state();
    for (i = 0; i < BACKEND_COUNT; ++i)
        if (g.backends[i].used && g.backends[i].id == backend) { *out = g.backends[i].reclaim; break; }
    unlock_state();
}
void epd_resource_retry(void)
{
    int i;
    if (!metadata_initialized) return;
    lock_state();
    for (i = 0; i < ACTIVITY_COUNT; ++i) {
        activity_t *a = &g.activities[i];
        if (a->view.activity && (a->view.state == EPD_RESOURCE_WAITING ||
            a->view.state == EPD_RESOURCE_PAUSING || a->view.state == EPD_RESOURCE_RESUMING)) {
            a->retry = true; queue_job(&a->work, EPD_RESOURCE_WORKER);
        }
    }
    for (i = 0; i < RESOURCE_COUNT; ++i) {
        resource_t *r = &g.resources[i];
        if (!r->view.resource) continue;
        if (r->view.state == EPD_RESOURCE_LOADING || r->destroy_blocked) {
            r->retry = true; r->destroy_blocked = false;
            resource_refresh(r); resource_schedule(r);
        }
    }
    unlock_state(); wake();
}
void epd_resource_stats(epd_resource_stats_t *out)
{
    if (!metadata_initialized || !out) return;
    lock_state(); *out = g.stats; unlock_state();
}

int epd_resource_close_owner(epd_memory_owner_t owner)
{
    int i;
    if (!metadata_initialized || owner == EPD_MEMORY_OWNER_SYSTEM) return EPD_RESOURCE_INVALID;
    if (owner == EPD_MEMORY_OWNER_INVALID) return EPD_RESOURCE_INVALID;
    for (i = 0; i < BINDING_COUNT; ++i) {
        uint64_t id; epd_memory_owner_t subject;
        lock_state(); id = g.bindings[i].id; subject = g.bindings[i].owner; unlock_state();
        if (id && within(subject, owner)) {
            lock_state(); if (g.bindings[i].id == id) g.bindings[i].stopped = true; unlock_state();
        }
    }
    for (i = 0; i < ACTIVITY_COUNT; ++i) {
        uint64_t id; epd_memory_owner_t subject;
        lock_state(); id = g.activities[i].view.activity; subject = g.activities[i].view.owner; unlock_state();
        if (id && within(subject, owner)) {
            lock_state();
            if (g.activities[i].view.activity == id) { g.activities[i].auto_close = true; end_activity(&g.activities[i]); }
            unlock_state();
        }
    }
    for (i = 0; i < PROVIDER_COUNT; ++i) {
        provider_t *p = &g.providers[i];
        int ri;
        uint64_t id; epd_memory_owner_t subject, code;
        lock_state(); id = p->id; subject = p->spec.lifetime_owner; code = p->spec.code_owner; unlock_state();
        if (!id || (!within(subject, owner) && !within(code, owner))) continue;
        lock_state();
        if (p->id != id) { unlock_state(); continue; }
        p->stopped = true; p->auto_close = true;
        for (ri = 0; ri < RESOURCE_COUNT; ++ri)
            if (g.resources[ri].view.resource && g.resources[ri].provider == p->id) retire(&g.resources[ri]);
        queue_job(&p->drain, EPD_RESOURCE_WORKER);
        unlock_state();
    }
    lock_state();
    queue_job(&binding_cleanup, EPD_RESOURCE_WORKER);
    unlock_state(); wake(); return EPD_RESOURCE_ACCEPTED;
}
bool epd_resource_owner_busy(epd_memory_owner_t owner)
{
    int i;
    if (!metadata_initialized) return false;
    for (i = 0; i < BINDING_COUNT; ++i) {
        uint64_t id; epd_memory_owner_t subject;
        lock_state(); id = g.bindings[i].id; subject = g.bindings[i].owner; unlock_state();
        if (id && within(subject, owner)) return true;
    }
    for (i = 0; i < ACTIVITY_COUNT; ++i) {
        bool busy; epd_memory_owner_t subject;
        lock_state(); subject = g.activities[i].view.owner;
        busy = g.activities[i].view.activity &&
            (g.activities[i].owner_held || g.activities[i].busy || g.activities[i].notifying ||
             g.activities[i].work.running || g.activities[i].notice.running);
        unlock_state(); if (busy && within(subject, owner)) return true;
    }
    for (i = 0; i < PROVIDER_COUNT; ++i) {
        uint64_t id; epd_memory_owner_t subject, code;
        lock_state(); id = g.providers[i].id; subject = g.providers[i].spec.lifetime_owner;
        code = g.providers[i].spec.code_owner; unlock_state();
        if (id && (within(subject, owner) || within(code, owner))) return true;
    }
    return false;
}
static void process_bindings(void)
{
    int i;
    for (i = 0; i < BINDING_COUNT; ++i) {
        epd_memory_owner_t owner = 0;
        bool release = false;
        binding_t *b = &g.bindings[i];
        lock_state();
        if (b->id && b->stopped && !b->activities) { owner = b->owner; release = true; }
        unlock_state();
        if (release) {
            g.rt.owner_release(g.rt.context, owner);
            lock_state(); b->id = 0; unlock_state();
        }
    }
}
static void process_provider(int index)
{
    provider_t *p = &g.providers[index];
    epd_memory_owner_t owner;
    lock_state();
    if (!p->id || !p->auto_close || p->resources || p->callbacks) { unlock_state(); return; }
    owner = p->spec.code_owner; p->owner_held = false;
    unlock_state();
    g.rt.owner_release(g.rt.context, owner);
    if (p->lifetime_held) g.rt.owner_release(g.rt.context, p->spec.lifetime_owner);
    lock_state(); p->id = 0; --g.stats.providers; unlock_state();
}

static void process_activity(int index)
{
    activity_t *a = &g.activities[index];
    epd_resource_activity_snapshot_t request;
    epd_resource_grant_t grant = {0};
    epd_resource_credit_t discard = 0;
    size_t unused[EPD_MEMORY_BACKEND_COUNT];
    epd_memory_owner_t release_owner = 0, release_binding = 0;
    bool owner_done = false, binding_done = false;
    uint64_t now = g.rt.now(g.rt.context);
    int result;
    lock_state();
    if (a->busy || a->borrowers || a->executions) { unlock_state(); return; }
    if (ending(a)) {
        /* Pins retain payload/code, not an unused allocation reservation. */
        discard = a->view.credit; a->view.credit = 0;
        a->view.guaranteed_bytes = a->view.unused_bytes = 0;
        if (!a->view.references && a->view.state != EPD_RESOURCE_ENDED) {
            binding_t *b = binding_of(a->view.requested.binding);
            a->view.state = EPD_RESOURCE_ENDED;
            if (b && --b->activities == 0 && b->stopped) {
                release_binding = b->owner; b->id = 0; binding_done = true;
            }
            activity_notice(a);
        }
        if (a->view.state == EPD_RESOURCE_ENDED && a->owner_held &&
            !a->notice.queued && !a->notifying) {
            release_owner = a->view.owner; a->owner_held = false; owner_done = true;
        }
        unlock_state(); release_credit(discard);
        if (binding_done) g.rt.owner_release(g.rt.context, release_binding);
        if (owner_done) g.rt.owner_release(g.rt.context, release_owner);
        return;
    }
    if (!a->retry) { unlock_state(); return; }
    a->retry = false; a->busy = true; request = a->view;
    memcpy(unused, a->resume_unused, sizeof(unused));
    unlock_state();
    if (request.state == EPD_RESOURCE_PAUSING) {
        result = g.rt.activity_suspend(g.rt.context, request.credit);
        lock_state(); a->busy = false;
        if (ending(a) || a->view.requested_revision != request.requested_revision)
            queue_job(&a->work, EPD_RESOURCE_WORKER);
        else {
            if (result == EPD_RESOURCE_OK) {
                a->view.state = EPD_RESOURCE_PAUSED;
                a->view.guaranteed_bytes = a->view.unused_bytes = 0;
                a->view.effective.kind = EPD_RESOURCE_OPTIONAL;
                a->view.effective.minimum_bytes = a->view.effective.target_bytes = 0;
                memset(a->view.effective.companion_bytes, 0, sizeof(a->view.effective.companion_bytes));
            }
            a->view.reason = result;
            activity_notice(a);
        }
        unlock_state(); return;
    }
    if (request.requested.deadline && now >= request.requested.deadline) result = EPD_RESOURCE_DENIED;
    else if (request.state == EPD_RESOURCE_RESUMING)
        result = g.rt.activity_resume(g.rt.context, &request, unused, &grant);
    else result = g.rt.activity_apply(g.rt.context, &request, &grant);
    lock_state(); a->busy = false;
    if (ending(a) || a->view.requested_revision != request.requested_revision) {
        discard = grant.credit;
        queue_job(&a->work, EPD_RESOURCE_WORKER);
    } else if (request.state == EPD_RESOURCE_RESUMING) {
        discard = grant.credit;
        if (result == EPD_RESOURCE_OK) {
            if (grant.guaranteed_bytes < request.requested.minimum_bytes ||
                grant.unused_bytes > grant.guaranteed_bytes || grant.credit == request.credit)
                result = EPD_RESOURCE_INVALID;
            else result = g.rt.credit_resume_commit(g.rt.context, request.credit, grant.credit);
        }
        if (result == EPD_RESOURCE_OK) {
            a->view.effective = a->view.requested;
            a->view.effective_revision = a->view.requested_revision;
            a->view.guaranteed_bytes = grant.guaranteed_bytes;
            a->view.unused_bytes = grant.unused_bytes;
            a->view.state = EPD_RESOURCE_ADMITTED; a->view.reason = 0;
        } else {
            a->view.state = result == EPD_RESOURCE_PENDING ? EPD_RESOURCE_RESUMING : EPD_RESOURCE_PAUSED;
            a->view.reason = result;
        }
        activity_notice(a);
    } else if (result == EPD_RESOURCE_OK) {
        if (!grant.credit || grant.credit == a->view.credit ||
            grant.guaranteed_bytes < a->view.requested.minimum_bytes ||
            grant.unused_bytes > grant.guaranteed_bytes) {
            discard = grant.credit == a->view.credit ? 0 : grant.credit;
            a->view.state = EPD_RESOURCE_REJECTED; a->view.reason = EPD_RESOURCE_INVALID;
        } else {
            int committed = g.rt.credit_commit ? g.rt.credit_commit(g.rt.context, grant.credit) : EPD_RESOURCE_OK;
            if (committed != EPD_RESOURCE_OK) {
                discard = grant.credit; a->view.state = EPD_RESOURCE_REJECTED; a->view.reason = committed;
            } else {
                discard = a->view.credit; a->view.credit = grant.credit;
                a->view.effective = a->view.requested;
                a->view.effective_revision = a->view.requested_revision;
                a->view.guaranteed_bytes = grant.guaranteed_bytes;
                a->view.unused_bytes = grant.unused_bytes;
                a->view.state = EPD_RESOURCE_ADMITTED; a->view.reason = 0;
                refresh_activity_resources(a);
            }
        }
        activity_notice(a);
    } else {
        discard = grant.credit;
        a->view.state = result == EPD_RESOURCE_PENDING ? EPD_RESOURCE_WAITING : EPD_RESOURCE_REJECTED;
        a->view.reason = result;
        activity_notice(a);
    }
    unlock_state(); release_credit(discard);
}

static void process_activity_notice(int index)
{
    activity_t *a = &g.activities[index];
    epd_resource_activity_snapshot_t snapshot;
    epd_resource_observer_t observer;
    epd_memory_owner_t previous;
    lock_state();
    if (a->notifying) { unlock_state(); return; }
    a->notifying = true; snapshot = a->view; observer = a->observer;
    unlock_state();
    previous = g.rt.owner_enter(g.rt.context, snapshot.owner);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        observer.notify(observer.context, &snapshot);
        g.rt.owner_leave(g.rt.context, previous);
    }
    lock_state(); a->notifying = false;
    if (ending(a)) queue_job(&a->work, EPD_RESOURCE_WORKER);
    unlock_state();
}
static void process_reference_notice(int index)
{
    reference_t *ref = &g.references[index];
    epd_resource_notify_t notify;
    void *context;
    uint64_t id;
    int result;
    epd_memory_owner_t owner, previous;
    lock_state();
    if (ref->closing || ref->delivered || ref->notifying) { ref_finalize(ref); unlock_state(); return; }
    ref->notifying = true; ref->delivered = true;
    notify = ref->notify; context = ref->context; id = ref->id;
    result = g.resources[ref->resource].view.result;
    owner = g.activities[ref->activity].view.owner;
    unlock_state();
    previous = g.rt.owner_enter(g.rt.context, owner);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        notify(context, id, result);
        g.rt.owner_leave(g.rt.context, previous);
    }
    lock_state(); ref->notifying = false; ref_finalize(ref); unlock_state();
}

static void process_resource(int index, epd_resource_lane_t lane)
{
    resource_t *r = &g.resources[index];
    provider_t *p;
    epd_resource_provider_spec_t spec;
    epd_resource_descriptor_t descriptor;
    epd_resource_credit_t credits[ACTIVITY_COUNT], new_credit = 0, discard = 0;
    bool seen[ACTIVITY_COUNT] = {false};
    size_t credit_count = 0, bytes = 0, released = 0;
    uint64_t offer_revision = 0;
    epd_memory_owner_t previous;
    uint64_t id;
    void *data = NULL;
    enum { NOTHING, ADMIT, LOAD, CANCEL, DESTROY, DROP, SETTLE, SHRINK } action = NOTHING;
    int i, result = EPD_RESOURCE_OK;
    lock_state(); p = provider_of(r->provider);
    if (!p || r->busy) { unlock_state(); return; }
    spec = p->spec; id = r->view.resource; descriptor = r->view.descriptor;
    if (r->external) {
        if (lane != spec.destroy_lane) { resource_schedule(r); unlock_state(); return; }
        if (r->view.invalidated && !r->destroy_blocked && !r->external_waiting) {
            if (r->view.physical_bytes && r->view.descriptor.estimated_bytes) {
                action = SHRINK; r->external_waiting = true; offer_revision = r->offer_revision;
                bytes = r->pending_bytes ? r->pending_bytes : r->view.descriptor.estimated_bytes;
            } else if (!r->view.physical_bytes && p->stopped) action = DROP;
            else {
                /* The queued offer can disappear before the legal lane runs:
                 * a draw repinned it, or the component already freed it. No
                 * shrink is in flight here. Retire only this reclaim request;
                 * held data and a stopped provider's drain remain protected. */
                finish_reclaim(r, 0, false);
                r->evicted = false;
                r->view.invalidated = p->stopped;
                r->view.state = p->stopped ? EPD_RESOURCE_RETIRING : EPD_RESOURCE_READY;
                resource_refresh(r);
            }
        }
    } else if (r->completed && r->credit && !r->settled && g.rt.credit_loaded) {
        r->settled = true; action = SETTLE;
    } else if (r->view.state == EPD_RESOURCE_LOADING) {
        if (lane != spec.load_lane) { resource_schedule(r); unlock_state(); return; }
        if (r->view.invalidated) {
            if (!r->started) {
                r->view.state = EPD_RESOURCE_RETIRING;
                r->view.result = EPD_RESOURCE_CANCELLED;
                for (i = r->refs; i != NONE; i = g.references[i].next_resource)
                    if (g.references[i].notify && !g.references[i].closing && !g.references[i].delivered)
                        queue_job(&g.references[i].notice, g.references[i].lane);
                if (!r->view.references) action = DROP;
            }
            else if (!r->cancel_sent && spec.cancel) { r->cancel_sent = true; action = CANCEL; }
        } else if (!r->started && r->retry) {
            r->retry = false;
            for (i = r->refs; i != NONE; i = g.references[i].next_resource) {
                reference_t *ref = &g.references[i];
                activity_t *a = &g.activities[ref->activity];
                if (!ref->closing && a->view.credit && !a->busy && !ending(a) && !seen[ref->activity]) {
                    credits[credit_count++] = a->view.credit; seen[ref->activity] = true;
                }
            }
            if (credit_count) action = r->credit ? LOAD : ADMIT;
        }
    } else if (r->view.invalidated && !r->view.pins && !r->view.dirty &&
               !r->destroy_blocked && !r->view.references) {
        if (lane != spec.destroy_lane) { resource_schedule(r); unlock_state(); return; }
        action = r->data ? DESTROY : DROP;
        r->view.state = EPD_RESOURCE_DESTROYING;
        data = r->data; bytes = r->view.physical_bytes;
    } else if (r->view.state == EPD_RESOURCE_LOAD_FAILED && r->credit) {
        discard = r->credit; r->credit = 0;
    }
    /* Reclaim can evict soft, unpinned references; users must acquire again. */
    if (r->evicted && !r->view.pins && !r->view.dirty &&
        r->view.state != EPD_RESOURCE_LOADING && r->view.references) {
        int next;
        for (i = r->refs; i != NONE; i = next) {
            next = g.references[i].next_resource;
            if (!g.references[i].notice.queued && !g.references[i].notifying) ref_close(&g.references[i]);
        }
        if (!r->view.references) resource_schedule(r);
    }
    if (action != NOTHING) { ++r->busy; ++p->callbacks; candidate_remove(r); }
    if (action == ADMIT)
        for (i = 0; i < ACTIVITY_COUNT; ++i) if (seen[i]) ++g.activities[i].borrowers;
    if (action == LOAD) {
        r->started = true; ++g.stats.loads; feedback_of(r)->load_started = g.clock;
    }
    unlock_state(); release_credit(discard);
    if (action == NOTHING) return;
    previous = action == LOAD && g.rt.credit_enter ? g.rt.credit_enter(g.rt.context, r->credit) :
        g.rt.owner_enter(g.rt.context, spec.code_owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) result = EPD_RESOURCE_STOPPED;
    else if (action == ADMIT) result = g.rt.resource_admit(g.rt.context, id, &descriptor, credits, credit_count, &new_credit);
    else if (action == LOAD) result = spec.load(spec.context, id, &descriptor, r->credit);
    else if (action == CANCEL) spec.cancel(spec.context, id);
    else if (action == DESTROY) result = spec.destroy(spec.context, id, data, bytes);
    else if (action == SETTLE) g.rt.credit_loaded(g.rt.context, r->credit);
    else if (action == SHRINK) result = spec.shrink(spec.context, descriptor.backend, bytes, &released);
    if (previous != EPD_MEMORY_OWNER_INVALID) g.rt.owner_leave(g.rt.context, previous);
    discard = 0;
    lock_state(); --r->busy; --p->callbacks;
    if (action == SETTLE) resource_schedule(r);
    else if (action == ADMIT) {
        for (i = 0; i < ACTIVITY_COUNT; ++i) if (seen[i]) {
            activity_t *a = &g.activities[i];
            --a->borrowers;
            if (a->retry || ending(a)) queue_job(&a->work, EPD_RESOURCE_WORKER);
        }
    }
    if (action == SHRINK && result != EPD_RESOURCE_PENDING && r->external_waiting) {
        r->external_waiting = false;
        finish_reclaim(r, released, result != EPD_RESOURCE_OK);
        g.stats.reclaimed_bytes += released;
        if (r->offer_revision == offer_revision) {
            size_t removed = released < r->view.physical_bytes ? released : r->view.physical_bytes;
            r->view.physical_bytes -= removed; g.stats.physical_bytes -= removed;
            r->view.descriptor.estimated_bytes = 0;
        }
        r->evicted = false; r->view.invalidated = p->stopped;
        r->view.state = p->stopped ? EPD_RESOURCE_RETIRING : EPD_RESOURCE_READY;
        r->destroy_blocked = result != EPD_RESOURCE_OK;
        if (p->stopped && (!r->view.physical_bytes || (released && r->view.descriptor.estimated_bytes))) resource_schedule(r);
    } else if (action == ADMIT) {
        if (!r->view.invalidated && result == EPD_RESOURCE_OK && new_credit)
            result = association_publish(r, r->view.descriptor.estimated_bytes, new_credit);
        if (r->view.invalidated) { discard = new_credit; resource_schedule(r); }
        else if (result == EPD_RESOURCE_OK && new_credit) {
            r->credit = new_credit; r->retry = true; resource_schedule(r);
        } else if (result != EPD_RESOURCE_PENDING) {
            discard = new_credit;
            r->started = true;
            unlock_state();
            epd_resource_load_complete(id, result < 0 ? result : EPD_RESOURCE_INVALID, NULL, 0);
            release_credit(discard); return;
        } else discard = new_credit;
    } else if (action == LOAD) {
        if (!r->completed && result != EPD_RESOURCE_PENDING) {
            unlock_state();
            epd_resource_load_complete(id, result < 0 ? result : EPD_RESOURCE_INVALID, NULL, 0);
            return;
        }
        if (r->completed || r->view.invalidated) resource_schedule(r);
    } else if (action == CANCEL) {
        if (r->completed) resource_schedule(r);
    } else if (action == DROP || action == DESTROY) {
        if (result == EPD_RESOURCE_OK) {
            backend_t *b = &g.backends[r->backend];
            discard = r->credit; r->credit = 0;
            hash_remove(r); candidate_remove(r);
            g.stats.physical_bytes -= r->view.physical_bytes;
            g.stats.reclaimed_bytes += r->view.physical_bytes;
            --g.stats.resources; --p->resources;
            if (p->auto_close && !p->resources) queue_job(&p->drain, EPD_RESOURCE_WORKER);
            --b->resources; finish_reclaim(r, r->view.physical_bytes, false);
            unqueue_job(&r->work);
            r->view.resource = 0;
        } else {
            r->view.state = EPD_RESOURCE_RETIRING; r->view.result = result; r->destroy_blocked = true;
            finish_reclaim(r, 0, true);
        }
    }
    if (r->view.resource) resource_refresh(r);
    unlock_state(); release_credit(discard);
    if (action == DESTROY && result == EPD_RESOURCE_OK && g.rt.payload_released)
        g.rt.payload_released(g.rt.context, descriptor.backend, bytes);
    if (action == SHRINK && released && g.rt.payload_released)
        g.rt.payload_released(g.rt.context, descriptor.backend, released);
}

size_t epd_resource_process(epd_resource_lane_t lane, size_t budget)
{
    size_t processed = 0;
    if (!metadata_initialized || lane > EPD_RESOURCE_UI) return 0;
    lock_state();
    if (g.pumping[lane]) { unlock_state(); return 0; }
    g.pumping[lane] = true;
    unlock_state();
    while (processed < budget) {
        job_t *j;
        unsigned type;
        int index;
        lock_state(); j = g.head[lane];
        if (!j || j->running) { unlock_state(); break; }
        g.head[lane] = j->next;
        if (!g.head[lane]) g.tail[lane] = NULL;
        j->queued = false; j->running = true; j->next = NULL; type = j->type; index = j->index;
        unlock_state();
        if (type == JOB_ACTIVITY) process_activity(index);
        else if (type == JOB_ACTIVITY_NOTICE) process_activity_notice(index);
        else if (type == JOB_RESOURCE) process_resource(index, lane);
        else if (type == JOB_REFERENCE_NOTICE) process_reference_notice(index);
        else if (type == JOB_PROVIDER) process_provider(index);
        else process_bindings();
        lock_state(); j->running = false;
        if (type == JOB_REFERENCE_NOTICE) {
            reference_t *ref = &g.references[index];
            if (ref->id) {
                resource_t *r = &g.resources[ref->resource];
                ref_finalize(ref);
                if (r->view.invalidated) resource_schedule(r);
            }
        }
        if (type == JOB_ACTIVITY || type == JOB_ACTIVITY_NOTICE) {
            activity_t *a = &g.activities[index];
            if (a->auto_close && a->view.state == EPD_RESOURCE_ENDED && !a->owner_held &&
                !a->busy && !a->notifying && !a->work.queued && !a->notice.queued &&
                !a->work.running && !a->notice.running) {
                a->view.activity = 0; --g.stats.activities;
            }
        }
        unlock_state();
        ++processed;
    }
    lock_state(); g.pumping[lane] = false; unlock_state(); wake(); return processed;
}

RTM_EXPORT(epd_resource_activity_begin);
RTM_EXPORT(epd_resource_activity_update);
RTM_EXPORT(epd_resource_activity_get);
RTM_EXPORT(epd_resource_activity_pause);
RTM_EXPORT(epd_resource_activity_end);
RTM_EXPORT(epd_resource_activity_forget);
RTM_EXPORT(epd_resource_activity_dispose);
RTM_EXPORT(epd_resource_provider_register);
RTM_EXPORT(epd_resource_provider_unregister);
RTM_EXPORT(epd_resource_provider_cache_update);
RTM_EXPORT(epd_resource_provider_cache_complete);
RTM_EXPORT(epd_resource_acquire);
RTM_EXPORT(epd_resource_adopt);
RTM_EXPORT(epd_resource_take);
RTM_EXPORT(epd_resource_pin);
RTM_EXPORT(epd_resource_unpin);
RTM_EXPORT(epd_resource_release);
RTM_EXPORT(epd_resource_get);
RTM_EXPORT(epd_resource_ref_get);
RTM_EXPORT(epd_resource_load_complete);
RTM_EXPORT(epd_resource_invalidate);
RTM_EXPORT(epd_resource_mark_dirty);
RTM_EXPORT(epd_resource_saved);
RTM_EXPORT(epd_resource_set_local_value);
