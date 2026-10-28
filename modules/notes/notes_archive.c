#include "notes_store.h"
#include "storage.h"
#include "storage_file.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct { char name[96]; uint32_t size, crc, offset; } zip_entry_t;
static uint16_t u16(const unsigned char *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static void p16(unsigned char *p, uint16_t n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void p32(unsigned char *p, uint32_t n) { p16(p, (uint16_t)n); p16(p + 2, (uint16_t)(n >> 16)); }
static bool exact(notes_store_t *s, int fd, void *data, size_t size, bool writing)
{
    unsigned char *p = data;
    while (size) {
        storage_lock();
        int n = !notes_store_valid(s) ? -1 : writing ? write(fd, p, size) : read(fd, p, size);
        storage_unlock();
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
static int open_file(notes_store_t *s, const char *path, int flags)
{ storage_lock(); int fd = notes_store_valid(s) ? open(path, flags, 0) : -1; storage_unlock(); return fd; }
static void close_file(int fd) { if (fd >= 0) { storage_lock(); close(fd); storage_unlock(); } }
static bool reposition(notes_store_t *s, int fd, uint32_t at)
{ storage_lock(); bool ok = notes_store_valid(s) && at <= INT32_MAX && lseek(fd, (off_t)at, SEEK_SET) == (off_t)at; storage_unlock(); return ok; }
static bool write_entry(notes_store_t *s, int fd, zip_entry_t *e, uint32_t *position,
                        const void *memory, size_t bytes, const char *source)
{
    int input = -1; unsigned char *buffer = NULL;
    if (source) {
        input = open_file(s, source, O_RDONLY);
        if (input < 0) return false;
        struct stat info; storage_lock(); int rc = fstat(input, &info); storage_unlock();
        if (rc || info.st_size < 0 || (uint64_t)info.st_size > INT32_MAX) { close_file(input); return false; }
        bytes = (size_t)info.st_size; buffer = note_alloc(16384);
        if (!buffer) { close_file(input); return false; }
    }
    bool ok = false; e->size = (uint32_t)bytes; e->offset = *position; e->crc = 0;
    if ((uint64_t)*position + bytes + 30 + strlen(e->name) > INT32_MAX) goto done;
    if (source) {
        for (size_t at = 0; at < bytes;) {
            size_t count = bytes - at; if (count > 16384) count = 16384;
            if (!exact(s, input, buffer, count, false)) goto done;
            e->crc = notes_crc(buffer, count, e->crc); at += count;
        }
        if (!reposition(s, input, 0)) goto done;
    } else e->crc = notes_crc(memory, bytes, 0);
    unsigned char h[30] = {0}; p32(h, 0x04034b50); p16(h + 4, 20); p16(h + 6, 0x0800);
    p32(h + 14, e->crc); p32(h + 18, e->size); p32(h + 22, e->size); p16(h + 26, (uint16_t)strlen(e->name));
    if (!exact(s, fd, h, sizeof(h), true) || !exact(s, fd, e->name, strlen(e->name), true)) goto done;
    if (source) {
        uint32_t crc = 0;
        for (size_t at = 0; at < bytes;) {
            size_t count = bytes - at; if (count > 16384) count = 16384;
            if (!exact(s, input, buffer, count, false) || !exact(s, fd, buffer, count, true)) goto done;
            crc = notes_crc(buffer, count, crc); at += count;
        }
        if (crc != e->crc) goto done;
    } else if (!exact(s, fd, (void *)memory, bytes, true)) goto done;
    *position += (uint32_t)(sizeof(h) + strlen(e->name) + bytes); ok = true;
done:
    close_file(input); note_free(buffer); return ok;
}
notes_error_t notes_zip_export(notes_store_t *s, const note_t *n, const char *path, bool complete)
{
    zip_entry_t *entries = note_alloc((n->count + 3) * sizeof(*entries));
    if (!entries) return NOTES_NOMEM;
    char temporary[NOTES_PATH_SIZE];
    if (snprintf(temporary, sizeof(temporary), "%s.part", path) >= (int)sizeof(temporary)) { note_free(entries); return NOTES_INVALID; }
    int fd = open_file(s, temporary, O_WRONLY | O_CREAT | O_TRUNC);
    notes_error_t error = NOTES_IO; size_t count = 0; uint32_t position = 0;
    note_buffer_t text = {0}, document = {0};
    if (fd < 0) goto done;
    if (!note_markdown(n, &text, false)) { error = NOTES_NOMEM; goto done; }
    strcpy(entries[count].name, "note.md");
    if (!write_entry(s, fd, entries + count++, &position, text.data, text.size, NULL)) goto done;
    if (complete) {
        static const char manifest[] = "{\"format\":\"sifli-notes\",\"version\":1,\"document\":\"note.bin\"}\n";
        strcpy(entries[count].name, "manifest.json");
        if (!write_entry(s, fd, entries + count++, &position, manifest, sizeof(manifest) - 1, NULL)) goto done;
        if (!note_encode(n, &document)) { error = NOTES_NOMEM; goto done; }
        strcpy(entries[count].name, "note.bin");
        if (!write_entry(s, fd, entries + count++, &position, document.data, document.size, NULL)) goto done;
    }
    for (size_t i = 0; i < n->count; ++i) {
        const char *name = n->blocks[i].attachment; if (!*name) continue;
        if (!note_attachment_valid(name)) { error = NOTES_INVALID; goto done; }
        bool duplicate = false;
        for (size_t j = 0; j < count; ++j) if (!strcmp(entries[j].name, name)) duplicate = true;
        if (duplicate) continue;
        char source[NOTES_PATH_SIZE]; if (!notes_note_path(source, sizeof(source), n->id, name)) goto done;
        strcpy(entries[count].name, name);
        if (!write_entry(s, fd, entries + count++, &position, NULL, 0, source)) goto done;
    }
    uint32_t central = position;
    for (size_t i = 0; i < count; ++i) {
        unsigned char h[46] = {0}; zip_entry_t *e = entries + i;
        p32(h, 0x02014b50); p16(h + 4, 20); p16(h + 6, 20); p16(h + 8, 0x0800);
        p32(h + 16, e->crc); p32(h + 20, e->size); p32(h + 24, e->size);
        p16(h + 28, (uint16_t)strlen(e->name)); p32(h + 42, e->offset);
        if (!exact(s, fd, h, sizeof(h), true) || !exact(s, fd, e->name, strlen(e->name), true)) goto done;
        position += (uint32_t)(sizeof(h) + strlen(e->name));
    }
    unsigned char end[22] = {0}; p32(end, 0x06054b50); p16(end + 8, (uint16_t)count); p16(end + 10, (uint16_t)count);
    p32(end + 12, position - central); p32(end + 16, central);
    if (!exact(s, fd, end, sizeof(end), true)) goto done;
    storage_lock(); bool synced = notes_store_valid(s) && fsync(fd) == 0; storage_unlock();
    close_file(fd); fd = -1;
    if (!synced) goto done;
    storage_lock(); bool committed = notes_store_valid(s) && storage_file_commit(temporary, path); storage_unlock();
    if (committed) error = NOTES_OK;
done:
    close_file(fd); note_buffer_clear(&text); note_buffer_clear(&document); note_free(entries);
    if (error && notes_store_valid(s)) { storage_lock(); unlink(temporary); storage_unlock(); }
    return !notes_store_valid(s) ? NOTES_NO_CARD : error;
}
notes_error_t notes_zip_import(notes_store_t *s, const char *path, note_t *out)
{
    int fd = open_file(s, path, O_RDONLY); if (fd < 0) return NOTES_IO;
    struct stat info; storage_lock(); int rc = fstat(fd, &info); storage_unlock();
    unsigned char end[22], header[46]; notes_error_t error = NOTES_INVALID;
    zip_entry_t *entries = NULL; unsigned char *buffer = NULL; note_t n = {0}, destination = {0};
    note_buffer_t document = {0};
    if (rc || info.st_size < 22 || (uint64_t)info.st_size > INT32_MAX ||
        !reposition(s, fd, (uint32_t)info.st_size - 22) || !exact(s, fd, end, 22, false) ||
        u32(end) != 0x06054b50 || u16(end + 4) || u16(end + 6) || u16(end + 20)) goto done;
    size_t count = u16(end + 10); uint32_t central = u32(end + 16), central_size = u32(end + 12);
    if (!count || count > NOTE_BLOCK_LIMIT + 3 || count != u16(end + 8) ||
        (uint64_t)central + central_size != (uint64_t)info.st_size - 22) goto done;
    entries = note_alloc(count * sizeof(*entries)); buffer = note_alloc(16384);
    if (!entries || !buffer) { error = NOTES_NOMEM; goto done; }
    if (!reposition(s, fd, central)) goto done;
    uint32_t at = central; bool manifest = false, binary = false;
    for (size_t i = 0; i < count; ++i) {
        if (!exact(s, fd, header, 46, false) || u32(header) != 0x02014b50 || u16(header + 10) ||
            (u16(header + 8) & ~0x0800u) || u16(header + 30) || u16(header + 32) || u16(header + 34) ||
            u32(header + 20) != u32(header + 24)) goto done;
        size_t length = u16(header + 28); zip_entry_t *e = entries + i;
        if (!length || length >= sizeof(e->name) || !exact(s, fd, e->name, length, false) || memchr(e->name, 0, length)) goto done;
        e->crc = u32(header + 16); e->size = u32(header + 24); e->offset = u32(header + 42);
        if ((uint64_t)e->offset + 30 + length + e->size > central) goto done;
        if (strcmp(e->name, "note.md") && strcmp(e->name, "note.bin") && strcmp(e->name, "manifest.json") &&
            (!note_attachment_valid(e->name) || !*e->name)) goto done;
        for (size_t j = 0; j < i; ++j) if (!strcmp(entries[j].name, e->name)) goto done;
        at += 46u + (uint32_t)length;
    }
    if (at != central + central_size) goto done;
    /* Validate every payload before publishing any imported note. */
    for (size_t i = 0; i < count; ++i) {
        zip_entry_t *e = entries + i; unsigned char local[30]; char name[96] = {0};
        if (!reposition(s, fd, e->offset) || !exact(s, fd, local, 30, false) || u32(local) != 0x04034b50 ||
            u16(local + 8) || (u16(local + 6) & ~0x0800u) || u16(local + 28) ||
            u16(local + 26) != strlen(e->name) || u32(local + 14) != e->crc ||
            u32(local + 18) != e->size || u32(local + 22) != e->size ||
            !exact(s, fd, name, strlen(e->name), false) || strcmp(name, e->name)) goto done;
        uint32_t crc = 0; note_buffer_t content = {0}; bool capture = !strcmp(e->name, "note.bin") || !strcmp(e->name, "manifest.json");
        bool ok = true;
        for (uint32_t pos = 0; pos < e->size;) {
            size_t take = e->size - pos; if (take > 16384) take = 16384;
            if (!exact(s, fd, buffer, take, false) || (capture && !note_buffer_append(&content, buffer, take))) { ok = false; break; }
            crc = notes_crc(buffer, take, crc); pos += (uint32_t)take;
        }
        if (!ok || crc != e->crc) { note_buffer_clear(&content); goto done; }
        if (!strcmp(e->name, "manifest.json")) {
            static const char expected[] = "{\"format\":\"sifli-notes\",\"version\":1,\"document\":\"note.bin\"}\n";
            manifest = content.size == sizeof(expected) - 1 && !memcmp(content.data, expected, content.size);
        }
        if (!strcmp(e->name, "note.bin")) { binary = note_decode(&n, content.data, content.size); }
        note_buffer_clear(&content);
    }
    if (!manifest || !binary) goto done;
    for (size_t i = 0; i < n.count; ++i) {
        if (!*n.blocks[i].attachment) continue;
        bool found = false;
        for (size_t j = 0; j < count; ++j) if (!strcmp(entries[j].name, n.blocks[i].attachment)) found = true;
        if (!found) goto done;
    }
    error = notes_store_new(s, &destination); if (error) goto done;
    n.id = destination.id; n.deleted = false; ++n.revision;
    for (size_t i = 0; i < count; ++i) {
        zip_entry_t *e = entries + i;
        if (strncmp(e->name, "attachments/", 12)) continue;
        char target[NOTES_PATH_SIZE]; notes_note_path(target, sizeof(target), n.id, e->name);
        int output = open_file(s, target, O_WRONLY | O_CREAT | O_EXCL);
        if (output < 0 || !reposition(s, fd, e->offset + 30u + (uint32_t)strlen(e->name))) { close_file(output); error = NOTES_IO; goto done; }
        bool ok = true; uint32_t crc = 0;
        for (uint32_t pos = 0; pos < e->size;) {
            size_t take = e->size - pos; if (take > 16384) take = 16384;
            if (!exact(s, fd, buffer, take, false) || !exact(s, output, buffer, take, true)) { ok = false; break; }
            crc = notes_crc(buffer, take, crc); pos += (uint32_t)take;
        }
        storage_lock(); if (!notes_store_valid(s) || fsync(output)) ok = false; storage_unlock(); close_file(output);
        if (!ok || crc != e->crc) { error = NOTES_IO; goto done; }
    }
    error = notes_store_save(s, &n);
    if (!error) { note_clear(out); *out = n; memset(&n, 0, sizeof(n)); }
done:
    if (error) notes_store_abandon(s, destination.id);
    close_file(fd); note_free(entries); note_free(buffer); note_buffer_clear(&document); note_clear(&n); note_clear(&destination);
    return !notes_store_valid(s) ? NOTES_NO_CARD : error;
}
