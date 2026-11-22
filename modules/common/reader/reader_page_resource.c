#include "reader_page_resource.h"
#include "../../../src/platform/epd_resource_runtime.h"
#include <rtthread.h>
#include <string.h>
#include <limits.h>


static int load_page(void *context, epd_resource_t resource,
                     const epd_resource_descriptor_t *descriptor, epd_resource_credit_t credit)
{
    (void)context; (void)resource; (void)descriptor; (void)credit;
    /* Layout owns reconstruction; completed layout pages enter through adopt. */
    return EPD_RESOURCE_FAILED;
}
static int destroy_page(void *context, epd_resource_t resource, void *data, size_t bytes)
{
    reader_cached_page_t *page = data;
    void (*dispose)(void *) = page->dispose;
    (void)context; (void)resource; (void)bytes;
    if (page->cache) reader_page_cache_unlink(page->cache, page);
    dispose(page);
    return EPD_RESOURCE_OK;
}
static void unborrow(reader_page_cache_t *cache)
{
    if (cache->borrowed) {
        epd_resource_unpin(cache->borrowed->reference);
        cache->borrowed = NULL;
    }
}
static bool borrow_page(reader_page_cache_t *cache, reader_cached_page_t *page)
{
    void *data;
    if (cache->borrowed == page) return true;
    unborrow(cache);
    if (epd_resource_pin(page->reference, &data) != EPD_RESOURCE_OK || data != page) return false;
    cache->borrowed = page;
    return true;
}
static bool retire_page(reader_page_cache_t *cache, reader_cached_page_t *page)
{
    void *data;
    if (cache->borrowed == page) unborrow(cache);
    if (epd_resource_take(page->reference, &data) == EPD_RESOURCE_OK) {
        page->reference = page->resource = 0;
        return data == page;
    }
    epd_resource_invalidate(page->resource);
    epd_resource_release(page->reference);
    return false;
}
static void rank_page(reader_page_cache_t *cache, reader_cached_page_t *page, unsigned distance)
{
    (void)cache;
    epd_resource_set_local_value(page->resource, UINT32_MAX - distance);
}
static void close_cache(reader_page_cache_t *cache)
{
    unborrow(cache);
    if (cache->resource_activity) epd_resource_activity_dispose(cache->resource_activity);
    if (cache->resource_parent) epd_resource_activity_dispose(cache->resource_parent);
    if (cache->resource_provider) epd_resource_provider_unregister(cache->resource_provider);
    if (cache->resource_scope) {
        epd_memory_owner_close(cache->resource_scope);
        epd_memory_owner_release(cache->resource_scope);
    }
    cache->resource_activity = cache->resource_parent = cache->resource_binding = cache->resource_provider = 0;
    cache->resource_scope = 0;
}
static bool start_cache(reader_page_cache_t *cache, epd_memory_backend_t backend)
{
    epd_memory_owner_t owner = epd_memory_owner_current();
    epd_resource_provider_spec_t provider = {0};
    epd_resource_context_request_t activity = {0};
    int result;
    if (cache->resource_scope) return true;
    cache->resource_scope = epd_memory_scope_create(owner, EPD_MEMORY_SCOPE_PAGE);
    if (cache->resource_scope == EPD_MEMORY_OWNER_INVALID) { cache->resource_scope = 0; return false; }
    provider.code_owner = owner; provider.implementation_version = 1;
    provider.lifetime_owner = cache->resource_scope;
    provider.load_lane = EPD_RESOURCE_UI; provider.destroy_lane = EPD_RESOURCE_UI;
    provider.load = load_page; provider.destroy = destroy_page;
    result = epd_resource_provider_register(&provider, &cache->resource_provider);
    if (result != EPD_RESOURCE_OK) { close_cache(cache); return false; }
    activity.backend = backend;
    activity.kind = EPD_RESOURCE_INTERACTIVE;
    result = epd_resource_context_begin(&activity, NULL, &cache->resource_parent);
    if (result == EPD_RESOURCE_ACCEPTED) {
        activity.kind = EPD_RESOURCE_NEARBY; activity.interactive_parent = cache->resource_parent;
        result = epd_resource_context_begin(&activity, NULL, &cache->resource_activity);
    } else if (result == EPD_RESOURCE_DENIED) {
        activity.kind = EPD_RESOURCE_OPTIONAL;
        result = epd_resource_context_begin(&activity, NULL, &cache->resource_activity);
    }
    if (result != EPD_RESOURCE_ACCEPTED) { close_cache(cache); return false; }
    return true;
}
static bool publish_page(reader_page_cache_t *cache, reader_cached_page_t *page)
{
    epd_memory_backend_t backend;
    epd_memory_owner_t previous;
    epd_resource_descriptor_t descriptor = {0};
    uint32_t key[6];
    unsigned distance = page->number > cache->wanted ? page->number - cache->wanted : cache->wanted - page->number;
    int result;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend) || !start_cache(cache, backend)) return false;
    /* Provider identity separates document instances; layout and cursor cover geometry/content. */
    key[0] = page->number; key[1] = page->layout;
    key[2] = page->start.record; key[3] = page->start.byte;
    key[4] = page->end.record; key[5] = page->end.byte;
    memcpy(descriptor.key.bytes, key, sizeof(key)); descriptor.key.length = sizeof(key);
    descriptor.key.version = page->layout; descriptor.backend = backend;
    descriptor.estimated_bytes = page->allocation; descriptor.rebuild_cost = 1;
    descriptor.local_value = UINT32_MAX - distance; descriptor.discardable = true;
    {
        epd_resource_activity_snapshot_t activity;
        if (epd_resource_activity_get(cache->resource_activity, &activity) != EPD_RESOURCE_OK) return false;
        previous = epd_memory_owner_enter(activity.owner);
    }
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    result = epd_resource_adopt(cache->resource_activity, cache->resource_provider, &descriptor,
        page, page->allocation, &page->reference);
    epd_memory_owner_leave(previous);
    if (result != EPD_RESOURCE_ACCEPTED) return false;
    return epd_resource_ref_get(page->reference, &page->resource) == EPD_RESOURCE_OK;
}
void reader_page_resource_attach(reader_page_cache_t *cache)
{
    cache->publish = publish_page; cache->retire = retire_page; cache->borrow = borrow_page;
    cache->rank = rank_page; cache->close = close_cache;
}
