#ifndef NOTES_STORE_H
#define NOTES_STORE_H
#include "notes_model.h"
#define NOTES_ROOT "/sdcard/Notes"
#define NOTES_PATH_SIZE 512
typedef enum { NOTES_OK, NOTES_NO_CARD, NOTES_IO, NOTES_INVALID, NOTES_NOMEM,
               NOTES_LIMIT, NOTES_CANCELLED, NOTES_CONFLICT, NOTES_BUSY } notes_error_t;
typedef struct { uint32_t session; } notes_store_t;
typedef struct {
    uint32_t id, revision, created, updated, tasks;
    bool pinned, deleted;
    char title[160], notebook[96], summary[192];
} notes_row_t;
typedef bool (*notes_scan_fn)(void *user, const notes_row_t *row);
notes_error_t notes_store_open(notes_store_t *store);
notes_error_t notes_store_resume(notes_store_t *store, uint32_t id, const note_buffer_t *saved);
bool notes_store_valid(const notes_store_t *store);
bool notes_note_path(char *out, size_t capacity, uint32_t id, const char *file);
notes_error_t notes_store_new(notes_store_t *store, note_t *note);
notes_error_t notes_store_load(notes_store_t *store, uint32_t id, note_t *note);
notes_error_t notes_store_save(notes_store_t *store, const note_t *note);
notes_error_t notes_store_checkpoint(notes_store_t *store, const note_t *note);
notes_error_t notes_store_restore(notes_store_t *store, uint32_t id, note_t *note);
notes_error_t notes_store_purge(notes_store_t *store, uint32_t id);
void notes_store_abandon(notes_store_t *store, uint32_t id);
notes_error_t notes_store_scan(notes_store_t *store, notes_scan_fn callback, void *user);
notes_error_t notes_store_attachment(notes_store_t *store, note_t *note, uint32_t block, const char *source);
notes_error_t notes_store_read(notes_store_t *store, const char *path, note_buffer_t *out);
notes_error_t notes_store_write(notes_store_t *store, const char *path, const void *data, size_t size);
notes_error_t notes_store_import(notes_store_t *store, const char *path, note_t *note);
notes_error_t notes_store_import_audio(notes_store_t *store, const char *path, uint32_t milliseconds, note_t *note);
notes_error_t notes_store_export(notes_store_t *store, const note_t *note, unsigned format, char *path, size_t size);
notes_error_t notes_zip_export(notes_store_t *store, const note_t *note, const char *path, bool complete);
notes_error_t notes_zip_import(notes_store_t *store, const char *path, note_t *note);
uint32_t notes_crc(const void *data, size_t size, uint32_t previous);
#endif
