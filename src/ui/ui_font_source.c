#include "ui_font_resource.h"
#include "ui_font_source.h"
#include "ui_font_pack.h"
#include "ui_font_memory.h"
#include "epd_memory.h"
#include "epd_shared_service.h"
#include "lvgl.h"
#include <rtthread.h>
#include <string.h>


static ui_font_pack_t *font_pack;
static struct rt_mutex font_lock;
static lv_fs_drv_t font_driver;
static bool font_lock_ready;
static epd_resource_cache_t source_resource;
static size_t source_physical, source_published = SIZE_MAX, source_published_free = SIZE_MAX;

static void source_publish(void)
{
    size_t reclaimable = font_pack ? font_pack->cache_bytes : 0;
    if (!source_resource.provider || (source_published == source_physical &&
        source_published_free == reclaimable)) return;
    if (epd_resource_cache_update_region(&source_resource, EPD_MEMORY_REGION_HCPU_PSRAM,
            source_physical, reclaimable, 1, 0) == EPD_RESOURCE_OK)
    {
        source_published = source_physical;
        source_published_free = reclaimable;
    }
}

static void source_unlock(void)
{
    source_publish();
    rt_mutex_release(&font_lock);
}

static int source_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)context;
    epd_memory_backend_t expected;
    *released = 0;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &expected) || backend != expected)
        return EPD_RESOURCE_INVALID;
    rt_mutex_take(&font_lock, RT_WAITING_FOREVER);
    if (font_pack) *released = ui_font_pack_reclaim(font_pack, requested);
    source_unlock();
    return EPD_RESOURCE_OK;
}

static void *source_allocate(size_t bytes, size_t *charge, void *context)
{
    (void)context;
    epd_memory_recovery_t recovery = {0};
    epd_memory_block_snapshot_t block;
    *charge = 0;
    if (!ui_font_memory_begin(&recovery, epd_app_allocation_peak(bytes))) return NULL;
    void *memory = epd_app_alloc(bytes, EPD_APP_PSRAM);
    ui_font_memory_result(&recovery, memory != NULL);
    if (!memory) return NULL;
    if (epd_memory_block_get(memory, &block) != EPD_MEMORY_OK ||
        ui_font_memory_cache_adopt(&source_resource, memory) != EPD_RESOURCE_OK)
    {
        epd_app_free(memory);
        return NULL;
    }
    *charge = block.physical;
    source_physical += block.physical;
    return memory;
}

static void source_release(void *memory, size_t charge, void *context)
{
    (void)context;
    if (!memory) return;
    epd_app_free(memory);
    source_physical -= charge;
}

static const ui_font_pack_allocator_t source_allocator = {source_allocate, source_release, NULL};

typedef struct { uint32_t position, base, length; } font_file_t;

static void *font_open(lv_fs_drv_t *driver, const char *path, lv_fs_mode_t mode)
{
    (void)driver;
    if (mode != LV_FS_MODE_RD) return NULL;
    if (*path == '/') ++path;
    uint32_t base, length;
    if (!strcmp(path, "MiSans.ttf")) { base = 0; length = ui_builtin_font_size; }
    else if (!strcmp(path, "Emoji.ttf"))
    { base = ui_builtin_font_size; length = ui_builtin_emoji_size; }
    else return NULL;
    font_file_t *file = lv_malloc_zeroed(sizeof(*file));
    if (file) { file->base = base; file->length = length; }
    return file;
}

static lv_fs_res_t font_close(lv_fs_drv_t *driver, void *file)
{
    (void)driver;
    lv_free(file);
    return LV_FS_RES_OK;
}

static lv_fs_res_t font_read(lv_fs_drv_t *driver, void *file, void *buffer,
                             uint32_t count, uint32_t *read_count)
{
    (void)driver;
    font_file_t *cursor = file;
    rt_mutex_take(&font_lock, RT_WAITING_FOREVER);
    if (count > cursor->length - cursor->position) count = cursor->length - cursor->position;
    bool ok = ui_font_pack_read(font_pack, cursor->base + cursor->position, buffer, count, read_count);
    cursor->position += *read_count;
    source_unlock();
    return ok ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_seek(lv_fs_drv_t *driver, void *file, uint32_t position,
                             lv_fs_whence_t origin)
{
    (void)driver;
    font_file_t *cursor = file;
    uint64_t next;
    switch (origin)
    {
    case LV_FS_SEEK_SET: next = position; break;
    case LV_FS_SEEK_CUR: next = (uint64_t)cursor->position + position; break;
    case LV_FS_SEEK_END:
        if (position > cursor->length) return LV_FS_RES_INV_PARAM;
        next = cursor->length - position;
        break;
    default: return LV_FS_RES_INV_PARAM;
    }
    if (next > cursor->length) return LV_FS_RES_INV_PARAM;
    cursor->position = (uint32_t)next;
    return LV_FS_RES_OK;
}

static lv_fs_res_t font_tell(lv_fs_drv_t *driver, void *file, uint32_t *position)
{
    (void)driver;
    *position = ((font_file_t *)file)->position;
    return LV_FS_RES_OK;
}

bool ui_font_source_init(void)
{
    if (font_pack) return true;
    if (lv_fs_get_drv('F')) return false;
    /* Initialization is UI-owned. Only IPC/provider lifetime enters SYSTEM;
     * allocations retain the producer's admission until cache adoption. */
    epd_shared_service_guard_t guard = {0};
    if (!epd_shared_service_enter(&guard)) return false;
    if (!font_lock_ready)
        font_lock_ready = rt_mutex_init(&font_lock, "font_io", RT_IPC_FLAG_PRIO) == RT_EOK;
    int opened = font_lock_ready ? epd_resource_cache_open(&source_resource,
        EPD_RESOURCE_WORKER, source_shrink, NULL) : EPD_RESOURCE_STOPPED;
    epd_shared_service_leave(&guard);
    /* A caller before resource-runtime startup can retry without resident data
     * or a filesystem driver escaping unregistered. */
    if (opened != EPD_RESOURCE_OK) return false;
    rt_mutex_take(&font_lock, RT_WAITING_FOREVER);
    size_t pack_charge = 0, box_charge = 0;
    ui_font_pack_t *pack = source_allocate(sizeof(*pack), &pack_charge, NULL);
    if (!pack) { source_unlock(); return false; }
    if (!system_font_header_valid(ui_font_resource()) || ui_builtin_font_pack_size <= 0 ||
        !ui_font_pack_init(pack, ui_builtin_font_pack, (size_t)ui_builtin_font_pack_size, &source_allocator) ||
        (uint64_t)pack->length != (uint64_t)ui_builtin_font_size + ui_builtin_emoji_size)
    {
        source_release(pack, pack_charge, NULL);
        source_unlock();
        return false;
    }
    const uint32_t face_sizes[] = {ui_builtin_font_size, ui_builtin_emoji_size};
    size_t box_bytes = ui_font_pack_boxes_size(pack, face_sizes, 2);
    void *boxes = box_bytes && box_bytes <= 384u * 1024u ? source_allocate(box_bytes, &box_charge, NULL) : NULL;
    if (boxes && !ui_font_pack_boxes_init(pack, boxes, box_bytes, face_sizes, 2))
    {
        source_release(boxes, box_charge, NULL);
        boxes = NULL;
    }
    if (!ui_font_pack_validate(pack))
    {
        ui_font_pack_reclaim(pack, SIZE_MAX);
        source_release(boxes, box_charge, NULL);
        source_release(pack, pack_charge, NULL);
        source_unlock();
        return false;
    }
    rt_kprintf("font source: glyph_boxes=%u index_bytes=%u block_cache_bytes=%u\n",
               pack->box_count, boxes ? (unsigned)box_bytes : 0,
               (unsigned)(pack->block_bytes * UI_FONT_CACHE_BLOCKS));
    font_pack = pack;
    lv_fs_drv_init(&font_driver);
    font_driver.letter = 'F';
    /* Font instances share the block cache; no second per-file cache. */
    font_driver.cache_size = 0;
    font_driver.open_cb = font_open;
    font_driver.close_cb = font_close;
    font_driver.read_cb = font_read;
    font_driver.seek_cb = font_seek;
    font_driver.tell_cb = font_tell;
    lv_fs_drv_register(&font_driver);
    source_unlock();
    return true;
}
