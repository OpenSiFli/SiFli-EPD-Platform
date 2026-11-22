/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_RESOURCE_FEEDBACK_H
#define EPD_RESOURCE_FEEDBACK_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Advisory history, owned by the resource lock. Four probes per event, no
 * payload access. Provider generations isolate unloaded/replaced code. */
#define EPD_RESOURCE_FEEDBACK_SLOTS 32u
#define EPD_RESOURCE_FEEDBACK_WINDOW_MS 30000u
typedef struct {
    uint64_t provider, key, reclaimed_at, touched_at;
    uint32_t cost, hits;
    bool pending;
} epd_resource_feedback_record_t;
typedef struct {
    epd_resource_feedback_record_t records[EPD_RESOURCE_FEEDBACK_SLOTS];
    uint64_t rebuilds, rebuild_cost;
} epd_resource_feedback_t;

static inline epd_resource_feedback_record_t *epd_resource_feedback_find(
    epd_resource_feedback_t *history, uint64_t provider, uint64_t key, uint64_t now, bool create)
{
    unsigned first = (unsigned)((provider ^ key) % (EPD_RESOURCE_FEEDBACK_SLOTS / 4u)) * 4u;
    epd_resource_feedback_record_t *oldest = &history->records[first];
    for (unsigned i = first; i < first + 4u; ++i) {
        epd_resource_feedback_record_t *r = &history->records[i];
        if (r->provider == provider && r->key == key) return r;
        if (!r->provider || r->touched_at < oldest->touched_at) oldest = r;
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->provider = provider; oldest->key = key; oldest->touched_at = now;
    return oldest;
}
static inline void epd_resource_feedback_reclaimed(epd_resource_feedback_t *history,
    uint64_t provider, uint64_t key, uint64_t now, uint32_t cost)
{
    epd_resource_feedback_record_t *r = epd_resource_feedback_find(history, provider, key, now, true);
    if (now - r->touched_at > EPD_RESOURCE_FEEDBACK_WINDOW_MS) r->hits = 0;
    r->pending = true; r->reclaimed_at = r->touched_at = now; r->cost = cost;
}
static inline void epd_resource_feedback_rebuilt(epd_resource_feedback_t *history,
    uint64_t provider, uint64_t key, uint64_t now)
{
    epd_resource_feedback_record_t *r = epd_resource_feedback_find(history, provider, key, now, false);
    if (!r || !r->pending) return;
    r->pending = false;
    if (now - r->reclaimed_at > EPD_RESOURCE_FEEDBACK_WINDOW_MS) { r->hits = 0; return; }
    if (r->hits < 4u) ++r->hits;
    r->touched_at = now;
    ++history->rebuilds; history->rebuild_cost += r->cost;
}
static inline uint32_t epd_resource_feedback_weight(epd_resource_feedback_t *history,
    uint64_t provider, uint64_t key, uint64_t now)
{
    epd_resource_feedback_record_t *r = epd_resource_feedback_find(history, provider, key, now, false);
    if (!r || now - r->touched_at > EPD_RESOURCE_FEEDBACK_WINDOW_MS) return 0;
    return r->hits * (1u + (r->cost > 65535u ? 65535u : r->cost));
}
#endif
