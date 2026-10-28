#include "storage_file_internal.h"
#include "app_image.h"
#include "storage.h"
#include "storage_file.h"
#include "bf0_hal.h"
#include "src/draw/lv_image_decoder_private.h"
#include <dfs_posix.h>
#include <rtm.h>
#include <string.h>

typedef struct
{
    lv_image_decoder_dsc_t decoder;
    lv_image_dsc_t image;
    char path[256];
} app_image_t;

static void image_close(app_image_t *resource)
{
    if (!resource->decoder.decoded) return;
    /* Finish GPU reads before the SDK decoder frees its compressed buffer. */
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&resource->image);
    lv_image_header_cache_drop(&resource->image);
    lv_image_header_cache_drop(resource->path);
    lv_image_decoder_close(&resource->decoder);
    memset(&resource->decoder, 0, sizeof(resource->decoder));
    resource->path[0] = 0;
}

static void image_deleted(lv_event_t *event)
{
    app_image_t *resource = lv_event_get_user_data(event);
    image_close(resource);
    lv_free(resource);
}

static app_image_t *image_resource(lv_obj_t *image)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(image); ++i)
    {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(image, i);
        if (lv_event_dsc_get_cb(event) == image_deleted)
            return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

void epd_app_image_release(lv_obj_t *image)
{
    if (!image) return;
    app_image_t *resource = image_resource(image);
    if (!resource || !resource->decoder.decoded) return;
    lv_image_set_src(image, NULL);
    image_close(resource);
}
RTM_EXPORT(epd_app_image_release);

bool epd_app_image_set(lv_obj_t *image, const char *path, const lv_image_dsc_t *fallback)
{
    if (!image) return false;
    app_image_t *resource = image_resource(image);
    if (path && resource && resource->decoder.decoded && !strcmp(resource->path, path)) return true;
    if (!path && (!resource || !resource->decoder.decoded) && lv_image_get_src(image) == fallback) return true;
    epd_app_image_release(image);
    if (!path) { lv_image_set_src(image, fallback); return true; }
    bool ok = false;
    if ((strncmp(path, STORAGE_FLASH_APPS "/", sizeof(STORAGE_FLASH_APPS "/") - 1) &&
         strncmp(path, STORAGE_SD_APPS "/", sizeof(STORAGE_SD_APPS "/") - 1)) ||
        strlen(path) >= sizeof(((app_image_t *)0)->path)) goto finish;
    if (!resource)
    {
        resource = lv_malloc_zeroed(sizeof(*resource));
        if (!resource) goto finish;
        if (!lv_obj_add_event_cb(image, image_deleted, LV_EVENT_DELETE, resource))
        { lv_free(resource); goto finish; }
    }
    storage_lock();
    struct stat st;
    lv_image_header_t header;
    /* Bound compressed allocations and reject non-EZIP files before opening. */
    ok = storage_path_available(path) && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_size > sizeof(header) && st.st_size <= 256 * 1024 &&
         lv_image_decoder_get_info(path, &header) == LV_RESULT_OK &&
         (header.flags & LV_IMAGE_FLAGS_EZIP) && header.w && header.h &&
         header.w <= 2048 && header.h <= 2048;
    if (ok)
    {
        strcpy(resource->path, path);
        lv_image_decoder_args_t args = { .no_cache = true };
        ok = lv_image_decoder_open(&resource->decoder, resource->path, &args) == LV_RESULT_OK;
        if (ok && !resource->decoder.decoded)
        { lv_image_decoder_close(&resource->decoder); ok = false; }
        if (!ok) memset(&resource->decoder, 0, sizeof(resource->decoder));
    }
    storage_unlock();
    if (ok)
    {
        const lv_draw_buf_t *buffer = resource->decoder.decoded;
        memset(&resource->image, 0, sizeof(resource->image));
        resource->image.header = buffer->header;
        resource->image.header.magic = LV_IMAGE_HEADER_MAGIC;
        resource->image.header.flags &= ~LV_IMAGE_FLAGS_ALLOCATED;
        resource->image.data = buffer->data;
        resource->image.data_size = buffer->data_size;
#ifdef PSRAM_CACHE_WB
        /* Compressed byte count, not pixel stride times height. */
        mpu_dcache_clean(buffer->data, buffer->data_size);
#endif
        lv_image_set_src(image, &resource->image);
        return true;
    }
finish:
    if (path) lv_image_header_cache_drop(path);
    lv_image_set_src(image, fallback);
    return false;
}
RTM_EXPORT(epd_app_image_set);
