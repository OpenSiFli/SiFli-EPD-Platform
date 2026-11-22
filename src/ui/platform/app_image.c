#include "storage_file_internal.h"
#include "app_image.h"
#include "storage.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "bf0_hal.h"
#include "src/draw/lv_image_decoder_private.h"
#include <dfs_posix.h>
#include <rtm.h>
#include <string.h>

typedef struct shared_image {
    struct shared_image *next;
    epd_resource_t resource;
    uint64_t key;
    lv_image_dsc_t image;
    uint32_t revision, volume_revision, session;
    size_t source_bytes, allocation;
    time_t modified;
    char path[256];
    uint8_t payload[];
} shared_image_t;
typedef struct {
    shared_image_t *shared;
    epd_resource_ref_t reference;
    epd_resource_activity_t activity;
} app_image_t;
static shared_image_t *images;
static epd_resource_provider_t image_provider;
static epd_memory_owner_t system_image_owner;
static uint64_t image_serial;
extern struct rt_memheap app_image_psram_memheap;

static int image_load(void *context, epd_resource_t resource,
    const epd_resource_descriptor_t *descriptor, epd_resource_credit_t credit)
{
    (void)context; (void)resource; (void)descriptor; (void)credit;
    return EPD_RESOURCE_INVALID;
}
static int image_destroy(void *context, epd_resource_t resource, void *data, size_t bytes)
{
    (void)context; (void)resource; (void)bytes;
    shared_image_t *item = data, **at = &images;
    while (*at && *at != item) at = &(*at)->next;
    if (*at) *at = item->next;
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&item->image); lv_image_header_cache_drop(&item->image);
    epd_app_free(item); return EPD_RESOURCE_OK;
}
static bool image_begin(app_image_t *item, epd_memory_backend_t backend)
{
    if (!image_provider) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
        if (previous == EPD_MEMORY_OWNER_INVALID) return false;
        epd_resource_provider_spec_t spec = {0};
        spec.code_owner = EPD_MEMORY_OWNER_SYSTEM; spec.implementation_version = 1;
        spec.load_lane = spec.destroy_lane = EPD_RESOURCE_UI;
        spec.load = image_load; spec.destroy = image_destroy;
        int result = epd_resource_provider_register(&spec, &image_provider);
        epd_memory_owner_leave_trusted(previous);
        if (result != EPD_RESOURCE_OK) return false;
    }
    epd_resource_context_request_t request = {0};
    request.backend = backend;
    if (epd_memory_owner_current() == EPD_MEMORY_OWNER_SYSTEM) {
        /* Shell widgets have no dynamic app page. Bind their references to a
         * system service with optional authority; widget deletion releases them. */
        if (!system_image_owner) {
            epd_memory_owner_t owner = epd_memory_scope_create_trusted(EPD_MEMORY_OWNER_SYSTEM,
                EPD_MEMORY_SCOPE_SERVICE);
            if (owner == EPD_MEMORY_OWNER_INVALID) return false;
            if (epd_resource_runtime_service_state(owner, owner, false) != EPD_RESOURCE_OK) {
                epd_resource_runtime_close_owner(owner);
                epd_memory_owner_release_trusted(owner);
                return false;
            }
            system_image_owner = owner;
        }
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(system_image_owner);
        if (previous == EPD_MEMORY_OWNER_INVALID) return false;
        request.kind = EPD_RESOURCE_OPTIONAL;
        int result = epd_resource_context_begin(&request, NULL, &item->activity);
        epd_memory_owner_leave_trusted(previous);
        return result == EPD_RESOURCE_ACCEPTED;
    }
    request.kind = EPD_RESOURCE_INTERACTIVE;
    int result = epd_resource_context_begin(&request, NULL, &item->activity);
    if (result == EPD_RESOURCE_DENIED) {
        request.kind = EPD_RESOURCE_OPTIONAL;
        result = epd_resource_context_begin(&request, NULL, &item->activity);
    }
    return result == EPD_RESOURCE_ACCEPTED;
}
static void image_close(app_image_t *item)
{
    if (item->reference) {
        lv_draw_wait_for_finish();
        epd_resource_unpin(item->reference); epd_resource_release(item->reference);
    }
    if (item->activity) epd_resource_activity_dispose(item->activity);
    memset(item, 0, sizeof(*item));
}
static void image_deleted(lv_event_t *event)
{
    app_image_t *item = lv_event_get_user_data(event);
    image_close(item); lv_free(item);
}
static app_image_t *image_resource(lv_obj_t *image)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(image); ++i) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(image, i);
        if (lv_event_dsc_get_cb(event) == image_deleted) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}
void epd_app_image_release(lv_obj_t *image)
{
    if (!image) return;
    app_image_t *item = image_resource(image);
    if (!item) return;
    lv_image_set_src(image, NULL); image_close(item);
}
RTM_EXPORT(epd_app_image_release);

bool epd_app_image_set(lv_obj_t *image, const char *path, const lv_image_dsc_t *fallback)
{
    if (!image) return false;
    app_image_t *item = image_resource(image);
    if (!path && (!item || !item->shared) && lv_image_get_src(image) == fallback) return true;
    if (!path) { epd_app_image_release(image); lv_image_set_src(image, fallback); return true; }
    epd_memory_backend_t backend;
    if ((strncmp(path, STORAGE_FLASH_APPS "/", sizeof(STORAGE_FLASH_APPS "/") - 1) &&
         strncmp(path, STORAGE_SD_APPS "/", sizeof(STORAGE_SD_APPS "/") - 1)) ||
        strlen(path) >= sizeof(((shared_image_t *)0)->path) ||
        !epd_memory_backend_find(&app_image_psram_memheap, &backend)) goto failed;
    storage_volume_t volume = !strncmp(path, STORAGE_SD_APPS "/", sizeof(STORAGE_SD_APPS "/") - 1) ? STORAGE_SD : STORAGE_FLASH;
    storage_lock();
    struct stat st;
    bool valid = storage_path_available(path) && !storage_file_writers(path) && stat(path, &st) == 0 &&
        S_ISREG(st.st_mode) && st.st_size > sizeof(lv_image_header_t) && st.st_size <= 256 * 1024;
    uint32_t revision = storage_path_revision(path), volume_revision = storage_volume_revision(volume), session = storage_card_session();
    storage_unlock();
    if (!valid) goto failed;
    shared_image_t *shared = NULL;
    for (shared_image_t *p = images; p; p = p->next) {
        epd_resource_snapshot_t snapshot;
        if (p->revision == revision && p->volume_revision == volume_revision && p->session == session &&
            p->source_bytes == (size_t)st.st_size && p->modified == st.st_mtime && !strcmp(p->path, path) &&
            epd_resource_get(p->resource, &snapshot) == EPD_RESOURCE_OK && !snapshot.invalidated) { shared = p; break; }
    }
    if (item && item->shared == shared && shared) return true;
    epd_app_image_release(image);
    if (!item) {
        item = lv_malloc_zeroed(sizeof(*item));
        if (!item) goto failed;
        if (!lv_obj_add_event_cb(image, image_deleted, LV_EVENT_DELETE, item)) { lv_free(item); item = NULL; goto failed; }
    }
    if (!image_begin(item, backend)) goto failed;
    bool fresh = shared == NULL;
    if (fresh) {
        lv_image_decoder_dsc_t decoder = {0}; lv_image_header_t header;
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
        if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
        storage_lock();
        valid = storage_path_available(path) && storage_session_valid(volume, session) &&
            revision == storage_path_revision(path) && volume_revision == storage_volume_revision(volume) &&
            !storage_file_writers(path) && lv_image_decoder_get_info(path, &header) == LV_RESULT_OK &&
            (header.flags & LV_IMAGE_FLAGS_EZIP) && header.w && header.h && header.w <= 2048 && header.h <= 2048;
        lv_image_decoder_args_t args = {.no_cache = true};
        if (valid) valid = lv_image_decoder_open(&decoder, path, &args) == LV_RESULT_OK;
        if (valid && decoder.decoded && decoder.decoded->data && decoder.decoded->data_size <= 256u * 1024u) {
            const lv_draw_buf_t *buffer = decoder.decoded;
            size_t bytes = sizeof(*shared) + 31u + buffer->data_size;
            shared = epd_app_alloc(bytes, EPD_APP_PSRAM);
            if (shared) {
                memset(shared, 0, sizeof(*shared)); shared->allocation = bytes;
                shared->image.header = buffer->header; shared->image.header.magic = LV_IMAGE_HEADER_MAGIC;
                shared->image.header.flags &= ~LV_IMAGE_FLAGS_ALLOCATED;
                uint8_t *pixels = (uint8_t *)(((uintptr_t)shared->payload + 31u) & ~(uintptr_t)31u);
                memcpy(pixels, buffer->data, buffer->data_size);
                shared->image.data = pixels; shared->image.data_size = buffer->data_size;
                strcpy(shared->path, path); shared->source_bytes = (size_t)st.st_size; shared->modified = st.st_mtime;
                shared->revision = revision; shared->volume_revision = volume_revision; shared->session = session;
                shared->key = ++image_serial;
#ifdef PSRAM_CACHE_WB
                mpu_dcache_clean(pixels, buffer->data_size);
#endif
            }
        }
        if (decoder.decoder) lv_image_decoder_close(&decoder);
        storage_unlock(); epd_memory_owner_leave_trusted(previous);
        if (!shared) goto failed;
    }
    epd_resource_descriptor_t descriptor = {0};
    memcpy(descriptor.key.bytes, &shared->key, sizeof(shared->key)); descriptor.key.length = sizeof(shared->key);
    descriptor.key.version = shared->revision; descriptor.backend = backend;
    descriptor.estimated_bytes = shared->allocation; descriptor.discardable = true; descriptor.rebuild_cost = 3;
    epd_resource_activity_snapshot_t activity;
    if (epd_resource_activity_get(item->activity, &activity) != EPD_RESOURCE_OK) { if (fresh) epd_app_free(shared); goto failed; }
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(activity.owner);
    int result = previous == EPD_MEMORY_OWNER_INVALID ? EPD_RESOURCE_STOPPED : fresh ?
        epd_resource_adopt(item->activity, image_provider, &descriptor, shared, shared->allocation, &item->reference) :
        epd_resource_acquire(item->activity, image_provider, &descriptor, NULL, NULL, EPD_RESOURCE_UI, &item->reference);
    if (previous != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_leave_trusted(previous);
    if (result != EPD_RESOURCE_ACCEPTED) { if (fresh) epd_app_free(shared); goto failed; }
    if (fresh) {
        epd_resource_ref_get(item->reference, &shared->resource);
        shared->next = images; images = shared;
    }
    void *data = NULL;
    if (epd_resource_pin(item->reference, &data) != EPD_RESOURCE_OK) goto failed;
    item->shared = data; lv_image_set_src(image, &item->shared->image); return true;
failed:
    if (item) { lv_image_set_src(image, NULL); image_close(item); }
    lv_image_header_cache_drop(path); lv_image_set_src(image, fallback); return false;
}
RTM_EXPORT(epd_app_image_set);
