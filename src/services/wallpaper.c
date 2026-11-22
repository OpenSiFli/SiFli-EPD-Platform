#include "storage_internal.h"
#include "wallpaper.h"
#include "../ui/platform/epd_gray_quantize.h"
#include "app_service_internal.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "epd_activity_work.h"
#include "epd_resource_runtime.h"
#include "icons/ui_icons.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "../../modules/common/image/raster.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include "wallpaper_builtin.inc"

#define WALLPAPER_PATH STORAGE_FLASH_ROOT "/system/wallpaper.bin"
#define WALLPAPER_SERVICE "wallpap"
#define FOLDER_MAGIC "EPD-WPF1\n"
_Static_assert(sizeof(WALLPAPER_SERVICE) <= RT_NAME_MAX, "wallpaper service name exceeds RT_NAME_MAX");

typedef enum { JOB_LOAD, JOB_IMPORT, JOB_DEFAULT, JOB_FOLDER, JOB_ROTATE } job_kind_t;
/* A zlib stream contains this header followed by packed 16-level pixels. */
typedef struct { uint32_t custom, width, height; } wallpaper_record_t;
typedef struct
{
    uint8_t *compressed;
    size_t size;
} wallpaper_image_t;
typedef struct { lv_image_dsc_t image; void *allocation; } wallpaper_bitmap_t;
typedef struct { char path[STORAGE_PATH_MAX], current[256]; } wallpaper_folder_t;
L2_RET_BSS_SECT_BEGIN(wallpaper_state)
static wallpaper_status_t state L2_RET_BSS_SECT(wallpaper_state);
static wallpaper_image_t *selected L2_RET_BSS_SECT(wallpaper_state);
static wallpaper_folder_t *rotation L2_RET_BSS_SECT(wallpaper_state);
static bool rotation_ready L2_RET_BSS_SECT(wallpaper_state);
static bool rotation_attempted L2_RET_BSS_SECT(wallpaper_state);
static uint32_t rotation_session L2_RET_BSS_SECT(wallpaper_state);
static unsigned display_width L2_RET_BSS_SECT(wallpaper_state);
static unsigned display_height L2_RET_BSS_SECT(wallpaper_state);
L2_RET_BSS_SECT_END
static bool wallpaper_initialized;
static struct
{
    job_kind_t kind;
    char path[STORAGE_PATH_MAX];
    uint32_t session;
    bool card, accepted, success;
    rt_err_t error;
    wallpaper_image_t *image;
    wallpaper_folder_t *folder;
    epd_activity_work_t *work;
    epd_service_t *service;
    epd_memory_owner_t image_owner;
} job;

static void image_wake(void *unused)
{
    (void)unused;
    rt_base_t level = rt_hw_interrupt_disable();
    if (job.service) epd_service_wake(job.service);
    rt_hw_interrupt_enable(level);
}

static void *image_alloc(size_t bytes)
{
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM,
        epd_app_allocation_peak(bytes)) != EPD_RESOURCE_OK) return NULL;
    return epd_app_alloc(bytes, EPD_APP_PSRAM);
}

static void image_work_close(void)
{
    epd_activity_work_close(job.work);
    job.work = NULL;
    if (job.image_owner != EPD_MEMORY_OWNER_INVALID) {
        epd_resource_runtime_close_owner(job.image_owner);
        epd_memory_owner_close_trusted(job.image_owner);
        epd_memory_owner_release_trusted(job.image_owner);
        job.image_owner = EPD_MEMORY_OWNER_INVALID;
    }
}

static bool image_work_open(void)
{
    /* Resident settings have no dynamic-module PAGE. The accepted UI import
     * owns this foreground binding until its final file access has ended. */
    job.image_owner = epd_memory_scope_create_trusted(epd_memory_owner_current(), EPD_MEMORY_SCOPE_PAGE);
    if (job.image_owner == EPD_MEMORY_OWNER_INVALID) return false;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(job.image_owner);
    if (previous != EPD_MEMORY_OWNER_INVALID) {
        if (epd_resource_runtime_page_state(job.image_owner, job.image_owner, true) == EPD_RESOURCE_OK)
            job.work = epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE, 0,
                epd_app_allocation_peak(4096), epd_resource_file_io_peak(STORAGE_PATH_MAX), image_wake, NULL);
        epd_memory_owner_leave_trusted(previous);
    }
    if (job.work) return true;
    image_work_close();
    return false;
}

static void image_release(wallpaper_image_t *image)
{
    if (!image) return;
    epd_app_free(image->compressed);
    epd_app_free(image);
}

static bool cancelled(void *context)
{
    return epd_service_cancelled(context) || !system_reset_write_allowed() ||
        (job.card && (!storage_session_valid(STORAGE_SD, job.session) || storage_release_requested()));
}

static bool wallpaper_path_valid(const char *path)
{
    if (!path || !*path || strlen(path) >= STORAGE_PATH_MAX) return false;
    return !strcmp(path, STORAGE_FLASH_ROOT) || !strcmp(path, STORAGE_SD_ROOT) ||
        !strncmp(path, STORAGE_FLASH_ROOT "/", sizeof(STORAGE_FLASH_ROOT)) ||
        !strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT));
}

static size_t record_size(void)
{
    return sizeof(wallpaper_record_t) + ((size_t)display_width * display_height + 1u) / 2u;
}

static bool dimensions_valid(int32_t width, int32_t height)
{
    /* LVGL v9 stores width, height and byte stride in 16-bit fields, and
     * data_size in 32 bits. Include row/alignment padding in allocation bounds. */
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) return false;
    uint64_t stride = ((uint64_t)width * 2u + 3u) & ~UINT64_C(3);
    uint64_t bytes = stride * (uint32_t)height;
    uint64_t packed = sizeof(wallpaper_record_t) + ((uint64_t)width * height + 1u) / 2u;
    return stride <= UINT16_MAX && bytes <= UINT32_MAX && bytes <= SIZE_MAX - 31u &&
           packed <= UINT32_MAX && packed <= SIZE_MAX;
}

static void *codec_alloc(void *context, size_t count, size_t size)
{
    (void)context;
    return size && count <= SIZE_MAX / size ? image_alloc(count * size) : NULL;
}

static void codec_free(void *context, void *memory)
{ (void)context; epd_app_free(memory); }

static bool record_valid(const wallpaper_record_t *record, size_t size)
{
    return size >= sizeof(*record) &&
        ((!record->custom && !record->width && !record->height && size == sizeof(*record)) ||
         (record->custom == 1 && record->width == display_width && record->height == display_height &&
          size == record_size()));
}

static wallpaper_image_t *compress_record(const wallpaper_record_t *record, size_t size)
{
    wallpaper_image_t *image = image_alloc(sizeof(*image));
    if (!image) return NULL;
    memset(image, 0, sizeof(*image));
    size_t capacity = mz_compressBound((mz_ulong)size);
    uint8_t *temporary = image_alloc(capacity);
    mz_stream stream = {0};
    stream.zalloc = codec_alloc;
    stream.zfree = codec_free;
    bool ok = temporary && mz_deflateInit(&stream, MZ_BEST_COMPRESSION) == MZ_OK;
    if (ok)
    {
        stream.next_in = (const unsigned char *)record;
        stream.avail_in = (unsigned)size;
        stream.next_out = temporary;
        stream.avail_out = (unsigned)capacity;
        ok = mz_deflate(&stream, MZ_FINISH) == MZ_STREAM_END;
        image->size = stream.total_out;
        mz_deflateEnd(&stream);
    }
    if (ok) image->compressed = image_alloc(image->size);
    if (image->compressed) memcpy(image->compressed, temporary, image->size);
    epd_app_free(temporary);
    if (!image->compressed) { image_release(image); return NULL; }
    return image;
}

static bool inflate_image(const uint8_t *input, size_t input_size, void *output,
                          size_t capacity, size_t *size)
{
    mz_stream stream = {0};
    stream.zalloc = codec_alloc;
    stream.zfree = codec_free;
    if (mz_inflateInit(&stream) != MZ_OK) return false;
    stream.next_in = input;
    stream.avail_in = (unsigned)input_size;
    stream.next_out = output;
    stream.avail_out = (unsigned)capacity;
    bool ok = mz_inflate(&stream, MZ_FINISH) == MZ_STREAM_END && !stream.avail_in;
    *size = stream.total_out;
    mz_inflateEnd(&stream);
    return ok;
}

static bool decompress_record(const wallpaper_image_t *image, wallpaper_record_t *record,
                              size_t capacity, size_t *size)
{
    return inflate_image(image->compressed, image->size, record, capacity, size) && record_valid(record, *size);
}

/* Decode only for a visible wallpaper view. The SDK consumes A8 masks directly;
 * its A4 image decoder would allocate another A8 buffer. Black coverage over
 * the white view represents the sixteen gray levels without an RGB copy. */
static wallpaper_bitmap_t *bitmap_create(const wallpaper_image_t *image)
{
    wallpaper_bitmap_t *bitmap = epd_app_alloc(sizeof(*bitmap), EPD_APP_PSRAM);
    if (!bitmap) return NULL;
    memset(bitmap, 0, sizeof(*bitmap));
    unsigned width = image ? display_width : DEFAULT_WALLPAPER_WIDTH;
    unsigned height = image ? display_height : DEFAULT_WALLPAPER_HEIGHT;
    unsigned stride = RT_ALIGN(width, 4);
    size_t bytes = (size_t)stride * height;
    bitmap->allocation = epd_app_alloc(bytes + sizeof(wallpaper_record_t) + 31u, EPD_APP_PSRAM);
    if (!bitmap->allocation) { epd_app_free(bitmap); return NULL; }
    uint8_t *data = (uint8_t *)RT_ALIGN((uintptr_t)bitmap->allocation + sizeof(wallpaper_record_t), 32);
    wallpaper_record_t *record = (wallpaper_record_t *)(data - sizeof(*record));
    size_t size;
    size_t packed = (size_t)(DEFAULT_WALLPAPER_WIDTH / 8u) * DEFAULT_WALLPAPER_HEIGHT;
    bool decoded = image ? decompress_record(image, record, record_size(), &size) && record->custom == 1 :
        inflate_image(default_wallpaper_data, sizeof(default_wallpaper_data), data, packed, &size) && size == packed;
    if (!decoded)
    { epd_app_free(bitmap->allocation); epd_app_free(bitmap); return NULL; }
    /* Expand backwards so the original packed source stays intact,
     * including odd widths where the next row begins in the low nibble. */
    for (unsigned y = height; y-- > 0;)
    {
        uint8_t *row = data + (size_t)y * stride;
        for (unsigned x = width; x-- > 0;)
        {
            size_t index = (size_t)y * width + x;
            if (image)
            {
                unsigned gray = (data[index / 2] >> (index % 2 ? 0 : 4)) & 15u;
                row[x] = (15u - gray) * 17u;
            }
            else row[x] = data[index / 8u] & (0x80u >> (index % 8u)) ? 0u : 255u;
        }
        memset(row + width, 0, stride - width);
    }
    bitmap->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    bitmap->image.header.cf = LV_COLOR_FORMAT_A8;
    bitmap->image.header.w = width;
    bitmap->image.header.h = height;
    bitmap->image.header.stride = stride;
    bitmap->image.data = data;
    bitmap->image.data_size = bytes;
    lv_draw_buf_t buffer = {.data = data, .data_size = bytes};
    epd_app_clean_draw_buffer(&buffer);
    return bitmap;
}

static void load_image(epd_service_t *service)
{
    struct stat st;
    wallpaper_image_t *image = epd_app_alloc(sizeof(*image), EPD_APP_PSRAM);
    if (!image) { job.error = -RT_ENOMEM; return; }
    memset(image, 0, sizeof(*image));
    storage_lock();
    bool recovered = storage_file_recover(WALLPAPER_PATH);
    int fd = recovered ? open(WALLPAPER_PATH, O_RDONLY) : -1;
    bool missing = recovered && fd < 0 && errno == -ENOENT;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              (uint64_t)st.st_size <= mz_compressBound((mz_ulong)record_size());
    if (ok)
    {
        image->size = st.st_size;
        image->compressed = epd_app_alloc(image->size, EPD_APP_PSRAM);
        ok = image->compressed != NULL;
        if (!ok) job.error = -RT_ENOMEM;
        size_t offset = 0;
        while (ok && offset < image->size && !cancelled(service))
        {
            int count = read(fd, image->compressed + offset, image->size - offset);
            if (count <= 0) ok = false;
            else offset += (size_t)count;
        }
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    storage_unlock();
    if (!ok || cancelled(service))
    { image_release(image); job.success = missing && !cancelled(service); return; }
    const size_t marker = sizeof(FOLDER_MAGIC) - 1;
    if (image->size > marker && !memcmp(image->compressed, FOLDER_MAGIC, marker))
    {
        const char *path = (const char *)image->compressed + marker;
        size_t length = image->size - marker;
        if (memchr(path, 0, length) == path + length - 1u && wallpaper_path_valid(path))
        {
            job.folder = epd_app_alloc(sizeof(*job.folder), EPD_APP_PSRAM);
            if (job.folder)
            {
                strcpy(job.folder->path, path);
                job.folder->current[0] = 0;
                job.success = true;
            }
            else job.error = -RT_ENOMEM;
        }
        image_release(image);
        return;
    }
    wallpaper_record_t *record = epd_app_alloc(record_size(), EPD_APP_PSRAM);
    if (!record) { image_release(image); job.error = -RT_ENOMEM; return; }
    size_t size;
    ok = decompress_record(image, record, record_size(), &size);
    if (!ok || !record->custom) { image_release(image); image = NULL; }
    job.image = image;
    job.success = ok && !cancelled(service);
    epd_app_free(record);
}

static unsigned crop_sample(unsigned pixel, unsigned output_size, unsigned source_size,
                            uint64_t extent)
{
    int64_t position = (((uint64_t)source_size << 16) - extent) / 2u +
                       (2u * pixel + 1u) * extent / (2u * output_size);
    position -= 32768;
    if (position < 0) position = 0;
    if ((uint64_t)position > ((uint64_t)(source_size - 1u) << 16))
        position = (uint64_t)(source_size - 1u) << 16;
    return (unsigned)(position >> 8);
}

static void import_image(epd_service_t *service)
{
    rt_tick_t start = rt_tick_get(), timeout = rt_tick_from_millisecond(500);
    int ready = -1;
    while (!cancelled(service) && (ready = epd_activity_work_ready(job.work)) == 0) {
        rt_tick_t elapsed = rt_tick_get() - start;
        if (elapsed >= timeout) { job.error = -RT_ENOMEM; return; }
        epd_service_wait(service, (rt_int32_t)(timeout - elapsed));
    }
    if (cancelled(service)) return;
    epd_activity_guard_t guard;
    if (ready < 0 || !epd_activity_work_enter(job.work, &guard)) { job.error = -RT_ENOMEM; return; }
    raster_decoder_t decoder = {0};
    decoder.fd = -1;
    decoder.path = job.path;
    decoder.context = service;
    decoder.cancel = cancelled;
    decoder.max_width = display_width;
    decoder.max_height = display_height;
    decoder.background = true;
    decoder.cover = true;
    wallpaper_record_t *record = NULL;
    if (cancelled(service) || !raster_io_open(&decoder) || !raster_decode(&decoder)) goto done;
    record = image_alloc(record_size());
    if (!record) { job.error = -RT_ENOMEM; goto done; }
    *record = (wallpaper_record_t){1, display_width, display_height};
    uint8_t *packed = (uint8_t *)(record + 1);
    memset(packed, 0xff, record_size() - sizeof(*record));
    uint64_t crop_width = (uint64_t)decoder.width << 16;
    uint64_t crop_height = (uint64_t)decoder.height << 16;
    if ((uint64_t)decoder.width * display_height > (uint64_t)decoder.height * display_width)
        crop_width = crop_height * display_width / display_height;
    else crop_height = crop_width * display_height / display_width;
    for (unsigned y = 0; y < display_height; ++y)
    {
        if (cancelled(service)) goto done;
        unsigned fy = crop_sample(y, display_height, decoder.height, crop_height);
        unsigned sy = fy >> 8, ay = fy & 255u;
        unsigned ny = sy + 1 < decoder.height ? sy + 1 : sy;
        for (unsigned x = 0; x < display_width; ++x)
        {
            unsigned fx = crop_sample(x, display_width, decoder.width, crop_width);
            unsigned sx = fx >> 8, ax = fx & 255u;
            unsigned nx = sx + 1 < decoder.width ? sx + 1 : sx;
            const uint8_t *a = decoder.gray + sy * decoder.width;
            const uint8_t *b = decoder.gray + ny * decoder.width;
            unsigned upper = a[sx] * (256 - ax) + a[nx] * ax;
            unsigned lower = b[sx] * (256 - ax) + b[nx] * ax;
            unsigned gray = ((upper * (256 - ay) + lower * ay + 32768u) >> 16);
            unsigned level = epd_gray4_dither(gray, x, y);
            size_t index = (size_t)y * display_width + x;
            unsigned shift = index % 2u ? 0 : 4;
            packed[index / 2u] = (packed[index / 2u] & ~(15u << shift)) | (level << shift);
        }
    }
    /* Packing is complete; close the input before opening the replacement file. */
    raster_decoder_clear(&decoder);
    job.image = compress_record(record, record_size());
    if (!job.image) { job.error = -RT_ENOMEM; goto done; }
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_SRAM,
        epd_resource_file_io_peak(STORAGE_PATH_MAX)) != EPD_RESOURCE_OK) {
        job.error = -RT_ENOMEM; goto done;
    }
    if (!cancelled(service))
    {
        if (job.kind == JOB_FOLDER)
        {
            char selection[sizeof(FOLDER_MAGIC) + STORAGE_PATH_MAX];
            int length = snprintf(selection, sizeof(selection), FOLDER_MAGIC "%s", job.folder->path);
            job.success = length > 0 && (size_t)length < sizeof(selection) &&
                storage_file_replace(WALLPAPER_PATH, selection, (size_t)length + 1u);
        }
        else job.success = job.kind == JOB_ROTATE ||
            storage_file_replace(WALLPAPER_PATH, job.image->compressed, job.image->size);
    }
done:
    epd_app_free(record);
    raster_decoder_clear(&decoder);
    if (!job.success) { image_release(job.image); job.image = NULL; }
    epd_activity_work_leave(&guard);
}

static bool folder_next(epd_service_t *service)
{
    wallpaper_folder_t *folder = job.folder;
    char first[sizeof(folder->current)] = {0}, next[sizeof(folder->current)] = {0};
    storage_lock();
    DIR *dir = !cancelled(service) ? opendir(folder->path) : NULL;
    storage_unlock();
    if (!dir) return false;
    while (!cancelled(service))
    {
        storage_lock();
        struct dirent *entry = !cancelled(service) ? readdir(dir) : NULL;
        if (!entry) { storage_unlock(); break; }
        const char *name = entry->d_name;
        const char *extension = strrchr(name, '.');
        bool image = name[0] != '.' && extension &&
            (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"));
        if (image && strlen(name) < sizeof(first))
        {
            int length = snprintf(job.path, sizeof(job.path), "%s/%s", folder->path, name);
            struct stat st;
            if (length > 0 && (size_t)length < sizeof(job.path) && stat(job.path, &st) == 0 && S_ISREG(st.st_mode))
            {
                if (!first[0] || strcmp(name, first) < 0) strcpy(first, name);
                if (strcmp(name, folder->current) > 0 && (!next[0] || strcmp(name, next) < 0)) strcpy(next, name);
            }
        }
        storage_unlock();
    }
    storage_lock();
    int close_result = closedir(dir);
    storage_unlock();
    if (close_result || cancelled(service) || !first[0]) return false;
    strcpy(folder->current, next[0] ? next : first);
    snprintf(job.path, sizeof(job.path), "%s/%s", folder->path, folder->current);
    return true;
}

static void worker(epd_service_t *service)
{
    /* Startup does no I/O until the UI has acquired the product sleep hold. */
    uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
    if (!(events & EPD_SERVICE_WAKE) || !job.accepted || cancelled(service)) goto done;
    if (job.kind == JOB_LOAD) load_image(service);
    else if (job.kind == JOB_IMPORT) import_image(service);
    else if (job.kind == JOB_FOLDER || job.kind == JOB_ROTATE)
    {
        char first[sizeof(job.folder->current)] = {0};
        while (folder_next(service) && strcmp(first, job.folder->current))
        {
            if (!first[0]) strcpy(first, job.folder->current);
            import_image(service);
            if (job.success || job.error == -RT_ENOMEM || cancelled(service)) break;
        }
    }
    else
    {
        const wallpaper_record_t builtin = {0};
        wallpaper_image_t *image = compress_record(&builtin, sizeof(builtin));
        if (image)
            job.success = storage_file_replace(WALLPAPER_PATH, image->compressed, image->size);
        else job.error = -RT_ENOMEM;
        image_release(image);
    }
done:
    {
        rt_base_t level = rt_hw_interrupt_disable();
        job.service = NULL;
        rt_hw_interrupt_enable(level);
    }
    image_work_close();
}

static bool publish(epd_service_t *service, bool stopping)
{
    if (!stopping) return false;
    epd_service_ui_watch(service, EPD_SERVICE_UI_DISPLAY);
    if (!epd_app_refresh_done()) return false;
    if (job.success)
    {
        wallpaper_image_t *old = selected;
        selected = job.image;
        job.image = NULL;
        image_release(old);
        epd_app_free(rotation);
        rotation = job.folder;
        job.folder = NULL;
        state.rotation = rotation != NULL;
        rotation_ready = rotation && selected;
        rotation_attempted = rotation_ready;
        state.custom = selected != NULL;
        state.ready = true;
    }
    else if (job.kind == JOB_LOAD) state.ready = true;
    state.error = job.success ? 0 : job.error;
    state.busy = false;
    ++state.revision;
    app_service_ui_signal(EPD_SERVICE_UI_WALLPAPER);
    image_release(job.image);
    job.image = NULL;
    epd_app_free(job.folder);
    job.folder = NULL;
    epd_service_sleep_release(service);
    return true;
}

static bool flush(void) { return !state.busy; }

static const epd_background_t background = {
    /* Miniz builds its dynamic Huffman tables on the worker stack. */
    .run = worker, .stack_size = 16384, .priority = 25, .ui_process = publish, .flush = flush
};

static rt_err_t start(job_kind_t kind, const char *path)
{
    if (state.busy || app_service_running(WALLPAPER_SERVICE) || storage_changing() || system_reset_blocks_apps())
        return -RT_EBUSY;
    memset(&job, 0, sizeof(job));
    job.image_owner = EPD_MEMORY_OWNER_INVALID;
    job.kind = kind;
    job.error = -RT_EIO;
    if (path)
    {
        if (!wallpaper_path_valid(path)) return -RT_EINVAL;
        strcpy(job.path, path);
        job.card = !strcmp(storage_path_root(path), STORAGE_SD_ROOT);
        job.session = storage_card_session();
    }
    if (kind == JOB_FOLDER || kind == JOB_ROTATE)
    {
        job.folder = epd_app_alloc(sizeof(*job.folder), EPD_APP_PSRAM);
        if (!job.folder) return -RT_ENOMEM;
        if (kind == JOB_ROTATE) *job.folder = *rotation;
        else
        {
            strcpy(job.folder->path, path);
            job.folder->current[0] = 0;
        }
    }
    if ((kind == JOB_IMPORT || kind == JOB_FOLDER || kind == JOB_ROTATE) && !image_work_open())
    { epd_app_free(job.folder); job.folder = NULL; return -RT_ENOMEM; }
    state.busy = true;
    state.error = 0;
    epd_service_t *service = app_service_start(WALLPAPER_SERVICE, &background, NULL);
    if (!service) {
        image_work_close();
        epd_app_free(job.folder);
        job.folder = NULL;
        state.busy = false; return -RT_ENOMEM;
    }
    job.service = service;
    if (!epd_service_sleep_acquire(service))
    {
        job.error = -RT_EBUSY;
        app_service_stop(WALLPAPER_SERVICE);
        return -RT_EBUSY;
    }
    job.accepted = true;
    epd_service_wake(service);
    return RT_EOK;
}

bool wallpaper_init(void)
{
    if (!wallpaper_initialized)
    {
        memset(&state, 0, sizeof(state));
        selected = NULL;
        rotation = NULL;
        rotation_ready = rotation_attempted = false;
        rotation_session = 0;
        wallpaper_initialized = true;
    }
    if (state.ready || state.busy) return true;
    lv_display_t *display = lv_display_get_default();
    if (!display) return false;
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    if (!dimensions_valid(width, height)) return false;
    display_width = (unsigned)width;
    display_height = (unsigned)height;
    return start(JOB_LOAD, NULL) == RT_EOK;
}

void wallpaper_get_status(wallpaper_status_t *out)
{
    if (out) *out = wallpaper_initialized ? state : (wallpaper_status_t){0};
}

rt_err_t wallpaper_import(const char *path)
{
    if (!path || !*path) return -RT_EINVAL;
    if (!state.ready) { wallpaper_init(); return -RT_EBUSY; }
    return start(JOB_IMPORT, path);
}

rt_err_t wallpaper_import_folder(const char *path)
{
    if (!wallpaper_path_valid(path)) return -RT_EINVAL;
    if (!state.ready) { wallpaper_init(); return -RT_EBUSY; }
    return start(JOB_FOLDER, path);
}

void wallpaper_rotation_prepare(void)
{
    if (!wallpaper_initialized || !rotation || rotation_ready || state.busy ||
        !storage_path_available(rotation->path)) return;
    uint32_t session = storage_card_session();
    if (rotation_attempted && session == rotation_session) return;
    rotation_session = session;
    rt_err_t result = start(JOB_ROTATE, rotation->path);
    if (result != -RT_EBUSY) rotation_attempted = true;
}

void wallpaper_rotation_shown(void)
{
    if (!rotation) return;
    if (rotation_ready) rt_kprintf("wallpaper rotation shown: %s\n", rotation->current);
    rotation_ready = rotation_attempted = false;
}

rt_err_t wallpaper_use_default(void)
{
    if (!state.ready) { wallpaper_init(); return -RT_EBUSY; }
    return start(JOB_DEFAULT, NULL);
}

static void view_deleted(lv_event_t *event)
{
    wallpaper_bitmap_t *image = lv_event_get_user_data(event);
    /* UI-side deletion, outside drawing callbacks: EPIC's completion hook
     * joins the render thread/GPU without dispatching LVGL events. The image
     * destructor does not dereference variable-image source descriptors. */
    lv_draw_wait_for_finish();
    lv_image_cache_drop(&image->image);
    lv_image_header_cache_drop(&image->image);
    epd_app_free(image->allocation);
    epd_app_free(image);
}

lv_obj_t *wallpaper_view_create(lv_obj_t *parent)
{
    if (!parent || !wallpaper_initialized || !state.ready) return NULL;
    lv_obj_t *view = lv_obj_create(parent);
    if (!view) return NULL;
    lv_obj_remove_style_all(view);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(view, 0, 0);
    lv_obj_set_size(view, display_width, display_height);
    lv_obj_set_style_bg_color(view, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    lv_obj_t *image = lv_image_create(view);
    if (!image) { lv_obj_delete(view); return NULL; }
    wallpaper_bitmap_t *bitmap = bitmap_create(selected);
    if (bitmap)
    {
        if (!lv_obj_add_event_cb(image, view_deleted, LV_EVENT_DELETE, bitmap))
        { epd_app_free(bitmap->allocation); epd_app_free(bitmap); lv_obj_delete(view); return NULL; }
        lv_obj_set_style_image_recolor(image, lv_color_black(), 0);
        lv_image_set_src(image, &bitmap->image);
        lv_obj_set_size(image, display_width, display_height);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    }
    else if (selected) { lv_obj_delete(view); return NULL; }
    else lv_image_set_src(image, &ui_icon_bookshelf);
    lv_obj_center(image);
    return view;
}
