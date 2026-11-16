/* SPDX-License-Identifier: Apache-2.0 */
#include "dfu_package.h"
#include "ff.h"
#include "ptab.h"

#if FF_LFN_UNICODE != 2 || !FF_USE_LFN || FF_FS_READONLY || FF_FS_MINIMIZE
#error DFU requires writable FatFs with UTF-8 long filenames and full file APIs
#endif

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_MALLOC
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#include "../../src/thirdparty/miniz/miniz.h"

#include <stdbool.h>
#include <string.h>

#define ZIP_LOCAL_SIZE       30u
#define ZIP_CENTRAL_SIZE     46u
#define ZIP_END_SIZE         22u
#define ZIP_MAX_ENTRIES      16u
#define ZIP_MAX_PATH         127u
#define ZIP_MAX_SIZE         (16u * 1024u * 1024u)
#define ZIP_DESCRIPTOR       0x0008u
#define ZIP_UTF8             0x0800u
#define ZIP_STORE            0u
#define ZIP_DEFLATE          8u
#define MANIFEST_MAX_SIZE    2048u
#define STAGE_DIR_NAME       "EPDDFU"

#ifndef DFU_PACKAGE_WORKSPACE_ATTR
#define DFU_PACKAGE_WORKSPACE_ATTR
#endif

typedef struct
{
    const char *name;
    const char *staging_path;
    uint32_t address;
    uint32_t capacity;
} target_info_t;

static const target_info_t targets[] =
{
    [DFU_TARGET_MAIN] = {"main", DFU_PACKAGE_STAGING_DIR "/MAIN.BIN",
                         HCPU_FLASH_CODE_START_ADDR, HCPU_FLASH_CODE_SIZE},
    [DFU_TARGET_RESOURCES] = {"resources", DFU_PACKAGE_STAGING_DIR "/RES.BIN",
                              BUILTIN_RESOURCE_START_ADDR, BUILTIN_RESOURCE_SIZE},
    [DFU_TARGET_ACPU] = {"acpu", DFU_PACKAGE_STAGING_DIR "/ACPU.BIN",
                         DFU_ACPU_FLASH_START_ADDR, DFU_ACPU_FLASH_SIZE},
    [DFU_TARGET_WAVEFORM] = {"waveform", DFU_PACKAGE_STAGING_DIR "/WAVE.BIN",
                             CUSTOM_EPD_WAVE_TABLE_START_ADDR, CUSTOM_EPD_WAVE_TABLE_SIZE},
};

typedef struct
{
    char name[ZIP_MAX_PATH + 1];
    uint32_t local;
    uint32_t data;
    uint32_t end;
    uint32_t compressed;
    uint32_t size;
    uint32_t crc;
    uint16_t flags;
    uint16_t method;
    uint16_t version;
    bool directory;
} zip_entry_t;

typedef struct
{
    FIL zip;
    FIL stage;
    tinfl_decompressor inflater;
    zip_entry_t entries[ZIP_MAX_ENTRIES];
    const zip_entry_t *image_entries[DFU_PACKAGE_MAX_IMAGES];
    uint8_t input[4096];
    uint8_t window[TINFL_LZ_DICT_SIZE];
    char manifest[MANIFEST_MAX_SIZE + 1];
    unsigned entry_count;
} package_workspace_t;

static package_workspace_t work DFU_PACKAGE_WORKSPACE_ATTR;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return read_le16(p) | ((uint32_t)read_le16(p + 2) << 16);
}

static bool read_exact(void *buffer, UINT size)
{
    UINT done;
    return f_read(&work.zip, buffer, size, &done) == FR_OK && done == size;
}

static bool read_at(uint32_t offset, void *buffer, UINT size)
{
    return f_lseek(&work.zip, offset) == FR_OK &&
           f_tell(&work.zip) == offset && read_exact(buffer, size);
}

static char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool path_separator(char c)
{
    return c == '/' || c == '\\';
}

static bool archive_path(const char *path)
{
    if (!path || !*path) return false;
    if (path[0] == '0' && path[1] == ':') path += 2;
    if (!path_separator(*path)) return false;

    /* FatFs ignores trailing dots and spaces in each name. */
    bool first = true;
    while (path_separator(*path))
    {
        do { ++path; } while (path_separator(*path));
        const char *start = path;
        while (*path && !path_separator(*path))
        {
            if (*path == ':') return false;
            ++path;
        }
        const char *end = path;
        while (end > start && (end[-1] == '.' || end[-1] == ' ')) --end;
        if (end == start) return false;
        if (first && end - start == sizeof(STAGE_DIR_NAME) - 1)
        {
            const char *reserved = STAGE_DIR_NAME;
            while (*reserved && ascii_lower(*start) == ascii_lower(*reserved))
            {
                ++start;
                ++reserved;
            }
            if (!*reserved) return false;
        }
        first = false;
    }
    return !first;
}

static bool entry_name(const char *name, unsigned size, bool directory)
{
    unsigned start = 0;
    if (directory) --size;
    for (unsigned i = 0; i <= size; ++i)
    {
        if (i == size || name[i] == '/')
        {
            unsigned length = i - start;
            if (!length || (name[start] == '.' &&
                (length == 1 || (length == 2 && name[start + 1] == '.'))))
                return false;
            start = i + 1;
        }
        else
        {
            unsigned char c = (unsigned char)name[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.' || c == ' '))
                return false;
        }
    }
    return true;
}

static dfu_package_result_t zip_end(uint32_t size, uint32_t *central, uint32_t *end)
{
    if (size < ZIP_END_SIZE || size > ZIP_MAX_SIZE) return DFU_PACKAGE_ZIP;
    const uint32_t limit = size - ZIP_END_SIZE > UINT16_MAX ?
                           size - ZIP_END_SIZE - UINT16_MAX : 0;
    uint32_t cursor = size;
    for (;;)
    {
        uint32_t start = cursor - limit > sizeof(work.input) ?
                         cursor - sizeof(work.input) : limit;
        if (!read_at(start, work.input, cursor - start)) return DFU_PACKAGE_IO;
        for (uint32_t i = cursor - start - ZIP_END_SIZE + 1; i-- > 0;)
        {
            const uint8_t *header = work.input + i;
            const uint32_t offset = start + i;
            if (read_le32(header) != 0x06054b50u ||
                read_le16(header + 20) != size - offset - ZIP_END_SIZE)
                continue;
            const uint16_t count = read_le16(header + 10);
            const uint32_t central_offset = read_le32(header + 16);
            if (read_le16(header + 4) || read_le16(header + 6) ||
                read_le16(header + 8) != count || !count || count > ZIP_MAX_ENTRIES ||
                central_offset > offset || read_le32(header + 12) != offset - central_offset ||
                offset - central_offset < (uint32_t)count * ZIP_CENTRAL_SIZE)
                continue;
            work.entry_count = count;
            *central = central_offset;
            *end = offset;
            return DFU_PACKAGE_OK;
        }
        if (start == limit) return DFU_PACKAGE_ZIP;
        cursor = start + ZIP_END_SIZE - 1;
    }
}

static bool descriptor_matches(const zip_entry_t *entry, const uint8_t *data)
{
    return read_le32(data) == entry->crc &&
           read_le32(data + 4) == entry->compressed &&
           read_le32(data + 8) == entry->size;
}

static dfu_package_result_t zip_local(zip_entry_t *entry, uint32_t central)
{
    uint8_t header[ZIP_LOCAL_SIZE];
    char name[ZIP_MAX_PATH + 1];
    if (entry->local > central || central - entry->local < ZIP_LOCAL_SIZE)
        return DFU_PACKAGE_ZIP;
    if (!read_at(entry->local, header, sizeof(header))) return DFU_PACKAGE_IO;
    uint16_t name_size = read_le16(header + 26);
    uint32_t extra = read_le16(header + 28);
    uint32_t name_offset = entry->local + ZIP_LOCAL_SIZE;
    if (read_le32(header) != 0x04034b50u || read_le16(header + 4) != entry->version ||
        read_le16(header + 6) != entry->flags || read_le16(header + 8) != entry->method ||
        name_size != strlen(entry->name) || name_size + extra > central - name_offset)
        return DFU_PACKAGE_ZIP;
    if (!read_exact(name, name_size)) return DFU_PACKAGE_IO;
    if (memcmp(name, entry->name, name_size)) return DFU_PACKAGE_ZIP;

    entry->data = name_offset + name_size + extra;
    if (entry->compressed > central - entry->data) return DFU_PACKAGE_ZIP;
    entry->end = entry->data + entry->compressed;
    if (!(entry->flags & ZIP_DESCRIPTOR))
        return descriptor_matches(entry, header + 14) ? DFU_PACKAGE_OK : DFU_PACKAGE_ZIP;

    if ((read_le32(header + 14) && read_le32(header + 14) != entry->crc) ||
        (read_le32(header + 18) && read_le32(header + 18) != entry->compressed) ||
        (read_le32(header + 22) && read_le32(header + 22) != entry->size))
        return DFU_PACKAGE_ZIP;

    uint8_t descriptor[16];
    uint32_t available = central - entry->end;
    if (available < 12u) return DFU_PACKAGE_ZIP;
    UINT amount = available < sizeof(descriptor) ? 12u : sizeof(descriptor);
    if (!read_at(entry->end, descriptor, amount)) return DFU_PACKAGE_IO;
    if (descriptor_matches(entry, descriptor))
        entry->end += 12;
    else if (amount == sizeof(descriptor) && read_le32(descriptor) == 0x08074b50u &&
             descriptor_matches(entry, descriptor + 4))
        entry->end += 16;
    else
        return DFU_PACKAGE_ZIP;
    return DFU_PACKAGE_OK;
}

static dfu_package_result_t zip_directory(uint32_t size)
{
    uint32_t central, end;
    dfu_package_result_t result = zip_end(size, &central, &end);
    if (result != DFU_PACKAGE_OK) return result;
    uint32_t position = central;

    for (unsigned i = 0; i < work.entry_count; ++i)
    {
        uint8_t header[ZIP_CENTRAL_SIZE];
        if (end - position < sizeof(header)) return DFU_PACKAGE_ZIP;
        if (!read_at(position, header, sizeof(header))) return DFU_PACKAGE_IO;
        uint16_t name_size = read_le16(header + 28);
        uint32_t record_size = sizeof(header) + name_size +
                               read_le16(header + 30) + read_le16(header + 32);
        zip_entry_t *entry = &work.entries[i];
        entry->version = read_le16(header + 6);
        entry->flags = read_le16(header + 8);
        entry->method = read_le16(header + 10);
        entry->crc = read_le32(header + 16);
        entry->compressed = read_le32(header + 20);
        entry->size = read_le32(header + 24);
        entry->local = read_le32(header + 42);

        uint16_t flags = ZIP_DESCRIPTOR | ZIP_UTF8;
        if (entry->method == ZIP_DEFLATE) flags |= 6u;
        if (read_le32(header) != 0x02014b50u || read_le16(header + 34) ||
            entry->version < 10u || entry->version > 20u ||
            (entry->flags & ~flags) || !name_size || name_size > ZIP_MAX_PATH ||
            record_size > end - position || entry->size == UINT32_MAX ||
            (entry->method != ZIP_STORE && entry->method != ZIP_DEFLATE) ||
            (entry->method == ZIP_STORE && entry->size != entry->compressed) ||
            (entry->method == ZIP_DEFLATE && !entry->compressed))
            return DFU_PACKAGE_ZIP;

        if (!read_exact(entry->name, name_size)) return DFU_PACKAGE_IO;
        entry->name[name_size] = '\0';
        entry->directory = entry->name[name_size - 1] == '/';
        if (!entry_name(entry->name, name_size, entry->directory) ||
            (entry->directory && (entry->size || entry->crc)))
            return DFU_PACKAGE_ZIP;
        result = zip_local(entry, central);
        if (result != DFU_PACKAGE_OK) return result;

        for (unsigned j = 0; j < i; ++j)
        {
            const zip_entry_t *prior = &work.entries[j];
            if (!strcmp(prior->name, entry->name) ||
                (prior->local < entry->end && entry->local < prior->end))
                return DFU_PACKAGE_ZIP;
        }
        position += record_size;
    }
    return position == end ? DFU_PACKAGE_OK : DFU_PACKAGE_ZIP;
}

static const zip_entry_t *find_entry(const char *name)
{
    for (unsigned i = 0; i < work.entry_count; ++i)
    {
        const zip_entry_t *entry = &work.entries[i];
        if (!entry->directory && !strcmp(entry->name, name)) return entry;
    }
    return NULL;
}

typedef struct
{
    FIL *file;
    dfu_package_progress_t progress;
    uint32_t total;
    uint32_t previous;
    uint32_t size;
    uint32_t crc;
    dfu_target_t target;
} extract_output_t;

static dfu_package_result_t extract_chunk(extract_output_t *output, const zip_entry_t *entry,
                                          const uint8_t *data, UINT size)
{
    if (size > entry->size - output->size) return DFU_PACKAGE_ZIP;
    if (!size) return DFU_PACKAGE_OK;

    if (output->file)
    {
        UINT written;
        if (f_write(output->file, data, size, &written) != FR_OK)
            return DFU_PACKAGE_IO;
        if (written != size) return DFU_PACKAGE_NO_SPACE;
    }
    else
        memcpy(work.manifest + output->size, data, size);

    output->crc = (uint32_t)mz_crc32(output->crc, data, size);
    output->size += size;
    if (output->progress && output->size < entry->size)
        output->progress(output->target, output->previous + output->size, output->total);
    return DFU_PACKAGE_OK;
}

static dfu_package_result_t extract_deflate(const zip_entry_t *entry, extract_output_t *output)
{
    uint32_t remaining = entry->compressed;
    size_t input_size = 0, input_offset = 0, window_offset = 0;
    tinfl_init(&work.inflater);
    for (;;)
    {
        if (input_offset == input_size && remaining)
        {
            input_size = remaining < sizeof(work.input) ? remaining : sizeof(work.input);
            if (!read_exact(work.input, (UINT)input_size)) return DFU_PACKAGE_IO;
            remaining -= (uint32_t)input_size;
            input_offset = 0;
        }

        size_t in = input_size - input_offset;
        size_t out = sizeof(work.window) - window_offset;
        unsigned flags = remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        /* Check back-reference distances while filling the first window. */
        if (output->size < sizeof(work.window)) flags |= TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        tinfl_status status = tinfl_decompress(&work.inflater, work.input + input_offset, &in,
            work.window, work.window + window_offset, &out, flags);
        if (status < TINFL_STATUS_DONE) return DFU_PACKAGE_ZIP;

        dfu_package_result_t result = extract_chunk(output, entry, work.window + window_offset,
                                                      (UINT)out);
        if (result != DFU_PACKAGE_OK) return result;
        window_offset = (window_offset + out) % sizeof(work.window);
        input_offset += in;
        if (status == TINFL_STATUS_DONE)
            return !remaining && input_offset == input_size ? DFU_PACKAGE_OK : DFU_PACKAGE_ZIP;
        if ((!in && !out) || (status == TINFL_STATUS_NEEDS_MORE_INPUT && !remaining))
            return DFU_PACKAGE_ZIP;
    }
}

static dfu_package_result_t extract(const zip_entry_t *entry, extract_output_t *output)
{
    if (f_lseek(&work.zip, entry->data) != FR_OK || f_tell(&work.zip) != entry->data)
        return DFU_PACKAGE_IO;
    if (entry->method == ZIP_DEFLATE)
    {
        dfu_package_result_t result = extract_deflate(entry, output);
        if (result != DFU_PACKAGE_OK) return result;
    }
    else
    {
        while (output->size < entry->size)
        {
            UINT amount = entry->size - output->size;
            if (amount > sizeof(work.window)) amount = sizeof(work.window);
            if (!read_exact(work.window, amount)) return DFU_PACKAGE_IO;
            dfu_package_result_t result = extract_chunk(output, entry, work.window, amount);
            if (result != DFU_PACKAGE_OK) return result;
        }
    }
    if (output->size != entry->size) return DFU_PACKAGE_ZIP;
    return output->crc == entry->crc ? DFU_PACKAGE_OK : DFU_PACKAGE_CRC;
}

static void json_space(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') ++*p;
}

static bool json_take(const char **p, char token)
{
    json_space(p);
    if (**p != token) return false;
    ++*p;
    return true;
}

static bool json_string(const char **p, char *text, size_t capacity)
{
    if (!json_take(p, '"')) return false;
    size_t length = 0;
    while (**p && **p != '"')
    {
        unsigned char c = (unsigned char)**p;
        if (c < 32 || c >= 127 || c == '\\' || length == capacity - 1)
            return false;
        text[length++] = (char)c;
        ++*p;
    }
    if (**p != '"') return false;
    ++*p;
    text[length] = '\0';
    return true;
}

static bool json_uint(const char **p, uint32_t *value)
{
    json_space(p);
    const char *start = *p;
    *value = 0;
    while (**p >= '0' && **p <= '9')
    {
        uint32_t digit = (uint32_t)(**p - '0');
        if (*value > (UINT32_MAX - digit) / 10u) return false;
        *value = *value * 10u + digit;
        ++*p;
    }
    return *p > start && (*start != '0' || *p - start == 1);
}

static int json_field(const char **p, const char *const fields[], unsigned count,
                      unsigned *seen)
{
    char key[24];
    if (!json_string(p, key, sizeof(key)) || !json_take(p, ':')) return -1;
    for (unsigned i = 0; i < count; ++i)
    {
        if (strcmp(key, fields[i])) continue;
        if (*seen & (1u << i)) return -1;
        *seen |= 1u << i;
        return (int)i;
    }
    return -1;
}

static dfu_package_result_t manifest_image(const char **p, dfu_package_t *package,
                                           unsigned *seen_targets)
{
    static const char *const fields[] = {"target", "file", "address", "size", "crc32"};
    char target_name[16], name[ZIP_MAX_PATH + 1];
    uint32_t values[3];
    unsigned seen = 0;
    if (!json_take(p, '{')) return DFU_PACKAGE_MANIFEST;
    do
    {
        int field = json_field(p, fields, 5, &seen);
        if (field < 0) return DFU_PACKAGE_MANIFEST;
        if (field == 0)
        {
            if (!json_string(p, target_name, sizeof(target_name))) return DFU_PACKAGE_MANIFEST;
        }
        else if (field == 1)
        {
            if (!json_string(p, name, sizeof(name))) return DFU_PACKAGE_MANIFEST;
        }
        else if (!json_uint(p, &values[field - 2]))
            return DFU_PACKAGE_MANIFEST;
    } while (json_take(p, ','));
    if (!json_take(p, '}') || seen != 31u) return DFU_PACKAGE_MANIFEST;

    unsigned target = 0;
    while (target < DFU_PACKAGE_MAX_IMAGES && strcmp(target_name, targets[target].name)) ++target;
    if (target == DFU_PACKAGE_MAX_IMAGES || (*seen_targets & (1u << target)) ||
        !strcmp(name, "manifest.json"))
        return DFU_PACKAGE_MANIFEST;

    const target_info_t *info = &targets[target];
    uint32_t size = values[1];
    if (values[0] != info->address || !size ||
        (info->address % DFU_PACKAGE_ERASE_SIZE) ||
        (info->capacity % DFU_PACKAGE_ERASE_SIZE) || size > info->capacity)
        return DFU_PACKAGE_RANGE;
    const zip_entry_t *entry = find_entry(name);
    if (!entry || entry->size != size) return DFU_PACKAGE_MANIFEST;
    if (entry->crc != values[2]) return DFU_PACKAGE_CRC;

    for (unsigned i = 0; i < DFU_PACKAGE_MAX_IMAGES; ++i)
    {
        if (work.image_entries[i] == entry) return DFU_PACKAGE_MANIFEST;
    }

    work.image_entries[target] = entry;
    package->images[target] = (dfu_package_image_t){
        (dfu_target_t)target, info->address, size, values[2], info->staging_path};
    package->total_size += size;
    *seen_targets |= 1u << target;
    return DFU_PACKAGE_OK;
}

static dfu_package_result_t manifest_parse(dfu_package_t *package)
{
    static const char *const fields[] = {"format_version", "board", "layout", "images"};
    const char *p = work.manifest;
    unsigned seen = 0, seen_targets = 0;
    uint32_t version;
    char id[32];
    for (unsigned i = 0; i < DFU_PACKAGE_MAX_IMAGES; ++i) work.image_entries[i] = NULL;
    if (!json_take(&p, '{')) return DFU_PACKAGE_MANIFEST;
    do
    {
        int field = json_field(&p, fields, 4, &seen);
        switch (field)
        {
        case 0:
            if (!json_uint(&p, &version) || version != 1u) return DFU_PACKAGE_MANIFEST;
            break;
        case 1:
        case 2:
            if (!json_string(&p, id, sizeof(id))) return DFU_PACKAGE_MANIFEST;
            if (strcmp(id, field == 1 ? DFU_PACKAGE_BOARD : DFU_PACKAGE_LAYOUT))
                return DFU_PACKAGE_LAYOUT_MISMATCH;
            break;
        case 3:
            if (!json_take(&p, '[')) return DFU_PACKAGE_MANIFEST;
            do
            {
                dfu_package_result_t result = manifest_image(&p, package, &seen_targets);
                if (result != DFU_PACKAGE_OK) return result;
            } while (json_take(&p, ','));
            if (!json_take(&p, ']')) return DFU_PACKAGE_MANIFEST;
            break;
        default:
            return DFU_PACKAGE_MANIFEST;
        }
    } while (json_take(&p, ','));
    if (!json_take(&p, '}') || seen != 15u) return DFU_PACKAGE_MANIFEST;
    json_space(&p);
    if (*p) return DFU_PACKAGE_MANIFEST;

    for (unsigned target = 0; target < DFU_PACKAGE_MAX_IMAGES; ++target)
    {
        if (seen_targets & (1u << target))
        {
            unsigned index = package->count++;
            if (index != target)
            {
                package->images[index] = package->images[target];
                memset(&package->images[target], 0, sizeof(package->images[target]));
            }
        }
    }
    return DFU_PACKAGE_OK;
}

static dfu_package_result_t read_package(dfu_package_t *package)
{
    if (f_size(&work.zip) > ZIP_MAX_SIZE) return DFU_PACKAGE_ZIP;
    package->zip_size = (uint32_t)f_size(&work.zip);
    dfu_package_result_t result = zip_directory(package->zip_size);
    if (result != DFU_PACKAGE_OK) return result;

    const zip_entry_t *manifest = find_entry("manifest.json");
    if (!manifest || !manifest->size || manifest->size > MANIFEST_MAX_SIZE)
        return DFU_PACKAGE_MANIFEST;
    extract_output_t output = {0};
    result = extract(manifest, &output);
    if (result != DFU_PACKAGE_OK) return result;
    if (memchr(work.manifest, '\0', output.size)) return DFU_PACKAGE_MANIFEST;
    work.manifest[output.size] = '\0';
    return manifest_parse(package);
}

dfu_package_result_t dfu_package_cleanup(void)
{
    dfu_package_result_t result = DFU_PACKAGE_OK;
    for (unsigned i = 0; i < DFU_PACKAGE_MAX_IMAGES; ++i)
    {
        FRESULT status = f_unlink(targets[i].staging_path);
        if (status != FR_OK && status != FR_NO_PATH && status != FR_NO_FILE)
            result = DFU_PACKAGE_IO;
    }
    return result;
}

static dfu_package_result_t prepare_staging(const dfu_package_t *package)
{
    dfu_package_result_t result = dfu_package_cleanup();
    if (result != DFU_PACKAGE_OK) return result;

    FRESULT status = f_mkdir(DFU_PACKAGE_STAGING_DIR);
    if (status != FR_OK && status != FR_EXIST) return DFU_PACKAGE_IO;

    DWORD free_clusters;
    FATFS *fs;
    if (f_getfree(DFU_PACKAGE_STAGING_DIR, &free_clusters, &fs) != FR_OK)
        return DFU_PACKAGE_IO;
#if FF_MAX_SS == FF_MIN_SS
    const uint32_t cluster_bytes = (uint32_t)fs->csize * FF_MIN_SS;
#else
    const uint32_t cluster_bytes = (uint32_t)fs->csize * fs->ssize;
#endif
    if (!cluster_bytes) return DFU_PACKAGE_IO;
    uint32_t required = 0;
    for (unsigned i = 0; i < package->count; ++i)
        required += 1u + (package->images[i].size - 1u) / cluster_bytes;
    return required <= free_clusters ? DFU_PACKAGE_OK : DFU_PACKAGE_NO_SPACE;
}

static dfu_package_result_t stage_images(const dfu_package_t *package,
                                          dfu_package_progress_t progress)
{
    uint32_t previous = 0;
    for (unsigned i = 0; i < package->count; ++i)
    {
        const dfu_package_image_t *image = &package->images[i];
        if (progress) progress(image->target, previous, package->total_size);
        if (f_open(&work.stage, image->staging_path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
            return DFU_PACKAGE_IO;
        extract_output_t output = {&work.stage, progress, package->total_size,
                                    previous, 0, 0, image->target};
        dfu_package_result_t result = extract(work.image_entries[image->target], &output);
        if (result == DFU_PACKAGE_OK && f_sync(&work.stage) != FR_OK)
            result = DFU_PACKAGE_IO;
        if (f_close(&work.stage) != FR_OK && result == DFU_PACKAGE_OK)
            result = DFU_PACKAGE_IO;
        if (result != DFU_PACKAGE_OK) return result;

        previous += image->size;
        if (progress) progress(image->target, previous, package->total_size);
    }
    return DFU_PACKAGE_OK;
}

static dfu_package_result_t load(const char *path, dfu_package_t *package,
                                 bool prepare, dfu_package_progress_t progress)
{
    if (!package) return DFU_PACKAGE_INVALID;
    memset(package, 0, sizeof(*package));
    if (!archive_path(path)) return DFU_PACKAGE_INVALID;
    if (f_open(&work.zip, path, FA_READ) != FR_OK) return DFU_PACKAGE_IO;

    dfu_package_result_t result = read_package(package);
    if (result == DFU_PACKAGE_OK && prepare)
    {
        result = prepare_staging(package);
        if (result == DFU_PACKAGE_OK) result = stage_images(package, progress);
    }
    if (f_close(&work.zip) != FR_OK && result == DFU_PACKAGE_OK) result = DFU_PACKAGE_IO;
    if (result != DFU_PACKAGE_OK)
    {
        memset(package, 0, sizeof(*package));
        if (prepare) (void)dfu_package_cleanup();
    }
    return result;
}

dfu_package_result_t dfu_package_open(const char *path, dfu_package_t *package)
{
    return load(path, package, false, NULL);
}

dfu_package_result_t dfu_package_prepare(const char *path, dfu_package_t *package,
                                         dfu_package_progress_t progress)
{
    return load(path, package, true, progress);
}
