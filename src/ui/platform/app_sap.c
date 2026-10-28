#include "storage_internal.h"
#include "storage_file.h"
#include "app_sap.h"
#include "epd_app.h"
#include "epd_memory.h"
#include "icons/ui_icons.h"
#include <cJSON.h>
#include <dfs_posix.h>
#include <mbedtls/sha256.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define SAP_MANIFEST_MAX 65536u
#define SAP_HEADER_SIZE 32u
#define SAP_RECORD_SIZE 48u

typedef struct
{
    int fd;
    const char *path;
    uint32_t session, revision, count, index;
    storage_volume_t volume;
    uint64_t size, offset, remaining;
    unsigned char digest[32];
    char name[EPD_PACKAGE_PATH_MAX], previous[EPD_PACKAGE_PATH_MAX];
} sap_reader_t;

static uint64_t little(const unsigned char *p, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static bool ready(const sap_reader_t *reader)
{
    return !storage_release_requested() && !storage_changing() &&
           storage_session_valid(reader->volume, reader->session) &&
           (reader->fd < 0 || (storage_path_revision(reader->path) == reader->revision &&
                              !storage_file_writers(reader->path)));
}

static bool read_exact(sap_reader_t *reader, void *buffer, size_t length)
{
    unsigned char *next = buffer;
    if (reader->offset > reader->size || length > reader->size - reader->offset) return false;
    while (length && ready(reader))
    {
        size_t chunk = length > EPD_PACKAGE_IO_SIZE ? EPD_PACKAGE_IO_SIZE : length;
        storage_lock();
        int n = ready(reader) ? read(reader->fd, next, chunk) : -1;
        storage_unlock();
        if (n <= 0) return false;
        next += n; length -= n; reader->offset += n;
    }
    return !length && ready(reader);
}

static bool utf8_valid(const unsigned char *text)
{
    while (*text)
    {
        uint32_t value = *text++; unsigned extra; uint32_t minimum;
        if (value < 0x80) continue;
        if (value >= 0xc2 && value <= 0xdf) { value &= 0x1f; extra = 1; minimum = 0x80; }
        else if (value >= 0xe0 && value <= 0xef) { value &= 15; extra = 2; minimum = 0x800; }
        else if (value >= 0xf0 && value <= 0xf4) { value &= 7; extra = 3; minimum = 0x10000; }
        else return false;
        while (extra--)
        {
            if ((*text & 0xc0) != 0x80) return false;
            value = (value << 6) | (*text++ & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool relative_name(const char *name)
{
    const char *part = name; unsigned depth = 1;
    if (!utf8_valid((const unsigned char *)name)) return false;
    for (const char *p = name; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || (unsigned char)*p == 127 || strchr("\\:<>\"|?*", *p))) return false;
        if (*p == '/' || !*p)
        {
            size_t length = p - part;
            if (!length || p[-1] == '.' || p[-1] == ' ') return false;
            if (!*p) return true;
            if (++depth > 8) return false;
            part = p + 1;
        }
    }
}

static app_package_status_t reader_open(sap_reader_t *reader, const char *path)
{
    memset(reader, 0, sizeof(*reader)); reader->fd = -1;
    if (!path || strlen(path) >= APP_SAP_PATH_MAX) return APP_PACKAGE_FORMAT_ERROR;
    reader->path = path;
    reader->volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    reader->session = storage_card_session();
    if (!ready(reader)) return APP_PACKAGE_IO_ERROR;
    struct stat st;
    storage_lock();
    reader->fd = ready(reader) ? open(path, O_RDONLY) : -1;
    bool opened = reader->fd >= 0 && fstat(reader->fd, &st) == 0;
    if (opened) reader->revision = storage_path_revision(path);
    storage_unlock();
    if (!opened) return APP_PACKAGE_IO_ERROR;
    if (!S_ISREG(st.st_mode) || st.st_size < SAP_HEADER_SIZE || (uint64_t)st.st_size > APP_SAP_SIZE_MAX)
        return APP_PACKAGE_FORMAT_ERROR;
    reader->size = st.st_size;
    unsigned char header[SAP_HEADER_SIZE];
    if (!read_exact(reader, header, sizeof(header))) return APP_PACKAGE_IO_ERROR;
    if (memcmp(header, "SIFLSAP\0", 8) || little(header + 8, 4) != 1 ||
        little(header + 16, 8) != reader->size || little(header + 24, 8)) return APP_PACKAGE_FORMAT_ERROR;
    reader->count = (uint32_t)little(header + 12, 4);
    if (reader->count < 2 || reader->count > APP_SAP_FILE_MAX ||
        reader->count > (reader->size - SAP_HEADER_SIZE) / (SAP_RECORD_SIZE + 1)) return APP_PACKAGE_FORMAT_ERROR;
    return APP_PACKAGE_OK;
}

static app_package_status_t next_file(sap_reader_t *reader)
{
    unsigned char record[SAP_RECORD_SIZE];
    if (reader->remaining || reader->index >= reader->count ||
        reader->size - reader->offset < sizeof(record)) return APP_PACKAGE_FORMAT_ERROR;
    if (!read_exact(reader, record, sizeof(record))) return APP_PACKAGE_IO_ERROR;
    size_t length = (size_t)little(record, 2);
    uint64_t size = little(record + 4, 8);
    if (!length || length >= sizeof(reader->name) || little(record + 2, 2) || little(record + 44, 4) ||
        length > reader->size - reader->offset || size > reader->size - reader->offset - length)
        return APP_PACKAGE_FORMAT_ERROR;
    if (!read_exact(reader, reader->name, length)) return APP_PACKAGE_IO_ERROR;
    reader->name[length] = 0;
    if (memchr(reader->name, 0, length) || !relative_name(reader->name)) return APP_PACKAGE_FORMAT_ERROR;
    if (!reader->index)
    {
        if (strcmp(reader->name, "app.json") || !size || size > SAP_MANIFEST_MAX) return APP_PACKAGE_FORMAT_ERROR;
    }
    else if (reader->index == 1)
    {
        if (strcmp(reader->name, "files.sha256") || size > APP_SAP_FILE_MAX * (EPD_PACKAGE_PATH_MAX + 67u))
            return APP_PACKAGE_FORMAT_ERROR;
    }
    else
    {
        if (!strcmp(reader->name, "app.json") || !strcmp(reader->name, "files.sha256") ||
            strcmp(reader->previous, reader->name) >= 0) return APP_PACKAGE_FORMAT_ERROR;
        strcpy(reader->previous, reader->name);
    }
    memcpy(reader->digest, record + 12, sizeof(reader->digest));
    reader->remaining = size; ++reader->index;
    return APP_PACKAGE_OK;
}

static bool bounded_json(const char *text, size_t size)
{
    unsigned depth = 0; bool quoted = false;
    if (memchr(text, 0, size) || !utf8_valid((const unsigned char *)text)) return false;
    for (size_t i = 0; i < size; ++i)
    {
        char c = text[i];
        if (quoted)
        {
            if ((unsigned char)c < 32) return false;
            if (c == '\\')
            {
                if (++i == size) return false;
                if (text[i] == 'u' && i + 4 < size && !memcmp(text + i + 1, "0000", 4)) return false;
            }
            else if (c == '"') quoted = false;
        }
        else if (c == '-' || (c >= '0' && c <= '9'))
        {
            size_t n = i;
            if (text[n] == '-') ++n;
            if (text[n] == '0') ++n;
            else
            {
                if (text[n] < '1' || text[n] > '9') return false;
                do { ++n; } while (text[n] >= '0' && text[n] <= '9');
            }
            if (text[n] == '.')
            {
                ++n;
                if (text[n] < '0' || text[n] > '9') return false;
                do { ++n; } while (text[n] >= '0' && text[n] <= '9');
            }
            if (text[n] == 'e' || text[n] == 'E')
            {
                ++n;
                if (text[n] == '+' || text[n] == '-') ++n;
                if (text[n] < '0' || text[n] > '9') return false;
                do { ++n; } while (text[n] >= '0' && text[n] <= '9');
            }
            if (n < size && !strchr(" \t\r\n,}]", text[n])) return false;
            i = n - 1;
        }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
        else if ((unsigned char)c < 32 && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return !quoted && !depth;
}

static const char *json_string(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_uint(const cJSON *json, const char *key, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 1 && item->valuedouble <= UINT32_MAX)) return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if (value != item->valuedouble) return false;
    *out = value; return true;
}

static app_package_status_t metadata_json(const char *text, epd_app_entry_t *entry)
{
    cJSON *json = cJSON_ParseWithOpts(text, NULL, true);
    if (!json) return APP_PACKAGE_FORMAT_ERROR;
    app_package_status_t status = APP_PACKAGE_FORMAT_ERROR;
    const char *id = json_string(json, "id"), *name = json_string(json, "name");
    const char *kind = json_string(json, "kind"), *build = json_string(json, "build_id");
    uint32_t abi, format;
    if (!cJSON_IsObject(json) || !id || !*id || strlen(id) >= RT_NAME_MAX || strlen(id) >= sizeof(entry->id) ||
        !name || !*name || strlen(name) >= sizeof(entry->name) || !build || strlen(build) != 64 ||
        (cJSON_GetObjectItemCaseSensitive(json, "kind") && !kind) ||
        (kind && strcmp(kind, "module") && strcmp(kind, "resources"))) goto done;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) goto done;
    for (const char *p = build; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) goto done;
    const cJSON *item; unsigned fields = 0;
    cJSON_ArrayForEach(item, json)
    {
        if (++fields > 64) goto done;
        for (const cJSON *previous = json->child; previous != item; previous = previous->next)
            if (!strcmp(previous->string, item->string)) goto done;
    }
    memset(entry, 0, sizeof(*entry));
    if (!json_uint(json, "abi", &abi) || !json_uint(json, "package_format", &format) ||
        !json_uint(json, "version", &entry->version) || !json_uint(json, "data_version", &entry->data_version)) goto done;
    if (abi != EPD_APP_ABI || format != EPD_PACKAGE_FORMAT) { status = APP_PACKAGE_INCOMPATIBLE; goto done; }
    const char *flags[] = {"settings", "background", "lockscreen"};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
    {
        item = cJSON_GetObjectItemCaseSensitive(json, flags[i]);
        if (item && !cJSON_IsBool(item)) goto done;
    }
    strcpy(entry->id, id); strcpy(entry->name, name); strcpy(entry->build_id, build);
    entry->resources_only = kind && !strcmp(kind, "resources");
    entry->has_settings = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "settings"));
    entry->background = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "background"));
    entry->lockscreen = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "lockscreen"));
    if (entry->resources_only && entry->lockscreen) goto done;
    entry->icon = &ui_icon_album;
    status = APP_PACKAGE_OK;
done:
    cJSON_Delete(json); return status;
}

app_package_status_t app_sap_metadata(const char *path, epd_app_entry_t *entry)
{
    if (!entry) return APP_PACKAGE_FORMAT_ERROR;
    sap_reader_t reader; char *text = NULL;
    app_package_status_t status = reader_open(&reader, path);
    if (status != APP_PACKAGE_OK) goto done;
    status = next_file(&reader);
    if (status != APP_PACKAGE_OK) goto done;
    size_t length = (size_t)reader.remaining;
    text = epd_app_alloc(length + 1, EPD_APP_PSRAM);
    status = APP_PACKAGE_IO_ERROR;
    if (!text || !read_exact(&reader, text, length)) goto done;
    text[length] = 0;
    unsigned char digest[32];
    if (mbedtls_sha256_ret((const unsigned char *)text, length, digest, 0)) goto done;
    status = APP_PACKAGE_INTEGRITY_ERROR;
    if (memcmp(digest, reader.digest, sizeof(digest))) goto done;
    status = APP_PACKAGE_FORMAT_ERROR;
    if (!bounded_json(text, length)) goto done;
    status = metadata_json(text, entry);
done:
    epd_app_free(text);
    if (reader.fd >= 0) { storage_lock(); close(reader.fd); storage_unlock(); }
    return status;
}

app_package_status_t app_sap_inspect(const char *path, uint32_t block_size,
                                    uint64_t *allocated, uint64_t *payload)
{
    if (!block_size || !allocated || !payload) return APP_PACKAGE_FORMAT_ERROR;
    *allocated = block_size; *payload = 0;
    sap_reader_t reader;
    app_package_status_t status = reader_open(&reader, path);
    char previous[EPD_PACKAGE_PATH_MAX] = {0};
    while (status == APP_PACKAGE_OK && reader.index < reader.count)
    {
        status = next_file(&reader);
        if (status != APP_PACKAGE_OK) break;
        *payload += reader.remaining;
        *allocated += (reader.remaining + block_size - 1) / block_size * block_size;
        for (size_t i = 0; reader.name[i]; ++i)
            if (reader.name[i] == '/' && strncmp(reader.name, previous, i + 1)) *allocated += block_size;
        strcpy(previous, reader.name);
        uint64_t next = reader.offset + reader.remaining;
        storage_lock();
        bool sought = ready(&reader) && lseek(reader.fd, (off_t)next, SEEK_SET) == (off_t)next && ready(&reader);
        storage_unlock();
        if (!sought)
        { status = APP_PACKAGE_IO_ERROR; break; }
        reader.offset = next; reader.remaining = 0;
    }
    if (status == APP_PACKAGE_OK && reader.offset != reader.size) status = APP_PACKAGE_FORMAT_ERROR;
    if (status == APP_PACKAGE_OK && !ready(&reader)) status = APP_PACKAGE_IO_ERROR;
    if (reader.fd >= 0) { storage_lock(); close(reader.fd); storage_unlock(); }
    return status;
}

static app_package_status_t extract(const char *path, const char *destination, const char *id,
                                     app_package_progress_t progress, void *user)
{
    if (!destination || !*destination || strlen(destination) >= APP_SAP_PATH_MAX)
        return APP_PACKAGE_FORMAT_ERROR;
    sap_reader_t reader;
    app_package_status_t status = reader_open(&reader, path);
    unsigned char *buffer = NULL;
    char *manifest = NULL;
    app_package_record_t *records = NULL;
    if (status != APP_PACKAGE_OK) goto done;
    status = APP_PACKAGE_IO_ERROR;
    storage_volume_t volume = !strcmp(storage_path_root(destination), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    storage_lock();
    bool created = ready(&reader) && storage_session_valid(volume, reader.session) && mkdir(destination, 0) == 0;
    storage_unlock();
    if (!created) goto done;
    buffer = epd_app_alloc(EPD_PACKAGE_IO_SIZE, EPD_APP_PSRAM);
    if (!buffer) goto done;
    if (id)
    {
        records = epd_app_alloc(reader.count * sizeof(*records), EPD_APP_PSRAM);
        if (!records) goto done;
    }
    status = APP_PACKAGE_OK;
    while (status == APP_PACKAGE_OK && reader.index < reader.count)
    {
        status = next_file(&reader);
        if (status != APP_PACKAGE_OK) break;
        if (records)
        {
            app_package_record_t *record = &records[reader.index - 1];
            strcpy(record->name, reader.name); record->size = reader.remaining;
            memcpy(record->digest, reader.digest, sizeof(record->digest));
            if (reader.index == 1)
            {
                manifest = epd_app_alloc((size_t)reader.remaining + 1, EPD_APP_PSRAM);
                if (!manifest) { status = APP_PACKAGE_IO_ERROR; break; }
                manifest[reader.remaining] = 0;
            }
        }
        char target[APP_SAP_PATH_MAX];
        int n = snprintf(target, sizeof(target), "%s/%s", destination, reader.name);
        if (n < 0 || (size_t)n >= sizeof(target)) { status = APP_PACKAGE_FORMAT_ERROR; break; }
        storage_lock();
        char *separator = strrchr(target, '/'); *separator = 0;
        bool parents = ready(&reader) && storage_session_valid(volume, reader.session) && storage_mkdirs(target);
        *separator = '/';
        int fd = parents ? open(target, O_WRONLY | O_CREAT | O_EXCL, 0) : -1;
        storage_unlock();
        if (fd < 0) { status = APP_PACKAGE_IO_ERROR; break; }
        mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
        bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0;
        while (ok && reader.remaining)
        {
            size_t length = reader.remaining > EPD_PACKAGE_IO_SIZE ? EPD_PACKAGE_IO_SIZE : (size_t)reader.remaining;
            ok = storage_session_valid(volume, reader.session) && read_exact(&reader, buffer, length) &&
                 mbedtls_sha256_update_ret(&hash, buffer, length) == 0;
            if (ok && manifest)
                memcpy(manifest + records[0].size - reader.remaining, buffer, length);
            size_t written = 0;
            while (ok && written < length)
            {
                storage_lock();
                int count = ready(&reader) && storage_session_valid(volume, reader.session) ?
                            write(fd, buffer + written, length - written) : -1;
                storage_unlock();
                if (count <= 0) ok = false;
                else written += count;
            }
            if (ok)
            {
                reader.remaining -= length;
                if (progress) progress(reader.offset, reader.size, user);
            }
        }
        unsigned char digest[32];
        ok = ok && ready(&reader) && storage_session_valid(volume, reader.session) &&
             mbedtls_sha256_finish_ret(&hash, digest) == 0;
        mbedtls_sha256_free(&hash);
        if (!ok) status = APP_PACKAGE_IO_ERROR;
        else if (memcmp(digest, reader.digest, sizeof(digest))) status = APP_PACKAGE_INTEGRITY_ERROR;
        storage_lock();
        if (status == APP_PACKAGE_OK && (!ready(&reader) || !storage_session_valid(volume, reader.session) ||
            fsync(fd) != 0)) status = APP_PACKAGE_IO_ERROR;
        if (close(fd) != 0 && status == APP_PACKAGE_OK) status = APP_PACKAGE_IO_ERROR;
        storage_unlock();
        if (status == APP_PACKAGE_OK && manifest)
        {
            epd_app_entry_t entry;
            status = bounded_json(manifest, (size_t)records[0].size) ?
                     metadata_json(manifest, &entry) : APP_PACKAGE_FORMAT_ERROR;
            if (status == APP_PACKAGE_OK && strcmp(entry.id, id)) status = APP_PACKAGE_FORMAT_ERROR;
        }
        epd_app_free(manifest);
        manifest = NULL;
    }
    if (status == APP_PACKAGE_OK && reader.offset != reader.size) status = APP_PACKAGE_FORMAT_ERROR;
    if (status == APP_PACKAGE_OK && (!ready(&reader) || !storage_session_valid(volume, reader.session)))
        status = APP_PACKAGE_IO_ERROR;
    if (status == APP_PACKAGE_OK && records)
    {
        status = ready(&reader) && storage_session_valid(volume, reader.session) ?
                 app_package_verify_extracted(destination, id, records, reader.count) : APP_PACKAGE_IO_ERROR;
        if (!ready(&reader) || !storage_session_valid(volume, reader.session)) status = APP_PACKAGE_IO_ERROR;
    }
done:
    epd_app_free(buffer);
    epd_app_free(manifest);
    epd_app_free(records);
    if (reader.fd >= 0) { storage_lock(); close(reader.fd); storage_unlock(); }
    return status;
}

app_package_status_t app_sap_extract(const char *path, const char *destination,
                                    app_package_progress_t progress, void *user)
{
    return extract(path, destination, NULL, progress, user);
}

app_package_status_t app_sap_install(const char *path, const char *destination, const char *id,
                                    app_package_progress_t progress, void *user)
{
    if (!id || !*id) return APP_PACKAGE_FORMAT_ERROR;
    return extract(path, destination, id, progress, user);
}
