#include "notes_store.h"
#include "storage.h"
#include "storage_file.h"
#include "system_time.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

uint32_t notes_crc(const void *data, size_t size, uint32_t previous)
{
    const unsigned char *p = data; uint32_t c = ~previous;
    while (size--) { c ^= *p++; for (unsigned i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
bool notes_store_valid(const notes_store_t *s)
{ return s && s->session && storage_session_valid(STORAGE_SD, s->session) && storage_available(STORAGE_SD); }
notes_error_t notes_store_open(notes_store_t *s)
{
    s->session = storage_card_session();
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock(); bool ok = notes_store_valid(s) && storage_mkdirs(NOTES_ROOT); storage_unlock();
    return ok ? NOTES_OK : NOTES_IO;
}
bool notes_note_path(char *out, size_t capacity, uint32_t id, const char *file)
{
    if (!id || !file || strstr(file, "..") || *file == '/' || strchr(file, '\\')) return false;
    int size = snprintf(out, capacity, NOTES_ROOT "/%08lx/%s", (unsigned long)id, file);
    return size > 0 && (size_t)size < capacity;
}
notes_error_t notes_store_read(notes_store_t *s, const char *path, note_buffer_t *out)
{
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock();
    int fd = notes_store_valid(s) ? open(path, O_RDONLY, 0) : -1;
    if (fd < 0) { storage_unlock(); return NOTES_IO; }
    unsigned char data[2048]; notes_error_t result = NOTES_OK;
    for (;;) {
        if (!notes_store_valid(s)) { result = NOTES_NO_CARD; break; }
        int n = read(fd, data, sizeof(data));
        if (n < 0) { result = NOTES_IO; break; }
        if (!n) break;
        if (!note_buffer_append(out, data, (size_t)n)) { result = NOTES_LIMIT; break; }
    }
    close(fd); storage_unlock(); return result;
}
notes_error_t notes_store_write(notes_store_t *s, const char *path, const void *data, size_t size)
{
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock(); bool ok = notes_store_valid(s) && storage_file_replace(path, data, size); storage_unlock();
    return ok ? NOTES_OK : notes_store_valid(s) ? NOTES_IO : NOTES_NO_CARD;
}
static notes_error_t save_named(notes_store_t *s, const note_t *n, const char *name)
{
    note_buffer_t b = {0}; char path[NOTES_PATH_SIZE];
    if (!note_encode(n, &b) || !notes_note_path(path, sizeof(path), n->id, name)) { note_buffer_clear(&b); return NOTES_NOMEM; }
    uint32_t crc = notes_crc(b.data, b.size, 0);
    unsigned char end[4] = {(unsigned char)crc, (unsigned char)(crc >> 8), (unsigned char)(crc >> 16), (unsigned char)(crc >> 24)};
    if (!note_buffer_append(&b, end, 4)) { note_buffer_clear(&b); return NOTES_NOMEM; }
    notes_error_t e = notes_store_write(s, path, b.data, b.size); note_buffer_clear(&b); return e;
}
static notes_error_t load_named(notes_store_t *s, uint32_t id, const char *name, note_t *n)
{
    char path[NOTES_PATH_SIZE]; note_buffer_t b = {0};
    if (!notes_note_path(path, sizeof(path), id, name)) return NOTES_INVALID;
    storage_lock();
    bool valid = notes_store_valid(s) && storage_file_recover(path);
    storage_unlock();
    if (!valid) return notes_store_valid(s) ? NOTES_IO : NOTES_NO_CARD;
    notes_error_t e = notes_store_read(s, path, &b);
    if (!e) {
        if (b.size < 4) e = NOTES_INVALID;
        else {
            const unsigned char *p = b.data + b.size - 4;
            uint32_t crc = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            note_t candidate = {0};
            if (crc != notes_crc(b.data, b.size - 4, 0) || !note_decode(&candidate, b.data, b.size - 4) || candidate.id != id) e = NOTES_INVALID;
            else { note_clear(n); *n = candidate; memset(&candidate, 0, sizeof(candidate)); }
            note_clear(&candidate);
        }
    }
    note_buffer_clear(&b); return e;
}
notes_error_t notes_store_load(notes_store_t *s, uint32_t id, note_t *n)
{ return load_named(s, id, "note.bin", n); }
notes_error_t notes_store_resume(notes_store_t *s, uint32_t id, const note_buffer_t *saved)
{
    if (!saved->size) return NOTES_CONFLICT;
    notes_store_t next = {0};
    notes_error_t e = notes_store_open(&next);
    note_t disk = {0}; note_buffer_t encoded = {0};
    if (!e) e = notes_store_load(&next, id, &disk);
    if (!e && !note_encode(&disk, &encoded)) e = NOTES_NOMEM;
    if (!e && (encoded.size != saved->size || memcmp(encoded.data, saved->data, saved->size))) e = NOTES_CONFLICT;
    if (!e) *s = next;
    note_clear(&disk); note_buffer_clear(&encoded); return e;
}
static void summarize(const note_t *n, notes_row_t *row)
{
    memset(row, 0, sizeof(*row)); row->id=n->id; row->revision=n->revision;
    row->created=n->created; row->updated=n->updated; row->pinned=n->pinned; row->deleted=n->deleted;
    strcpy(row->notebook,n->notebook); note_display_title(n,row->title,sizeof(row->title));
    for(size_t i=0;i<n->count;++i) {
        if(n->blocks[i].kind==NOTE_TASK&&!n->blocks[i].checked)++row->tasks;
        if(!row->summary[0]&&n->blocks[i].text[0]) {
            size_t size=strlen(n->blocks[i].text);if(size>=sizeof(row->summary))size=sizeof(row->summary)-1;
            while(size&&((unsigned char)n->blocks[i].text[size]&0xc0)==0x80)--size;
            memcpy(row->summary,n->blocks[i].text,size);
        }
    }
}
static void save_summary(notes_store_t *s, const note_t *n)
{
    notes_row_t row; summarize(n,&row); note_t summary={0};note_init(&summary,n->id,n->created);
    strcpy(summary.title,row.title);strcpy(summary.notebook,row.notebook);summary.updated=n->updated;
    summary.pinned=n->pinned;summary.deleted=n->deleted;
    note_block_t *b=note_insert(&summary,0,NOTE_TEXT,row.summary);
    if(b){b->source=row.tasks;summary.revision=n->revision;save_named(s,&summary,"summary.bin");}
    note_clear(&summary);
}
notes_error_t notes_store_save(notes_store_t *s, const note_t *n)
{
    notes_error_t e=save_named(s,n,"note.bin");
    if(!e)save_summary(s,n);
    return e;
}
notes_error_t notes_store_checkpoint(notes_store_t *s, const note_t *n)
{ return save_named(s, n, "before-ai.bin"); }
notes_error_t notes_store_restore(notes_store_t *s, uint32_t id, note_t *n)
{ return load_named(s, id, "before-ai.bin", n); }
notes_error_t notes_store_new(notes_store_t *s, note_t *n)
{
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock();
    uint32_t id = (uint32_t)rt_tick_get() ^ (uint32_t)system_time_get_utc();
    if (!id) id = 1;
    char path[NOTES_PATH_SIZE]; bool ok = false;
    for (unsigned attempt = 0; attempt < 65536 && notes_store_valid(s); ++attempt) {
        if (!notes_note_path(path, sizeof(path), id, "")) break;
        struct stat info;
        if (stat(path, &info) != 0) { ok = mkdir(path, 0) == 0; break; }
        if (++id == 0) id = 1;
    }
    bool created = ok;
    if (ok) {
        notes_note_path(path, sizeof(path), id, "attachments");
        ok = storage_mkdirs(path);
    }
    storage_unlock();
    if (!ok) { if (created) notes_store_abandon(s, id); return NOTES_IO; }
    note_clear(n); note_init(n, id, (uint32_t)system_time_get_utc()); return NOTES_OK;
}
static bool id_from_name(const char *name, uint32_t *id)
{
    if (strlen(name) != 8) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned digit = (unsigned char)name[i];
        if (digit >= '0' && digit <= '9') digit -= '0';
        else if (digit >= 'a' && digit <= 'f') digit = digit - 'a' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    *id = value; return value != 0;
}
notes_error_t notes_store_scan(notes_store_t *s, notes_scan_fn callback, void *user)
{
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock(); DIR *dir = opendir(NOTES_ROOT); storage_unlock();
    if (!dir) return NOTES_IO;
    notes_error_t error = NOTES_OK;
    for (;;) {
        storage_lock(); struct dirent *entry = notes_store_valid(s) ? readdir(dir) : NULL;
        uint32_t id = 0; bool found = entry && id_from_name(entry->d_name, &id); bool end = entry == NULL;
        storage_unlock();
        if (!notes_store_valid(s)) { error = NOTES_NO_CARD; break; }
        if (end) break;
        if (!found) continue;
        note_t n={0};notes_row_t row={0};bool cached=false;
        if(!load_named(s,id,"summary.bin",&n)&&n.count==1) {
            char path[NOTES_PATH_SIZE];unsigned char head[12]={0};notes_note_path(path,sizeof(path),id,"note.bin");
            storage_lock();bool recovered=notes_store_valid(s)&&storage_file_recover(path);
            int fd=recovered?open(path,O_RDONLY,0):-1;
            bool read_ok=fd>=0&&read(fd,head,sizeof(head))==sizeof(head);if(fd>=0)close(fd);storage_unlock();
            uint32_t revision=(uint32_t)head[8]|(uint32_t)head[9]<<8|(uint32_t)head[10]<<16|(uint32_t)head[11]<<24;
            cached=read_ok&&!memcmp(head,"NOT1",4)&&revision==n.revision;
            if(cached){summarize(&n,&row);row.tasks=n.blocks[0].source;}
        }
        if(!cached){note_clear(&n);if(notes_store_load(s,id,&n))continue;summarize(&n,&row);save_summary(s,&n);}
        note_clear(&n); if (!callback(user, &row)) { error = NOTES_CANCELLED; break; }
        rt_thread_yield();
    }
    storage_lock(); closedir(dir); storage_unlock(); return error;
}
static notes_error_t remove_directory(notes_store_t *s, uint32_t id)
{
    notes_error_t e = NOTES_OK;
    char path[NOTES_PATH_SIZE], child[NOTES_PATH_SIZE];
    notes_note_path(path, sizeof(path), id, "attachments"); storage_lock();
    DIR *dir = notes_store_valid(s) ? opendir(path) : NULL;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char relative[96];
            if (strlen(entry->d_name) > sizeof(relative) - 13) { e = NOTES_INVALID; break; }
            strcpy(relative, "attachments/"); strcat(relative, entry->d_name);
            if (!note_attachment_valid(relative) || !notes_note_path(child, sizeof(child), id, relative) ||
                !notes_store_valid(s) || unlink(child)) { e = NOTES_IO; break; }
        }
        closedir(dir);
    }
    if (!e && notes_store_valid(s)) {
        rmdir(path);
        const char *files[] = {"before-ai.bin", "before-ai.bin.old", "before-ai.bin.tmp", "ai.bin", "ai.bin.old", "ai.bin.tmp",
                               "summary.bin", "summary.bin.old", "summary.bin.tmp", "note.bin.old", "note.bin.tmp", "note.bin"};
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
            notes_note_path(child, sizeof(child), id, files[i]); struct stat info;
            if (stat(child, &info) == 0 && unlink(child)) { e = NOTES_IO; break; }
        }
        if (!e) { notes_note_path(path, sizeof(path), id, ""); if (rmdir(path)) e = NOTES_IO; }
    } else if (!notes_store_valid(s)) e = NOTES_NO_CARD;
    storage_unlock(); return e;
}
notes_error_t notes_store_purge(notes_store_t *s, uint32_t id)
{
    note_t n = {0}; notes_error_t e = notes_store_load(s, id, &n);
    bool deleted = n.deleted; note_clear(&n);
    return e ? e : deleted ? remove_directory(s, id) : NOTES_INVALID;
}
void notes_store_abandon(notes_store_t *s, uint32_t id)
{
    char path[NOTES_PATH_SIZE]; struct stat info;
    if (!id || !notes_note_path(path, sizeof(path), id, "note.bin")) return;
    storage_lock(); bool unpublished = notes_store_valid(s) && stat(path, &info) != 0; storage_unlock();
    if (unpublished) remove_directory(s, id);
}
notes_error_t notes_store_attachment(notes_store_t *s, note_t *n, uint32_t id, const char *source)
{
    note_block_t *b = note_find(n, id); if (!b || (b->kind != NOTE_IMAGE && b->kind != NOTE_AUDIO)) return NOTES_INVALID;
    if (!source) return NOTES_INVALID;
    const char *extension = strrchr(source, '.');
    if (!extension || strlen(extension) > 8) return NOTES_INVALID;
    char relative[96], path[NOTES_PATH_SIZE]; snprintf(relative, sizeof(relative), "attachments/import-%08lx%s", (unsigned long)id, extension);
    for (char *p = relative; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    if (!note_attachment_valid(relative) || !notes_note_path(path, sizeof(path), n->id, relative)) return NOTES_INVALID;
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    bool ok = storage_file_import_session(source, path, STORAGE_SD, s->session);
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    if (!ok) return NOTES_IO;
    strcpy(b->attachment, relative); ++n->revision; return NOTES_OK;
}
static bool import_extension(const char *path,const char *wanted)
{
    const char *ext=strrchr(path,'.');if(!ext)return false;
    for(;*ext&&*wanted;++ext,++wanted){char c=*ext;if(c>='A'&&c<='Z')c+='a'-'A';if(c!=*wanted)return false;}
    return !*ext&&!*wanted;
}
notes_error_t notes_store_import_audio(notes_store_t *s,const char *path,uint32_t milliseconds,note_t *out)
{
    if(!import_extension(path,".mp3")&&!import_extension(path,".wav"))return NOTES_INVALID;
    note_t n={0};notes_error_t e=notes_store_new(s,&n);
    const char *name=strrchr(path,'/');name=name?name+1:path;
    size_t length=(size_t)(strrchr(name,'.')-name);if(length>=sizeof(n.title))length=sizeof(n.title)-1;
    while(length&&((unsigned char)name[length]&0xc0)==0x80)--length;
    memcpy(n.title,name,length);n.title[length]=0;
    if(!e){note_block_t *b=note_insert(&n,0,NOTE_AUDIO,n.title);
        if(!b)e=NOTES_NOMEM;else{b->milliseconds=milliseconds;e=notes_store_attachment(s,&n,b->id,path);}}
    if(!e)e=notes_store_save(s,&n);
    if(e)notes_store_abandon(s,n.id);else{note_clear(out);*out=n;memset(&n,0,sizeof(n));}
    note_clear(&n);return e;
}
notes_error_t notes_store_import(notes_store_t *s, const char *path, note_t *out)
{
    if(import_extension(path,".zip"))return notes_zip_import(s,path,out);
    bool markdown=import_extension(path,".md")||import_extension(path,".markdown");
    if(!markdown&&!import_extension(path,".txt"))return NOTES_INVALID;
    note_buffer_t b = {0}; notes_error_t e = notes_store_read(s, path, &b);
    if (e) { note_buffer_clear(&b); return e; }
    note_t n = {0}; e = notes_store_new(s, &n);
    if (!e && !note_import_text(&n, (const char *)b.data, b.size, markdown)) e = NOTES_INVALID;
    if (!e) e = notes_store_save(s, &n);
    if (!e) { note_clear(out); *out = n; memset(&n, 0, sizeof(n)); }
    if (e) notes_store_abandon(s, n.id);
    note_clear(&n); note_buffer_clear(&b); return e;
}
notes_error_t notes_store_export(notes_store_t *s, const note_t *n, unsigned format, char *path, size_t size)
{
    if (format > 2) return NOTES_INVALID;
    if (!notes_store_valid(s)) return NOTES_NO_CARD;
    storage_lock(); bool ok = storage_mkdirs(NOTES_ROOT "/Exports"); storage_unlock();
    if (!ok) return NOTES_IO;
    int count = snprintf(path, size, NOTES_ROOT "/Exports/%08lx-%lu%s", (unsigned long)n->id,
        (unsigned long)n->revision, format == 0 ? ".txt" : format == 1 ? "-markdown.zip" : ".zip");
    if (count < 0 || (size_t)count >= size) return NOTES_INVALID;
    if (format) return notes_zip_export(s, n, path, format == 2);
    note_buffer_t b = {0};
    notes_error_t e = note_markdown(n, &b, true) ? notes_store_write(s, path, b.data, b.size) : NOTES_NOMEM;
    note_buffer_clear(&b); return e;
}
