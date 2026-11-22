/* Real resource manager and LVGL cache; only platform ownership/scheduling is replaced. */
struct rt_memheap { unsigned unused; } app_image_psram_memheap;
static __declspec(thread) unsigned resource_lock_depth;
static CRITICAL_SECTION resource_mutex;
static __declspec(thread) epd_memory_owner_t resource_owner = EPD_MEMORY_OWNER_SYSTEM;
static void manager_lock(void *ctx) { (void)ctx; EnterCriticalSection(&resource_mutex); assert(!resource_lock_depth++); }
static void manager_unlock(void *ctx) { (void)ctx; assert(resource_lock_depth-- == 1); LeaveCriticalSection(&resource_mutex); }
static epd_memory_owner_t manager_current(void *ctx) { (void)ctx; return resource_owner; }
epd_memory_owner_t epd_memory_owner_current(void) { return resource_owner; }
static int manager_retain(void *ctx, epd_memory_owner_t owner) { (void)ctx; (void)owner; return 0; }
static void manager_release(void *ctx, epd_memory_owner_t owner) { (void)ctx; (void)owner; }
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{ return owner == resource_owner ? resource_owner : EPD_MEMORY_OWNER_INVALID; }
void epd_memory_owner_leave(epd_memory_owner_t owner) { resource_owner = owner; }
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{ epd_memory_owner_t old = resource_owner; resource_owner = owner; return old; }
void epd_memory_owner_leave_trusted(epd_memory_owner_t owner) { resource_owner = owner; }
static epd_memory_owner_t manager_enter(void *ctx, epd_memory_owner_t owner)
{ (void)ctx; assert(!resource_lock_depth); return epd_memory_owner_enter_trusted(owner); }
static void manager_leave(void *ctx, epd_memory_owner_t owner) { (void)ctx; epd_memory_owner_leave(owner); }
static uint64_t manager_now(void *ctx) { (void)ctx; return 1; }
static void *manager_control(void *ctx,size_t bytes)
{ (void)ctx;assert(!resource_lock_depth);return calloc(1,bytes); }
static void manager_wake(void *ctx, epd_resource_lane_t lane) { (void)ctx; (void)lane; assert(!resource_lock_depth); }
static int manager_apply(void *ctx, const epd_resource_activity_snapshot_t *s, epd_resource_grant_t *out)
{ (void)ctx; (void)s; (void)out; return EPD_RESOURCE_DENIED; }
static int manager_admit(void *ctx, epd_resource_t r, const epd_resource_descriptor_t *d,
    const epd_resource_credit_t *credits, size_t count, epd_resource_credit_t *out)
{ (void)ctx; (void)r; (void)d; (void)credits; (void)count; (void)out; return EPD_RESOURCE_DENIED; }
static void manager_credit_release(void *ctx, epd_resource_credit_t credit) { (void)ctx; (void)credit; }
int epd_resource_cache_open(epd_resource_cache_t *cache, epd_resource_lane_t lane,
    int (*shrink)(void *, uint32_t, size_t, size_t *), void *context)
{
    epd_resource_provider_spec_t spec = {0};
    if (cache->provider) return 0;
    spec.code_owner = EPD_MEMORY_OWNER_SYSTEM; spec.implementation_version = 1;
    cache->owner = EPD_MEMORY_OWNER_SYSTEM;
    spec.load_lane = spec.destroy_lane = lane; spec.shrink = shrink; spec.context = context;
    return epd_resource_provider_register(&spec, &cache->provider);
}
int epd_resource_cache_update(epd_resource_cache_t *cache, const void *allocator,
    size_t total, size_t free_bytes, uint32_t cost, uint32_t local)
{ assert(allocator == &app_image_psram_memheap); return epd_resource_provider_cache_update(cache->provider, 1, total, free_bytes, cost, local); }
int epd_resource_cache_update_region(epd_resource_cache_t *cache, epd_memory_region_t region,
    size_t total, size_t free_bytes, uint32_t cost, uint32_t local)
{ assert(region == EPD_MEMORY_REGION_HCPU_PSRAM); return epd_resource_provider_cache_update(cache->provider, 1, total, free_bytes, cost, local); }
int epd_resource_cache_close(epd_resource_cache_t *cache) {
    int result = epd_resource_provider_unregister(cache->provider);
    if (result == EPD_RESOURCE_OK || result == EPD_RESOURCE_STALE) cache->provider = 0;
    return result;
}
int epd_resource_cache_adopt(epd_resource_cache_t *cache, void *allocation)
{ assert(cache->provider && allocation);
  return resource_owner == cache->owner ? EPD_RESOURCE_OK : EPD_RESOURCE_INVALID; }
static void manager_setup(void)
{
    InitializeCriticalSection(&resource_mutex);
    epd_resource_runtime_t rt = {0};
    rt.control_allocate = manager_control;
    rt.lock = manager_lock; rt.unlock = manager_unlock; rt.current_owner = manager_current;
    rt.owner_retain = manager_retain; rt.owner_release = manager_release;
    rt.owner_enter = manager_enter; rt.owner_leave = manager_leave; rt.now = manager_now; rt.wake = manager_wake;
    rt.activity_apply = manager_apply; rt.resource_admit = manager_admit; rt.credit_release = manager_credit_release;
    assert(epd_resource_init(&rt) == 0);
}
