/* Legacy codec/filesystem tests; the memory suite links the real runtime. */
typedef unsigned epd_memory_backend_t;
typedef struct { size_t physical; } epd_memory_block_snapshot_t;
typedef struct { unsigned provider, owner; } epd_resource_cache_t;
#define EPD_RESOURCE_WORKER 0
#define EPD_RESOURCE_INVALID (-1)
#define EPD_MEMORY_OK 0
static int epd_resource_cache_open(epd_resource_cache_t *c,int lane,
    int (*shrink)(void *,uint32_t,size_t,size_t *),void *ctx)
{ (void)lane;(void)shrink;(void)ctx;c->provider=1;return 0; }
static int epd_resource_cache_close(epd_resource_cache_t *c) { c->provider=0;return 0; }
static int epd_resource_cache_update_region(epd_resource_cache_t *c,int region,size_t total,size_t idle,unsigned cost,unsigned value)
{ (void)c;(void)region;(void)cost;(void)value;CHECK(idle<=total);return 0; }
static bool epd_memory_app_backend(int region,epd_memory_backend_t *out) { *out=(unsigned)region;return true; }
static int epd_memory_block_get(const void *p,epd_memory_block_snapshot_t *b)
{ if(!p)return -1;b->physical=((const size_t *)p)[-1]+sizeof(size_t);return 0; }
