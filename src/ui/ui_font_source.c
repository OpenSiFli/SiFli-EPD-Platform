#include "ui_font_source.h"
#include "ui_font_pack.h"
#include "epd_memory.h"
#include "lvgl.h"
#include <rtthread.h>
#include <string.h>

extern const unsigned char ui_builtin_font_pack[];
extern const int ui_builtin_font_pack_size;
extern const uint32_t ui_builtin_font_size;
extern const uint32_t ui_builtin_emoji_size;

static ui_font_pack_t *font_pack;
static struct rt_mutex font_lock;
static lv_fs_drv_t font_driver;

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
    rt_mutex_release(&font_lock);
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
    ui_font_pack_t *pack = epd_app_alloc(sizeof(*pack), EPD_APP_PSRAM);
    if (!pack) return false;
    if (ui_builtin_font_pack_size <= 0 ||
        !ui_font_pack_init(pack, ui_builtin_font_pack, (size_t)ui_builtin_font_pack_size) ||
        (uint64_t)pack->length != (uint64_t)ui_builtin_font_size + ui_builtin_emoji_size)
    {
        epd_app_free(pack);
        return false;
    }
    const uint32_t face_sizes[] = {ui_builtin_font_size, ui_builtin_emoji_size};
    size_t box_bytes = ui_font_pack_boxes_size(pack, face_sizes, 2);
    void *boxes = box_bytes && box_bytes <= 384u * 1024u ? epd_app_alloc(box_bytes, EPD_APP_PSRAM) : NULL;
    if (boxes && !ui_font_pack_boxes_init(pack, boxes, box_bytes, face_sizes, 2))
    {
        epd_app_free(boxes);
        boxes = NULL;
    }
    if (!ui_font_pack_validate(pack) ||
        rt_mutex_init(&font_lock, "font_io", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        epd_app_free(boxes);
        epd_app_free(pack);
        return false;
    }
    rt_kprintf("font source: glyph_boxes=%u index_bytes=%u block_cache_bytes=%u\n",
               pack->box_count, boxes ? (unsigned)box_bytes : 0,
               (unsigned)(UI_FONT_BLOCK_SIZE * UI_FONT_CACHE_BLOCKS));
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
    return true;
}
