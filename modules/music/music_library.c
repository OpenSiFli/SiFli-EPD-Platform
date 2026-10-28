#include "music_library.h"
#include "storage_file.h"
#include "audio_mp3ctrl.h"
#include <dfs_posix.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct music_library_scan {
    DIR *directory;
    char *path;
    uint32_t session;
    bool card, complete;
};

static unsigned char lower_ascii(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c; }

static bool utf8_valid(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p)
    {
        uint32_t value;
        unsigned count;
        if (*p < 0x80) { ++p; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { value = *p++ & 31; count = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { value = *p++ & 15; count = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { value = *p++ & 7; count = 3; }
        else return false;
        unsigned bytes = count;
        while (count--)
        {
            if ((*p & 0xc0) != 0x80) return false;
            value = (value << 6) | (*p++ & 63);
        }
        if ((bytes == 2 && value < 0x800) || (bytes == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool equal_ascii(const char *a, const char *b)
{
    while (*a && *b && lower_ascii((unsigned char)*a) == lower_ascii((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}

static bool contains_ascii(const char *text, const char *query)
{
    if (!text) return false;
    for (; *text; ++text)
    {
        const char *a = text, *b = query;
        while (*a && *b && lower_ascii((unsigned char)*a) == lower_ascii((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return !*query;
}

bool music_library_matches(const char *filename, const char *title, const char *query)
{ return !query || !*query || contains_ascii(filename, query) || contains_ascii(title, query); }

int music_library_natural_compare(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left, *b = (const unsigned char *)right;
    while (*a && *b)
    {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9')
        {
            const unsigned char *ae = a, *be = b, *az = a, *bz = b;
            while (*ae >= '0' && *ae <= '9') ++ae;
            while (*be >= '0' && *be <= '9') ++be;
            while (az < ae && *az == '0') ++az;
            while (bz < be && *bz == '0') ++bz;
            if (ae - az != be - bz) return ae - az < be - bz ? -1 : 1;
            int cmp = memcmp(az, bz, (size_t)(ae - az));
            if (cmp) return cmp;
            a = ae; b = be;
        }
        else
        {
            unsigned char ac = lower_ascii(*a++), bc = lower_ascii(*b++);
            if (ac != bc) return ac < bc ? -1 : 1;
        }
    }
    if (*a || *b) return *a ? 1 : -1;
    return 0;
}

music_format_t music_library_extension(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return MUSIC_FORMAT_UNKNOWN;
    if (equal_ascii(dot, ".mp3")) return MUSIC_FORMAT_MP3;
    if (equal_ascii(dot, ".wav")) return MUSIC_FORMAT_WAV;
    if (equal_ascii(dot, ".flac")) return MUSIC_FORMAT_FLAC;
    if (equal_ascii(dot, ".m3u8")) return MUSIC_FORMAT_M3U8;
    return MUSIC_FORMAT_UNKNOWN;
}

music_model_result_t music_library_resolve(const char *base, const char *path, char **out)
{
    if (!path || !*path || !out || !utf8_valid(path) || (base && !utf8_valid(base)))
        return MUSIC_MODEL_INVALID;
    size_t n = strlen(path), prefix = path[0] == '/' ? 0 : (base ? strlen(base) : 0);
    if ((path[0] == '/' && path[1] == '/') || (!prefix && path[0] != '/') ||
        (prefix && base[0] != '/')) return MUSIC_MODEL_INVALID;
    size_t separator = prefix && base[prefix - 1] != '/' ? 1 : 0;
    if (n >= STORAGE_PATH_MAX || prefix >= STORAGE_PATH_MAX || n + prefix + separator >= STORAGE_PATH_MAX)
        return MUSIC_MODEL_RANGE;
    char joined[STORAGE_PATH_MAX], normalized[STORAGE_PATH_MAX];
    size_t used = 0;
    if (prefix) { memcpy(joined, base, prefix); used = prefix; if (joined[used - 1] != '/') joined[used++] = '/'; }
    memcpy(joined + used, path, n + 1);
    for (const char *p = joined; *p; ++p)
        if ((unsigned char)*p < 32 || *p == '\\' || *p == ':' || (unsigned char)*p == 127)
            return MUSIC_MODEL_INVALID;
    used = 1; normalized[0] = '/';
    const char *p = joined;
    while (*p)
    {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t length = (size_t)(p - start);
        if (!length || (length == 1 && start[0] == '.')) continue;
        if (length == 2 && start[0] == '.' && start[1] == '.')
        {
            if (used == 1) return MUSIC_MODEL_INVALID;
            while (used > 1 && normalized[used - 1] != '/') --used;
            if (used > 1) --used;
            continue;
        }
        if (used > 1) normalized[used++] = '/';
        memcpy(normalized + used, start, length); used += length;
    }
    normalized[used] = 0;
    char *copy = music_model_strdup(normalized);
    if (!copy) return MUSIC_MODEL_MEMORY;
    *out = copy;
    return MUSIC_MODEL_OK;
}

static const char *basename_of(const char *path)
{ const char *slash = strrchr(path, '/'); return slash ? slash + 1 : path; }

static music_model_result_t parent_of(const char *path, char **out)
{
    char *normalized = NULL;
    music_model_result_t rc = music_library_resolve(NULL, path, &normalized);
    if (rc != MUSIC_MODEL_OK) return rc;
    char *slash = strrchr(normalized, '/');
    if (slash == normalized) slash[1] = 0; else *slash = 0;
    *out = normalized;
    return MUSIC_MODEL_OK;
}

void music_library_free(music_library_t *library)
{
    for (size_t i = 0; i < library->count; ++i)
    { music_model_free(library->entries[i].path); music_model_free(library->entries[i].name); }
    music_model_free(library->entries);
    memset(library, 0, sizeof(*library));
}

static int entry_compare(const void *a, const void *b)
{
    const music_library_entry_t *left = a, *right = b;
    if (left->directory != right->directory) return left->directory ? -1 : 1;
    int rc = music_library_natural_compare(left->name, right->name);
    return rc ? rc : strcmp(left->path, right->path);
}

static music_model_result_t add_entry(music_library_t *list, char *path, bool directory)
{
    for (size_t i = 0; i < list->count; ++i) if (!strcmp(list->entries[i].path, path)) return MUSIC_MODEL_END;
    char *name = music_model_strdup(basename_of(path));
    if (!name) return MUSIC_MODEL_MEMORY;
    if (list->count == list->capacity)
    {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*list->entries))
        { music_model_free(name); return MUSIC_MODEL_RANGE; }
        void *copy = music_model_realloc(list->entries, capacity * sizeof(*list->entries));
        if (!copy) { music_model_free(name); return MUSIC_MODEL_MEMORY; }
        list->entries = copy; list->capacity = capacity;
    }
    music_library_entry_t entry = {path, name, directory, music_library_extension(path)};
    list->entries[list->count++] = entry;
    return MUSIC_MODEL_OK;
}

music_model_result_t music_library_scan_begin(const char *directory, music_library_scan_t **out)
{
    if (!out) return MUSIC_MODEL_INVALID;
    char *path = NULL;
    music_model_result_t rc = music_library_resolve(NULL, directory, &path);
    if (rc != MUSIC_MODEL_OK) return rc;
    music_library_scan_t *scan = music_model_alloc(sizeof(*scan));
    if (!scan) { music_model_free(path); return MUSIC_MODEL_MEMORY; }
    memset(scan, 0, sizeof(*scan)); scan->path = path;
    scan->card = !strcmp(storage_path_root(path), STORAGE_SD_ROOT);
    scan->session = storage_card_session();
    if (!storage_path_available(path) || !(scan->directory = opendir(path)))
    { music_library_scan_close(scan); return MUSIC_MODEL_IO; }
    *out = scan;
    return MUSIC_MODEL_OK;
}

music_model_result_t music_library_scan_step(music_library_scan_t *scan, music_library_t *out, size_t budget)
{
    if (!scan || !out || !budget) return MUSIC_MODEL_INVALID;
    if (scan->complete) return MUSIC_MODEL_END;
    while (budget--)
    {
        if (!storage_path_available(scan->path) ||
            (scan->card && !storage_session_valid(STORAGE_SD, scan->session))) return MUSIC_MODEL_CHANGED;
        errno = 0;
        struct dirent *entry = readdir(scan->directory);
        if (!entry)
        {
            if (errno) return MUSIC_MODEL_IO;
            if (out->count > 1) qsort(out->entries, out->count, sizeof(*out->entries), entry_compare);
            scan->complete = true;
            return MUSIC_MODEL_END;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *path = NULL;
        music_model_result_t rc = music_library_resolve(scan->path, entry->d_name, &path);
        if (rc != MUSIC_MODEL_OK) return rc;
        struct stat st;
        if (stat(path, &st) != 0) { music_model_free(path); return MUSIC_MODEL_IO; }
        bool directory = S_ISDIR(st.st_mode);
        if (!directory && (!S_ISREG(st.st_mode) || music_library_extension(path) == MUSIC_FORMAT_UNKNOWN))
        { music_model_free(path); continue; }
        rc = add_entry(out, path, directory);
        if (rc != MUSIC_MODEL_OK) music_model_free(path);
        if (rc < 0) return rc;
    }
    return MUSIC_MODEL_OK;
}

void music_library_scan_close(music_library_scan_t *scan)
{
    if (!scan) return;
    if (scan->directory) closedir(scan->directory);
    music_model_free(scan->path); music_model_free(scan);
}

music_model_result_t music_library_read(const char *directory, music_library_t *out)
{
    music_library_scan_t *scan = NULL;
    music_library_t list = {0};
    music_model_result_t rc = music_library_scan_begin(directory, &scan);
    if (rc != MUSIC_MODEL_OK) return rc;
    do { rc = music_library_scan_step(scan, &list, 32); } while (rc == MUSIC_MODEL_OK);
    music_library_scan_close(scan);
    if (rc == MUSIC_MODEL_END) { music_library_free(out); *out = list; return MUSIC_MODEL_OK; }
    music_library_free(&list); return rc;
}

music_model_result_t music_library_stat(const char *path, bool signature, music_file_stamp_t *out)
{
    struct stat st, after;
    music_file_stamp_t stamp = {0};
    if (!path || !out) return MUSIC_MODEL_INVALID;
    if (!storage_path_available(path) || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
        return MUSIC_MODEL_IO;
    stamp.size = (uint64_t)st.st_size; stamp.mtime = (int64_t)st.st_mtime;
    if (signature)
    {
        if (!storage_file_signature(path, &stamp.signature) || stat(path, &after) ||
            st.st_size != after.st_size || st.st_mtime != after.st_mtime) return MUSIC_MODEL_IO;
        stamp.signature_valid = true;
    }
    stamp.valid = true; *out = stamp;
    return MUSIC_MODEL_OK;
}

bool music_library_stamp_equal(const music_file_stamp_t *a, const music_file_stamp_t *b)
{
    return a->valid && b->valid && a->size == b->size && a->mtime == b->mtime &&
           a->signature_valid == b->signature_valid &&
           (!a->signature_valid || a->signature == b->signature);
}

static bool read_exact(int fd, void *buffer, size_t size)
{
    unsigned char *p = buffer;
    while (size)
    {
        size_t chunk = size > INT_MAX ? INT_MAX : size;
        int n = (int)read(fd, p, chunk);
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}

static bool seek_to(int fd, uint64_t offset)
{
    off_t target = (off_t)offset;
    return target >= 0 && (uint64_t)target == offset && lseek(fd, target, SEEK_SET) == target;
}

static uint32_t le32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t be32(const unsigned char *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Read only ID3's length and the first MPEG frame. Text remains SDK-owned. */
static music_model_result_t mp3_header_duration(const char *path, music_metadata_t *m)
{
    static const unsigned rates[] = {44100, 48000, 32000};
    static const unsigned bitrates[2][15] = {
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}
    };
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return MUSIC_MODEL_IO;
    unsigned char header[10], *frame = NULL;
    uint64_t start = 0, bytes = m->stamp.size;
    music_model_result_t rc = MUSIC_MODEL_END;
    if (bytes < sizeof(header)) goto done;
    if (!read_exact(fd, header, sizeof(header))) { rc = MUSIC_MODEL_IO; goto done; }
    if (!memcmp(header, "ID3", 3))
    {
        if ((header[6] | header[7] | header[8] | header[9]) & 128) goto done;
        if (header[3] < 2 || header[3] > 4) goto done;
        start = ((uint32_t)header[6] << 21) | ((uint32_t)header[7] << 14) |
                ((uint32_t)header[8] << 7) | header[9];
        start += 10 + (header[3] == 4 && (header[5] & 16) ? 10 : 0);
        if (start > bytes || bytes - start < 4) goto done;
        if (!seek_to(fd, start) || !read_exact(fd, header, 4)) { rc = MUSIC_MODEL_IO; goto done; }
    }
    unsigned version = (header[1] >> 3) & 3, rate_index = (header[2] >> 2) & 3;
    unsigned bitrate_index = header[2] >> 4, channels = (header[3] >> 6) == 3 ? 1 : 2;
    if (header[0] != 255 || (header[1] & 0xe0) != 0xe0 || (header[1] & 6) != 2 ||
        version == 1 || rate_index == 3 || !bitrate_index || bitrate_index == 15) goto done;
    unsigned low_rate = version != 3;
    unsigned rate = rates[rate_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    unsigned coefficient = low_rate ? 72000 : 144000;
    size_t frame_size = coefficient * bitrates[low_rate][bitrate_index] / rate + ((header[2] >> 1) & 1);
    if (frame_size > bytes - start || frame_size < 4) goto done;
    frame = music_model_alloc(frame_size);
    if (!frame) { rc = MUSIC_MODEL_MEMORY; goto done; }
    if (!seek_to(fd, start) || !read_exact(fd, frame, frame_size)) { rc = MUSIC_MODEL_IO; goto done; }
    size_t tag = 4 + (low_rate ? (channels == 1 ? 9 : 17) : (channels == 1 ? 17 : 32));
    uint32_t frames = 0, stream_bytes = 0;
    if (tag <= frame_size && frame_size - tag >= 12 &&
        (!memcmp(frame + tag, "Xing", 4) || !memcmp(frame + tag, "Info", 4)))
    {
        uint32_t flags = be32(frame + tag + 4);
        size_t required = 8 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 4 : 0) +
                          ((flags & 4) ? 100 : 0) + ((flags & 8) ? 4 : 0);
        if (!(flags & 1) || (flags & ~15u) || required > frame_size - tag) goto done;
        frames = be32(frame + tag + 8);
        if (flags & 2)
        {
            stream_bytes = be32(frame + tag + 12);
            if (!stream_bytes) goto done;
        }
    }
    else if (frame_size >= 62 && !memcmp(frame + 36, "VBRI", 4))
    {
        if (frame[40] != 0 || frame[41] != 1) goto done;
        stream_bytes = be32(frame + 46); frames = be32(frame + 50);
        if (!stream_bytes) goto done;
    }
    else goto done;
    if (!frames || (stream_bytes && (stream_bytes < frame_size || stream_bytes > bytes - start))) goto done;
    uint64_t available = stream_bytes ? stream_bytes : bytes - start;
    unsigned minimum_frame = coefficient * bitrates[low_rate][1] / rate;
    if (frames > available / minimum_frame) goto done;
    m->duration_ms = (uint64_t)frames * (low_rate ? 576 : 1152) * 1000 / rate;
    m->duration_estimated = false;
    m->sample_rate = rate; m->channels = channels; m->bits_per_sample = 16;
    rc = MUSIC_MODEL_OK;
done:
    music_model_free(frame);
    if (close(fd) != 0) rc = MUSIC_MODEL_IO;
    return rc;
}

music_model_result_t music_library_probe(const char *path, music_format_t *format)
{
    if (!path || !format) return MUSIC_MODEL_INVALID;
    if (!storage_path_available(path)) return MUSIC_MODEL_IO;
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return MUSIC_MODEL_IO;
    unsigned char header[12];
    int count = (int)read(fd, header, sizeof(header));
    music_format_t result = MUSIC_FORMAT_UNKNOWN;
    if (count >= 4 && !memcmp(header, "fLaC", 4)) result = MUSIC_FORMAT_FLAC;
    else if (count >= 12 && !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4)) result = MUSIC_FORMAT_WAV;
    else
    {
        if (count >= 10 && !memcmp(header, "ID3", 3))
        {
            if (!(header[6] & 128) && !(header[7] & 128) && !(header[8] & 128) && !(header[9] & 128))
            {
                uint64_t skip = ((uint32_t)header[6] << 21) | ((uint32_t)header[7] << 14) |
                                ((uint32_t)header[8] << 7) | header[9];
                skip += 10 + (header[3] == 4 && (header[5] & 16) ? 10 : 0);
                count = seek_to(fd, skip) ? (int)read(fd, header, 4) : -1;
            }
            else count = 0;
        }
        if (count >= 4 && header[0] == 255 && (header[1] & 0xe0) == 0xe0 &&
            (header[1] & 6) == 2 && (header[1] & 24) != 8 &&
            (header[2] & 0xf0) != 0xf0 && (header[2] & 12) != 12)
            result = MUSIC_FORMAT_MP3;
    }
    bool ok = close(fd) == 0;
    if (count < 0 || !ok) return MUSIC_MODEL_IO;
    if (result == MUSIC_FORMAT_UNKNOWN) return MUSIC_MODEL_FORMAT;
    *format = result;
    return MUSIC_MODEL_OK;
}

void music_metadata_free(music_metadata_t *m)
{
    music_model_free(m->path); music_model_free(m->title); music_model_free(m->artist);
    music_model_free(m->album); music_model_free(m->cover_path);
    memset(m, 0, sizeof(*m));
}

static music_model_result_t replace_text(char **out, const char *text)
{
    if (!text || !*text || !utf8_valid(text)) return MUSIC_MODEL_OK;
    char *copy = music_model_strdup(text);
    if (!copy) return MUSIC_MODEL_MEMORY;
    music_model_free(*out); *out = copy;
    return MUSIC_MODEL_OK;
}

static music_model_result_t flac_comments(int fd, uint64_t start, uint32_t length, music_metadata_t *m)
{
    unsigned char word[4];
    uint64_t end = start + length, offset = start;
    if (length < 8 || !read_exact(fd, word, 4)) return MUSIC_MODEL_FORMAT;
    uint32_t vendor = le32(word);
    offset += 4;
    if (vendor > end - offset || end - offset - vendor < 4) return MUSIC_MODEL_FORMAT;
    offset += vendor;
    if (!seek_to(fd, offset) || !read_exact(fd, word, 4)) return MUSIC_MODEL_IO;
    uint32_t count = le32(word); offset += 4;
    if (count > (end - offset) / 4) return MUSIC_MODEL_FORMAT;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (end - offset < 4 || !read_exact(fd, word, 4)) return MUSIC_MODEL_FORMAT;
        uint32_t n = le32(word); offset += 4;
        if (n > end - offset) return MUSIC_MODEL_FORMAT;
        char key[8] = {0}; size_t prefix = n < 7 ? n : 7;
        if (!read_exact(fd, key, prefix)) return MUSIC_MODEL_IO;
        for (size_t j = 0; j < prefix; ++j) key[j] = (char)lower_ascii((unsigned char)key[j]);
        char **target = NULL; size_t key_size = 0;
        if (n >= 6 && !memcmp(key, "title=", 6)) { target = &m->title; key_size = 6; }
        else if (n >= 7 && !memcmp(key, "artist=", 7)) { target = &m->artist; key_size = 7; }
        else if (n >= 6 && !memcmp(key, "album=", 6)) { target = &m->album; key_size = 6; }
        if (target)
        {
            size_t text_size = (size_t)n - key_size;
            if (text_size == SIZE_MAX) return MUSIC_MODEL_RANGE;
            char *text = music_model_alloc(text_size + 1);
            if (!text) return MUSIC_MODEL_MEMORY;
            if (!seek_to(fd, offset + key_size) || !read_exact(fd, text, text_size))
            { music_model_free(text); return MUSIC_MODEL_IO; }
            text[text_size] = 0;
            if (!memchr(text, 0, text_size) && utf8_valid(text) && *text)
            { music_model_free(*target); *target = text; }
            else music_model_free(text);
        }
        offset += n;
        if (!seek_to(fd, offset)) return MUSIC_MODEL_IO;
    }
    return MUSIC_MODEL_OK;
}

static music_model_result_t flac_metadata(const char *path, music_metadata_t *m)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return MUSIC_MODEL_IO;
    music_model_result_t rc = MUSIC_MODEL_OK;
    uint64_t offset = 4;
    bool last = false, streaminfo = false;
    while (!last)
    {
        unsigned char header[4], data[34];
        if (offset > m->stamp.size || m->stamp.size - offset < 4 || !seek_to(fd, offset) || !read_exact(fd, header, 4))
        { rc = MUSIC_MODEL_FORMAT; break; }
        last = (header[0] & 128) != 0;
        unsigned type = header[0] & 127;
        uint32_t length = (uint32_t)header[1] << 16 | (uint32_t)header[2] << 8 | header[3];
        offset += 4;
        if (length > m->stamp.size - offset || (!streaminfo && type != 0)) { rc = MUSIC_MODEL_FORMAT; break; }
        if (type == 0)
        {
            if (streaminfo || length != sizeof(data) || !read_exact(fd, data, sizeof(data))) { rc = MUSIC_MODEL_FORMAT; break; }
            streaminfo = true;
            m->sample_rate = (uint32_t)data[10] << 12 | (uint32_t)data[11] << 4 | data[12] >> 4;
            m->channels = ((data[12] >> 1) & 7) + 1;
            m->bits_per_sample = (((unsigned)data[12] & 1) << 4 | data[13] >> 4) + 1;
            uint64_t samples = ((uint64_t)data[13] & 15) << 32 | (uint64_t)data[14] << 24 |
                               (uint64_t)data[15] << 16 | (uint64_t)data[16] << 8 | data[17];
            if (!m->sample_rate) { rc = MUSIC_MODEL_FORMAT; break; }
            m->duration_ms = samples * 1000 / m->sample_rate;
        }
        else if (type == 4)
        {
            rc = flac_comments(fd, offset, length, m);
            /* Tags are optional; resource and I/O failures still reach callers. */
            if (rc == MUSIC_MODEL_FORMAT) rc = MUSIC_MODEL_OK;
            if (rc != MUSIC_MODEL_OK) break;
        }
        else if (type == 127) { rc = MUSIC_MODEL_FORMAT; break; }
        offset += length;
    }
    if (close(fd) != 0 && rc == MUSIC_MODEL_OK) rc = MUSIC_MODEL_IO;
    return rc;
}

music_model_result_t music_library_metadata(const char *path, music_metadata_t *out)
{
    music_metadata_t m = {0};
    music_model_result_t rc = music_library_resolve(NULL, path, &m.path);
    if (rc != MUSIC_MODEL_OK) return rc;
    rc = music_library_stat(m.path, false, &m.stamp);
    if (rc != MUSIC_MODEL_OK) goto done;
    rc = music_library_probe(m.path, &m.format);
    if (rc != MUSIC_MODEL_OK) goto done;
    m.title = music_model_strdup(basename_of(m.path));
    if (!m.title) { rc = MUSIC_MODEL_MEMORY; goto done; }
    if (m.format == MUSIC_FORMAT_FLAC) rc = flac_metadata(m.path, &m);
    else
    {
        mp3_info_t info;
        memset(&info, 0, sizeof(info));
        if (mp3ctrl_getinfo(m.path, &info) == 0)
        {
            m.duration_ms = (uint64_t)info.total_time_in_seconds * 1000;
            m.sample_rate = info.samplerate; m.channels = info.channels;
            m.bits_per_sample = 16;
            m.duration_estimated = m.format == MUSIC_FORMAT_MP3;
        }
        if (m.format == MUSIC_FORMAT_MP3)
        {
            m.duration_estimated = true;
            music_model_result_t duration_result = mp3_header_duration(m.path, &m);
            if (duration_result < 0) { rc = duration_result; goto done; }
            mp3_id3_info_t tags;
            memset(&tags, 0, sizeof(tags));
            int tag_result = mp3_get_id3_start(m.path, &tags);
            if (tag_result == 0)
            {
                rc = replace_text(&m.title, tags.title);
                if (rc == MUSIC_MODEL_OK) rc = replace_text(&m.artist, tags.artist);
                if (rc == MUSIC_MODEL_OK) rc = replace_text(&m.album, tags.album);
            }
            mp3_get_id3_end(&tags);
        }
    }
    if (rc != MUSIC_MODEL_OK) goto done;
    char *parent = NULL;
    rc = parent_of(m.path, &parent);
    if (rc != MUSIC_MODEL_OK) goto done;
    const char *covers[] = {"cover.jpg", "cover.png"};
    for (size_t i = 0; i < 2; ++i)
    {
        char *cover = NULL;
        if (music_library_resolve(parent, covers[i], &cover) != MUSIC_MODEL_OK) continue;
        struct stat st;
        if (stat(cover, &st) == 0 && S_ISREG(st.st_mode)) { m.cover_path = cover; break; }
        music_model_free(cover);
    }
    music_model_free(parent);
    music_file_stamp_t after;
    rc = music_library_stat(m.path, false, &after);
    if (rc == MUSIC_MODEL_OK && !music_library_stamp_equal(&m.stamp, &after)) rc = MUSIC_MODEL_CHANGED;
done:
    if (rc == MUSIC_MODEL_OK) { music_metadata_free(out); *out = m; }
    else music_metadata_free(&m);
    return rc;
}

music_model_result_t music_library_metadata_refresh(music_metadata_t *m)
{
    if (!m || !m->path) return MUSIC_MODEL_INVALID;
    music_file_stamp_t stamp;
    music_model_result_t rc = music_library_stat(m->path, m->stamp.signature_valid, &stamp);
    if (rc == MUSIC_MODEL_OK && music_library_stamp_equal(&m->stamp, &stamp)) return MUSIC_MODEL_OK;
    char *path = m->path; m->path = NULL;
    music_metadata_free(m);
    m->path = path;
    if (rc != MUSIC_MODEL_OK) return rc;
    return music_library_metadata(path, m);
}

static music_model_result_t import_line(const char *parent, char *line, music_queue_t *queue)
{
    size_t length = strlen(line);
    if (length && line[length - 1] == '\r') line[--length] = 0;
    if (!*line || *line == '#') return MUSIC_MODEL_OK;
    char *resolved = NULL;
    music_model_result_t rc = music_library_resolve(parent, line, &resolved);
    if (rc != MUSIC_MODEL_OK) return rc;
    rc = music_queue_append(queue, resolved, NULL);
    music_model_free(resolved);
    return rc;
}

music_model_result_t music_library_import_m3u8(const char *path, music_queue_t *out)
{
    char *parent = NULL;
    music_model_result_t rc = parent_of(path, &parent);
    if (rc != MUSIC_MODEL_OK) return rc;
    if (!storage_path_available(path)) { music_model_free(parent); return MUSIC_MODEL_IO; }
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) { music_model_free(parent); return MUSIC_MODEL_IO; }
    music_queue_t queue; music_queue_init(&queue, out->random_state);
    queue.mode = out->mode; queue.next_id = out->next_id;
    char line[STORAGE_PATH_MAX + 4]; size_t used = 0;
    bool first = true, comment = false;
    unsigned char block[1024]; int count;
    while (rc == MUSIC_MODEL_OK && (count = (int)read(fd, block, sizeof(block))) > 0)
    {
        for (int i = 0; i < count && rc == MUSIC_MODEL_OK; ++i)
        {
            if (!block[i]) { rc = MUSIC_MODEL_FORMAT; break; }
            if (block[i] != '\n')
            {
                if (comment) continue;
                if (block[i] == '#' && (!used || (first && used == 3 && !memcmp(line, "\xef\xbb\xbf", 3))))
                { comment = true; continue; }
                if (used + 1 >= sizeof(line)) { rc = MUSIC_MODEL_RANGE; break; }
                line[used++] = (char)block[i]; continue;
            }
            if (comment) { comment = false; used = 0; first = false; continue; }
            line[used] = 0;
            char *start = line;
            if (first && used >= 3 && !memcmp(start, "\xef\xbb\xbf", 3)) start += 3;
            first = false;
            rc = import_line(parent, start, &queue); used = 0;
        }
    }
    if (rc == MUSIC_MODEL_OK && count < 0) rc = MUSIC_MODEL_IO;
    if (rc == MUSIC_MODEL_OK && used && !comment)
    {
        line[used] = 0;
        char *start = first && used >= 3 && !memcmp(line, "\xef\xbb\xbf", 3) ? line + 3 : line;
        rc = import_line(parent, start, &queue);
    }
    if (close(fd) != 0 && rc == MUSIC_MODEL_OK) rc = MUSIC_MODEL_IO;
    music_model_free(parent);
    if (rc == MUSIC_MODEL_OK) { music_queue_free(out); *out = queue; }
    else music_queue_free(&queue);
    return rc;
}

static music_model_result_t relative_path(const char *parent, const char *path, char **out)
{
    char *absolute = NULL;
    music_model_result_t rc = music_library_resolve(NULL, path, &absolute);
    if (rc != MUSIC_MODEL_OK) return rc;
    size_t common = 1, i = 1;
    while (parent[i] && absolute[i] && parent[i] == absolute[i]) { if (parent[i] == '/') common = i + 1; ++i; }
    if (!parent[i] && absolute[i] == '/') common = i + 1;
    size_t up = 0;
    size_t parent_length = strlen(parent);
    for (i = common; i < parent_length; ++i) if (parent[i] == '/' || i == common) ++up;
    size_t tail = strlen(absolute + common);
    if (up > (SIZE_MAX - tail - 3) / 3) { music_model_free(absolute); return MUSIC_MODEL_RANGE; }
    char *relative = music_model_alloc(up * 3 + tail + 3);
    if (!relative) { music_model_free(absolute); return MUSIC_MODEL_MEMORY; }
    size_t used = 0;
    /* ./ also protects filenames beginning with # from comment interpretation. */
    if (!up) { relative[used++] = '.'; relative[used++] = '/'; }
    while (up--) { memcpy(relative + used, "../", 3); used += 3; }
    memcpy(relative + used, absolute + common, tail + 1);
    music_model_free(absolute); *out = relative;
    return MUSIC_MODEL_OK;
}

music_model_result_t music_library_export_m3u8(const char *path, const music_queue_t *q)
{
    char *parent = NULL, *data = NULL;
    size_t used = 8;
    music_model_result_t rc = parent_of(path, &parent);
    if (rc != MUSIC_MODEL_OK) return rc;
    data = music_model_alloc(used);
    if (!data) { music_model_free(parent); return MUSIC_MODEL_MEMORY; }
    memcpy(data, "#EXTM3U\n", used);
    for (size_t i = 0; i < q->count; ++i)
    {
        char *relative = NULL;
        rc = relative_path(parent, q->entries[i].path, &relative);
        if (rc != MUSIC_MODEL_OK) break;
        size_t length = strlen(relative);
        if (length >= STORAGE_PATH_MAX || length >= SIZE_MAX - used)
        { music_model_free(relative); rc = MUSIC_MODEL_RANGE; break; }
        char *next = music_model_realloc(data, used + length + 1);
        if (!next) { music_model_free(relative); rc = MUSIC_MODEL_MEMORY; break; }
        data = next; memcpy(data + used, relative, length); used += length;
        data[used++] = '\n'; music_model_free(relative);
    }
    if (rc == MUSIC_MODEL_OK && !storage_file_replace(path, data, used)) rc = MUSIC_MODEL_IO;
    music_model_free(parent); music_model_free(data);
    return rc;
}

void music_favorites_free(music_favorites_t *favorites)
{
    for (size_t i = 0; i < favorites->count; ++i) music_model_free(favorites->paths[i]);
    music_model_free(favorites->paths); memset(favorites, 0, sizeof(*favorites));
}

bool music_favorites_contains(const music_favorites_t *favorites, const char *path)
{
    char *normalized = NULL;
    if (music_library_resolve(NULL, path, &normalized) != MUSIC_MODEL_OK) return false;
    bool found = false;
    for (size_t i = 0; i < favorites->count; ++i)
        if (!strcmp(favorites->paths[i], normalized)) { found = true; break; }
    music_model_free(normalized); return found;
}

music_model_result_t music_favorites_set(music_favorites_t *favorites, const char *path, bool enabled)
{
    char *normalized = NULL;
    music_model_result_t rc = music_library_resolve(NULL, path, &normalized);
    if (rc != MUSIC_MODEL_OK) return rc;
    size_t index;
    for (index = 0; index < favorites->count; ++index)
        if (!strcmp(favorites->paths[index], normalized)) break;
    if (index < favorites->count)
    {
        music_model_free(normalized);
        if (!enabled)
        {
            music_model_free(favorites->paths[index]);
            memmove(favorites->paths + index, favorites->paths + index + 1,
                    (favorites->count - index - 1) * sizeof(*favorites->paths));
            --favorites->count;
        }
        return MUSIC_MODEL_OK;
    }
    if (!enabled) { music_model_free(normalized); return MUSIC_MODEL_OK; }
    if (favorites->count == favorites->capacity)
    {
        size_t capacity = favorites->capacity ? favorites->capacity * 2 : 8;
        if (capacity < favorites->capacity || capacity > SIZE_MAX / sizeof(*favorites->paths))
        { music_model_free(normalized); return MUSIC_MODEL_RANGE; }
        void *copy = music_model_realloc(favorites->paths, capacity * sizeof(*favorites->paths));
        if (!copy) { music_model_free(normalized); return MUSIC_MODEL_MEMORY; }
        favorites->paths = copy; favorites->capacity = capacity;
    }
    favorites->paths[favorites->count++] = normalized;
    return MUSIC_MODEL_OK;
}
