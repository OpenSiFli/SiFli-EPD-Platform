#ifndef NOTES_MODEL_H
#define NOTES_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOTE_TEXT_LIMIT (256u * 1024u)
#define NOTE_BLOCK_LIMIT 1024u
#define NOTE_SPAN_LIMIT 2048u
#define NOTE_BOLD 1u
#define NOTE_ITALIC 2u
typedef enum { NOTE_TEXT, NOTE_HEADING, NOTE_QUOTE, NOTE_RULE, NOTE_TASK,
               NOTE_IMAGE, NOTE_AUDIO, NOTE_TRANSCRIPT } note_kind_t;
typedef struct { uint32_t start, end, style; } note_span_t;
typedef struct { uint32_t milliseconds; char title[96]; } note_mark_t;
typedef struct {
    uint32_t id, source, milliseconds;
    note_kind_t kind;
    bool checked;
    char *text, *original;
    note_span_t *spans;
    size_t span_count;
    note_mark_t *marks;
    size_t mark_count;
    char attachment[96];
} note_block_t;
typedef struct {
    uint32_t id, revision, next_block, created, updated;
    bool pinned, deleted;
    char title[160], notebook[96];
    note_block_t *blocks;
    size_t count;
} note_t;
typedef struct { unsigned char *data; size_t size, capacity; bool failed; } note_buffer_t;
typedef struct { size_t text, encoded; } note_size_t;
typedef struct { char *text; note_span_t *spans; size_t span_count; note_size_t size; } note_text_edit_t;

void *note_alloc(size_t size);
void note_free(void *data);
char *note_string(const char *text, size_t length);
bool note_utf8(const char *text, size_t length);
size_t note_byte_offset(const char *text, size_t character);
void note_init(note_t *note, uint32_t id, uint32_t now);
void note_clear(note_t *note);
bool note_clone(note_t *out, const note_t *source);
bool note_measure(const note_t *note, note_size_t *size);
/* Prepare accepts a validated block and current document sizes. It owns only
 * replacement text/spans; the source remains unchanged on failure. */
bool note_edit_prepare(const note_block_t *block, const char *text, note_size_t size, note_text_edit_t *edit);
void note_edit_apply(note_block_t *block, note_text_edit_t *edit);
void note_edit_clear(note_text_edit_t *edit);
note_block_t *note_find(note_t *note, uint32_t id);
note_block_t *note_insert(note_t *note, size_t position, note_kind_t kind, const char *text);
bool note_remove(note_t *note, uint32_t id);
bool note_move(note_t *note, uint32_t id, int direction);
bool note_edit(note_t *note, uint32_t id, const char *text);
bool note_style(note_t *note, uint32_t id, uint32_t begin, uint32_t end, unsigned flag);
bool note_mark(note_t *note, uint32_t id, uint32_t milliseconds, const char *label);
bool note_empty(const note_t *note);
size_t note_text_bytes(const note_t *note);
size_t note_word_count(const note_t *note);
void note_display_title(const note_t *note, char *out, size_t size);
void note_buffer_clear(note_buffer_t *buffer);
bool note_buffer_append(note_buffer_t *buffer, const void *data, size_t bytes);
bool note_encode(const note_t *note, note_buffer_t *buffer);
bool note_decode(note_t *note, const void *data, size_t size);
bool note_markdown(const note_t *note, note_buffer_t *out, bool plain);
bool note_import_text(note_t *note, const char *text, size_t bytes, bool markdown);
bool note_attachment_valid(const char *path);
#endif
