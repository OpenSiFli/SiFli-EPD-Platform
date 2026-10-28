#include "epd_memory.h"
#include "app_package.h"
#include "epd_app.h"
#include "storage.h"
#include "storage_internal.h"
#include "storage_file.h"
#include "app_module.h"
#include <rthw.h>
#include "icons/ui_icons.h"
#include <cJSON.h>
#include <dfs_posix.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "app_package_io.h"

static bool path_join(char *path, size_t size, const char *root, const char *name)
{
    int n = snprintf(path, size, "%s/%s", root, name);
    return n > 0 && (size_t)n < size;
}

static bool relative_path(const char *path)
{
    if (!path || !*path || strlen(path) >= EPD_PACKAGE_PATH_MAX) return false;
    const char *part = path;
    unsigned depth = 1;
    for (const char *p = path; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p)
        {
            size_t length = p - part;
            if (!length || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!*p) return true;
            if (++depth > 8) return false;
            part = p + 1;
        }
    }
}

static cJSON *manifest_read(const char *root)
{
    char path[256];
    if (!path_join(path, sizeof(path), root, "app.json")) return NULL;
    int fd = package_io_open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (package_io_fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size >= SIZE_MAX)
    { package_io_close(fd); return NULL; }
    size_t size = (size_t)st.st_size, length = 0;
    char *text = epd_app_alloc(size + 1, EPD_APP_PSRAM);
    if (!text) { package_io_close(fd); return NULL; }
    while (length < size)
    {
        size_t amount = size - length;
        if (amount > 4096) amount = 4096;
        int n = package_io_read(fd, text + length, amount);
        if (n <= 0) break;
        length += n;
    }
    package_io_close(fd);
    text[length] = 0;
    cJSON *json = length == size ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    epd_app_free(text);
    return json;
}

static const char *string_value(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool locale_code_valid(const char *code)
{
    if (!code) return false;
    size_t size = strlen(code);
    if (size != 5 && size != 6) return false;
    for (size_t i = 0; i < size; ++i)
        if (i == size - 3 ? code[i] != '_' : (code[i] < 'a' || code[i] > 'z')) return false;
    return true;
}

static bool localized_names_valid(const cJSON *json)
{
    const cJSON *locale = cJSON_GetObjectItemCaseSensitive(json, "default_locale");
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(json, "localized_names");
    if (locale && (!cJSON_IsString(locale) || !locale_code_valid(locale->valuestring))) return false;
    if (!names) return true;
    if (!locale || !cJSON_IsObject(names)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, names)
    {
        if (!locale_code_valid(item->string) || !cJSON_IsString(item) ||
            !*item->valuestring || strlen(item->valuestring) >= 48) return false;
        for (const cJSON *previous = names->child; previous != item; previous = previous->next)
            if (!strcmp(previous->string, item->string)) return false;
    }
    return true;
}

static const char *localized_name(const cJSON *json, const char *fallback, const char *locale)
{
    if (!locale) return fallback;
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(json, "localized_names");
    const char *name = string_value(names, locale);
    const char *default_locale = string_value(json, "default_locale");
    if (!name && default_locale) name = string_value(names, default_locale);
    return name ? name : fallback;
}

static bool positive_integer(const cJSON *json, const char *key, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 1 && item->valuedouble <= UINT32_MAX)) return false;
    uint32_t number = (uint32_t)item->valuedouble;
    if (number != item->valuedouble) return false;
    if (value) *value = number;
    return true;
}

static bool content_extensions_valid(const cJSON *json, bool resources_only)
{
    const cJSON *extensions = cJSON_GetObjectItemCaseSensitive(json, "file_extensions");
    if (!extensions) return true;
    if (!cJSON_IsArray(extensions) || (resources_only && extensions->child)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, extensions)
    {
        if (!cJSON_IsString(item) || !item->valuestring[0]) return false;
        for (const char *p = item->valuestring; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
        for (const cJSON *previous = extensions->child; previous != item; previous = previous->next)
            if (!strcmp(previous->valuestring, item->valuestring)) return false;
    }
    return true;
}

bool app_package_handles_path(const char *root, const char *path)
{
    if (!path) return false;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *extension = strrchr(name, '.');
    if (!extension || extension == name || !*++extension) return false;
    cJSON *json = manifest_read(root);
    if (!json) return false;
    const char *kind = string_value(json, "kind");
    bool found = false;
    if ((!kind || !strcmp(kind, "module")) && content_extensions_valid(json, false))
    {
        const cJSON *item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "file_extensions"))
        {
            const char *a = extension, *b = item->valuestring;
            while (*a && *b)
            {
                char lower = *a >= 'A' && *a <= 'Z' ? *a + ('a' - 'A') : *a;
                if (lower != *b) break;
                ++a; ++b;
            }
            if (!*a && !*b) { found = true; break; }
        }
    }
    cJSON_Delete(json);
    return found;
}

bool app_package_metadata_locale(const char *root, const char *id, epd_app_entry_t *entry, const char *locale)
{
    cJSON *json = manifest_read(root);
    if (!json) return false;
    const char *name = string_value(json, "name");
    const char *kind = string_value(json, "kind");
    bool resources_only = kind && !strcmp(kind, "resources");
    const cJSON *lockscreen = cJSON_GetObjectItemCaseSensitive(json, "lockscreen");
    bool ok = name && *name && strlen(name) < sizeof(entry->name) &&
              (!lockscreen || (cJSON_IsBool(lockscreen) && (!resources_only || !cJSON_IsTrue(lockscreen)))) &&
              content_extensions_valid(json, resources_only) && localized_names_valid(json);
    char path[256], program[RT_NAME_MAX + 4];
    snprintf(program, sizeof(program), "%s.so", id);
    struct stat st;
    ok = ok && (!kind || !strcmp(kind, "module") || resources_only) &&
         (resources_only || (path_join(path, sizeof(path), root, program) &&
         package_io_stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 52));
    if (ok)
    {
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->id, sizeof(entry->id), "%s", id);
        snprintf(entry->name, sizeof(entry->name), "%s", localized_name(json, name, locale));
        entry->icon = &ui_icon_album;
        const char *icon = string_value(json, "icon");
        if (icon && relative_path(icon))
            snprintf(entry->icon_file, sizeof(entry->icon_file), "%s", icon);
        entry->has_settings = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "settings"));
        entry->lockscreen = cJSON_IsTrue(lockscreen);
        entry->resources_only = resources_only;
        entry->background = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "background"));
        const cJSON *extensions = cJSON_GetObjectItemCaseSensitive(json, "file_extensions");
        entry->handles_content = extensions && extensions->child;
        positive_integer(json, "version", &entry->version);
        positive_integer(json, "data_version", &entry->data_version);
        const char *build = string_value(json, "build_id");
        if (build) snprintf(entry->build_id, sizeof(entry->build_id), "%s", build);
    }
    cJSON_Delete(json);
    return ok;
}

bool app_package_metadata(const char *root, const char *id, epd_app_entry_t *entry)
{
    return app_package_metadata_locale(root, id, entry, NULL);
}

static bool hash_text(const char *text)
{
    if (!text || strlen(text) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

typedef struct
{
    void *buffer;
    uint64_t completed, total;
    app_package_progress_t progress;
    void *user;
    const app_package_record_t *records;
    uint32_t count, next;
    const char *root;
} hash_io_t;

static bool file_hash(const char *path, const char *expected, hash_io_t *io)
{
    unsigned char result[32];
    if (io->records)
    {
        if (io->next >= io->count) return false;
        const app_package_record_t *record = &io->records[io->next++];
        size_t prefix = strlen(io->root);
        if (strncmp(path, io->root, prefix) || path[prefix] != '/' ||
            strcmp(path + prefix + 1, record->name)) return false;
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < sizeof(record->digest); ++i)
            if (expected[i * 2] != hex[record->digest[i] >> 4] ||
                expected[i * 2 + 1] != hex[record->digest[i] & 15]) return false;
        io->completed += record->size;
        return true;
    }
    int fd = package_io_open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = package_io_fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    ok = ok && mbedtls_sha256_starts_ret(&hash, 0) == 0;
    int n = 0;
    while (ok && storage_path_available(path) && (n = package_io_read(fd, io->buffer, EPD_PACKAGE_IO_SIZE)) > 0)
    {
        ok = mbedtls_sha256_update_ret(&hash, io->buffer, n) == 0;
        io->completed += n;
        if (io->progress) io->progress(io->completed, io->total, io->user);
    }
    ok = ok && n == 0 && storage_path_available(path) && mbedtls_sha256_finish_ret(&hash, result) == 0;
    mbedtls_sha256_free(&hash);
    if (package_io_close(fd) != 0) ok = false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; ok && i < sizeof(result); ++i)
        ok = expected[i * 2] == hex[result[i] >> 4] && expected[i * 2 + 1] == hex[result[i] & 15];
    return ok;
}

static uint32_t get_u32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned get_u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static bool elf_valid(const char *path, uint32_t expected_span, size_t *load_peak)
{
    int fd = package_io_open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char header[52], segment[32];
    struct stat st;
    bool ok = package_io_fstat(fd, &st) == 0 && st.st_size >= 52 && package_io_read(fd, header, sizeof(header)) == sizeof(header);
    ok = ok && !memcmp(header, "\177ELF\1\1\1", 7) && get_u16(header + 16) == 3 &&
         get_u16(header + 18) == 40 && get_u16(header + 42) == sizeof(segment);
    uint32_t start = 0, end = 0;
    bool found = false;
    if (ok)
    {
        uint32_t phoff = get_u32(header + 28);
        unsigned count = get_u16(header + 44);
        ok = (uint64_t)phoff + (uint64_t)count * sizeof(segment) <= (uint64_t)st.st_size &&
             package_io_lseek(fd, phoff, SEEK_SET) == phoff;
        for (unsigned i = 0; ok && i < count; ++i)
        {
            ok = package_io_read(fd, segment, sizeof(segment)) == sizeof(segment);
            if (!ok || get_u32(segment) != 1) continue;
            uint32_t offset = get_u32(segment + 4), address = get_u32(segment + 8);
            uint32_t file_size = get_u32(segment + 16), memory_size = get_u32(segment + 20);
            ok = file_size <= memory_size && (uint64_t)offset + file_size <= (uint64_t)st.st_size &&
                 (uint64_t)address + memory_size <= UINT32_MAX && (!found || address >= end);
            if (!found) start = address;
            end = address + memory_size;
            found = true;
        }
    }
    package_io_close(fd);
    if (!ok || !found || end <= start || end - start != expected_span) return false;
    if (load_peak)
    {
        /* Cache reclamation hint, not an allocation admission check. */
        uint64_t bytes = (uint64_t)st.st_size + expected_span;
        *load_peak = bytes > SIZE_MAX ? SIZE_MAX : (size_t)bytes;
    }
    return true;
}

static bool file_count(const char *root, unsigned depth, uint64_t *count, uint64_t *bytes)
{
    if (depth > 8) return false;
    DIR *dir = package_io_opendir(root);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = package_io_readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[256];
        struct stat st;
        ok = path_join(path, sizeof(path), root, entry->d_name) && package_io_stat(path, &st) == 0;
        if (!ok) break;
        if (S_ISDIR(st.st_mode)) ok = file_count(path, depth + 1, count, bytes);
        else if (S_ISREG(st.st_mode))
        {
            ok = *count != UINT64_MAX && st.st_size >= 0 &&
                 (uint64_t)st.st_size <= UINT64_MAX - *bytes;
            if (ok) { ++*count; *bytes += st.st_size; }
        }
        else ok = false;
    }
    package_io_closedir(dir);
    return ok;
}

static bool library_name(const char *name)
{
    if (!name || !*name || strlen(name) >= RT_NAME_MAX) return false;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

bool app_package_revision(const char *root, const char *id, const char *library,
                          app_package_revision_t *revision)
{
    if (!root || !library_name(id) || (library && !library_name(library))) return false;
    storage_volume_t volume = !strcmp(storage_path_root(root), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    memset(revision, 0, sizeof(*revision));
    revision->session = volume == STORAGE_SD ? storage_card_session() : 0;
    revision->state = volume == STORAGE_SD ? storage_revision() : storage_available(volume);
    if (storage_release_requested() || storage_changing() || !storage_apps_enabled(volume) ||
        !storage_session_valid(volume, revision->session)) return false;
    char code[EPD_PACKAGE_PATH_MAX], path[256];
    snprintf(code, sizeof(code), library ? "codecs/%s.so" : "%s.so", library ? library : id);
    const char *names[] = {"", "app.json", "files.sha256", code, "codecs"};
    for (unsigned i = 0; i < 5; ++i)
    {
        if (*names[i] ? !path_join(path, sizeof(path), root, names[i]) : strlen(root) >= sizeof(path)) return false;
        if (!*names[i]) strcpy(path, root);
        if (storage_file_writers(path)) return false;
        revision->paths[i] = storage_path_revision(path);
    }
    strcpy(path, root);
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = 0;
    if (storage_file_writers(path)) return false;
    revision->paths[5] = storage_path_revision(path);
    return revision->state == (volume == STORAGE_SD ? storage_revision() : storage_available(volume)) &&
           (volume != STORAGE_SD || revision->session == storage_card_session()) &&
           storage_session_valid(volume, revision->session) && !storage_release_requested() && !storage_changing();
}

typedef struct
{
    bool valid;
    char root[256], id[RT_NAME_MAX], library[RT_NAME_MAX];
    app_package_revision_t revision;
    size_t peak;
} verification_cache_t;
static verification_cache_t verification_cache[8];
static unsigned verification_next;

static app_package_status_t package_verify(const char *root, const char *id, bool all_files,
                                           const char *requested, size_t *load_peak,
                                           app_package_progress_t progress, void *user,
                                           const app_package_record_t *records, uint32_t record_count)
{
    hash_io_t io = {0};
    io.records = records; io.count = record_count; io.next = 1; io.root = root;
    cJSON *json = manifest_read(root);
    if (!json) return APP_PACKAGE_FORMAT_ERROR;
    app_package_status_t status = APP_PACKAGE_FORMAT_ERROR;
    uint32_t format, abi, load_bytes = 0;
    const char *kind = string_value(json, "kind");
    bool resources_only = kind && !strcmp(kind, "resources");
    const char *declared_id = string_value(json, "id");
    const char *build = string_value(json, "build_id");
    const char *digest = string_value(json, "files_sha256");
    const char *icon = string_value(json, "icon");
    const char *name = string_value(json, "name");
    const cJSON *icon_item = cJSON_GetObjectItemCaseSensitive(json, "icon");
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(json, "libraries");
    const cJSON *lockscreen = cJSON_GetObjectItemCaseSensitive(json, "lockscreen");
    unsigned library_count = 0, library_seen = 0;
    if (requested && !library_name(requested)) goto finish;
    if (libraries)
    {
        if (!cJSON_IsObject(libraries) || resources_only) goto finish;
        const cJSON *item;
        cJSON_ArrayForEach(item, libraries)
        {
            if (library_count == UINT_MAX || !library_name(item->string) ||
                !strcmp(item->string, id) || !positive_integer(libraries, item->string, NULL)) goto finish;
            ++library_count;
            for (const cJSON *previous = libraries->child; previous != item; previous = previous->next)
                if (!strcmp(previous->string, item->string)) goto finish;
        }
    }
    if (requested && !positive_integer(libraries, requested, NULL)) goto finish;
    if (!positive_integer(json, "package_format", &format) || format != EPD_PACKAGE_FORMAT ||
        !positive_integer(json, "abi", &abi) || !positive_integer(json, "version", NULL) ||
        !positive_integer(json, "data_version", NULL) ||
        (!resources_only && !positive_integer(json, "load_bytes", &load_bytes)) ||
        (kind && strcmp(kind, "module") && !resources_only) ||
        !name || !*name || strlen(name) >= 48 || !declared_id || strcmp(declared_id, id) ||
        !hash_text(build) || !hash_text(digest) || (icon_item && !cJSON_IsString(icon_item)) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(json, "settings")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(json, "background")) ||
        (lockscreen && (!cJSON_IsBool(lockscreen) || (resources_only && cJSON_IsTrue(lockscreen)))) ||
        (icon && !relative_path(icon)) || !content_extensions_valid(json, resources_only) ||
        !localized_names_valid(json) ||
        (resources_only && (!icon || !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "settings")) ||
                             !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "background"))))) goto finish;
    status = APP_PACKAGE_INCOMPATIBLE;
    if (abi != EPD_APP_ABI) goto finish;
    status = APP_PACKAGE_INTEGRITY_ERROR;
    char path[256], program[RT_NAME_MAX + 4];
    uint64_t actual = 0;
    if (records)
    {
        if (record_count < 2 || strcmp(records[0].name, "app.json") ||
            strcmp(records[1].name, "files.sha256")) goto finish;
        actual = record_count;
        for (uint32_t i = 1; i < record_count; ++i)
        {
            if (!memchr(records[i].name, 0, sizeof(records[i].name)) ||
                !relative_path(records[i].name) || records[i].size > UINT64_MAX - io.total) goto finish;
            io.total += records[i].size;
        }
    }
    else if (all_files)
    {
        struct stat manifest;
        if (!file_count(root, 0, &actual, &io.total) ||
            !path_join(path, sizeof(path), root, "app.json") || package_io_stat(path, &manifest) != 0 ||
            manifest.st_size < 0 || (uint64_t)manifest.st_size > io.total) goto finish;
        io.total -= manifest.st_size;
    }
    if (!records)
    {
        io.buffer = epd_app_alloc(EPD_PACKAGE_IO_SIZE, EPD_APP_PSRAM);
        if (!io.buffer) { status = APP_PACKAGE_IO_ERROR; goto finish; }
    }
    io.progress = progress;
    io.user = user;
    if (progress) progress(0, io.total, user);
    if (!path_join(path, sizeof(path), root, "files.sha256") || !file_hash(path, digest, &io)) goto finish;
    FILE *index = package_io_fopen(path, "rb");
    if (!index) { status = APP_PACKAGE_IO_ERROR; goto finish; }
    char line[64 + 2 + EPD_PACKAGE_PATH_MAX + 2], previous[EPD_PACKAGE_PATH_MAX] = {0};
    uint64_t count = 0;
    bool ok = true, has_program = resources_only, has_icon = !icon;
    snprintf(program, sizeof(program), "%s.so", id);
    while (ok && package_io_fgets(line, sizeof(line), index))
    {
        size_t length = strlen(line);
        if (length < 68 || line[length - 1] != '\n' || line[64] != ' ' || line[65] != ' ')
        { ok = false; break; }
        line[length - 1] = 0;
        line[64] = 0;
        const char *name = line + 66;
        ok = count < UINT64_MAX - 2 && hash_text(line) && relative_path(name) &&
             strcmp(previous, name) < 0 && strcmp(name, "app.json") && strcmp(name, "files.sha256") &&
             path_join(path, sizeof(path), root, name);
        if (!ok) break;
        ++count;
        size_t name_length = strlen(name);
        if (name_length >= sizeof(previous)) { ok = false; break; }
        memcpy(previous, name, name_length + 1);
        bool is_program = !strcmp(name, program);
        const char *selected_library = NULL;
        uint32_t library_bytes = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, libraries)
        {
            char filename[32];
            snprintf(filename, sizeof(filename), "codecs/%s.so", item->string);
            if (!strcmp(name, filename))
            {
                selected_library = item->string;
                library_bytes = (uint32_t)item->valuedouble;
                /* Unique manifest names and sorted unique file paths ensure
                 * that each declared library contributes exactly once. */
                ++library_seen;
                break;
            }
        }
        if (resources_only && strcmp(name, icon) && strncmp(name, "res/", 4))
        { ok = false; break; }
        bool check_library = selected_library && (all_files ||
                             (requested && !strcmp(requested, selected_library)));
        if (all_files || (is_program && !requested) || check_library) ok = file_hash(path, line, &io);
        if (is_program)
        {
            has_program = true;
            ok = !resources_only && ok && (requested || elf_valid(path, load_bytes, load_peak));
        }
        if (check_library) ok = ok && elf_valid(path, library_bytes,
                              requested && !strcmp(requested, selected_library) ? load_peak : NULL);
        if (icon && !strcmp(name, icon)) has_icon = true;
    }
    ok = ok && !ferror(index) && has_program && has_icon &&
         library_seen == library_count;
    package_io_fclose(index);
    if (ok && all_files) ok = actual == count + 2 && io.completed == io.total &&
                             (!records || io.next == record_count);
    if (ok) status = APP_PACKAGE_OK;
finish:
    epd_app_free(io.buffer);
    cJSON_Delete(json);
    return status;
}

static app_package_status_t verify_cached(const char *root, const char *id, const char *library,
                                          size_t *load_peak)
{
    if (load_peak) *load_peak = 0;
    app_package_revision_t before, after;
    if (!app_package_revision(root, id, library, &before)) return APP_PACKAGE_IO_ERROR;
    int resident = app_module_verified(root, id, library, load_peak);
    if (resident) return resident > 0 ? APP_PACKAGE_OK : APP_PACKAGE_IO_ERROR;
    bool hit = false;
    size_t cached_peak = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < sizeof(verification_cache) / sizeof(verification_cache[0]); ++i)
    {
        verification_cache_t *cached = &verification_cache[i];
        if (cached->valid && !strcmp(cached->root, root) && !strcmp(cached->id, id) &&
            !strcmp(cached->library, library ? library : "") &&
            !memcmp(&cached->revision, &before, sizeof(before)))
        {
            cached_peak = cached->peak;
            hit = true;
            break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (hit)
    {
        if (!app_package_revision(root, id, library, &after) || memcmp(&before, &after, sizeof(before)))
            return APP_PACKAGE_IO_ERROR;
        if (load_peak) *load_peak = cached_peak;
        return APP_PACKAGE_OK;
    }
    size_t peak = 0;
    app_package_status_t status = package_verify(root, id, false, library, &peak, NULL, NULL, NULL, 0);
    if (!app_package_revision(root, id, library, &after) || memcmp(&before, &after, sizeof(before)))
        return APP_PACKAGE_IO_ERROR;
    if (status == APP_PACKAGE_OK)
    {
        verification_cache_t cached = {0};
        strcpy(cached.root, root); strcpy(cached.id, id);
        strcpy(cached.library, library ? library : "");
        cached.revision = after; cached.peak = peak; cached.valid = true;
        level = rt_hw_interrupt_disable();
        verification_cache[verification_next++ % 8] = cached;
        rt_hw_interrupt_enable(level);
        if (load_peak) *load_peak = peak;
    }
    return status;
}

app_package_status_t app_package_verify(const char *root, const char *id, bool all_files,
                                        size_t *load_peak)
{
    if (load_peak) *load_peak = 0;
    return all_files ? package_verify(root, id, true, NULL, load_peak, NULL, NULL, NULL, 0) :
                      verify_cached(root, id, NULL, load_peak);
}

app_package_status_t app_package_verify_progress(const char *root, const char *id,
                                                app_package_progress_t progress, void *user)
{
    return package_verify(root, id, true, NULL, NULL, progress, user, NULL, 0);
}

app_package_status_t app_package_verify_extracted(const char *root, const char *id,
                                                  const app_package_record_t *records, uint32_t count)
{
    if (!records || count < 2 || !memchr(records[0].name, 0, sizeof(records[0].name)) ||
        !memchr(records[1].name, 0, sizeof(records[1].name))) return APP_PACKAGE_FORMAT_ERROR;
    return package_verify(root, id, true, NULL, NULL, NULL, NULL, records, count);
}

app_package_status_t app_package_verify_library(const char *root, const char *id,
                                               const char *name, size_t *load_peak)
{
    if (load_peak) *load_peak = 0;
    return name ? verify_cached(root, id, name, load_peak) : APP_PACKAGE_FORMAT_ERROR;
}

const char *app_package_error(app_package_status_t status)
{
    switch (status)
    {
    case APP_PACKAGE_OK: return "ok";
    case APP_PACKAGE_FORMAT_ERROR: return "manifest format";
    case APP_PACKAGE_INCOMPATIBLE: return "firmware profile";
    case APP_PACKAGE_IO_ERROR: return "file access";
    default: return "file integrity or ELF layout";
    }
}
