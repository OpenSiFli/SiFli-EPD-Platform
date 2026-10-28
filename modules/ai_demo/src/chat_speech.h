#ifndef CHAT_SPEECH_H
#define CHAT_SPEECH_H
#include "platform/epd_speech.h"
#include "markdown.h"

#define CHAT_SPEECH_CHUNK_BYTES 1024u
typedef struct {
    epd_speech_snapshot_t output;
    epd_speech_error_t output_error;
    bool output_active;
} chat_speech_view_t;
typedef struct {
    /* source returns a copied snapshot of the application's existing message.
     * false means the source was deleted, replaced or exceeds capacity. */
    bool (*source)(void *user, uint32_t chat, uint32_t message, char *text,
                   size_t capacity, size_t *bytes, bool *finished);
    void *user;
} chat_speech_hooks_t;
typedef struct {
    epd_service_t *owner;
    chat_speech_hooks_t hooks;
    chat_speech_view_t view;
    epd_speech_error_t read_error;
    bool automatic, finishing;
    uint32_t chat, message, blocks;
    md_doc_t document;
    uint32_t node, run, end, document_blocks;
    size_t run_offset, pending_size, pending_offset;
    bool separator;
    bool source_seen, source_finished, document_finished;
    size_t source_bytes;
    char pending[CHAT_SPEECH_CHUNK_BYTES];
} chat_speech_t;

/* All controller operations belong to one background worker. */
void chat_speech_init(chat_speech_t *speech, epd_service_t *owner, const chat_speech_hooks_t *hooks);
void chat_speech_read(chat_speech_t *speech, uint32_t chat, uint32_t message, bool automatic);
void chat_speech_pause_toggle(chat_speech_t *speech);
void chat_speech_stop_reading(chat_speech_t *speech);
void chat_speech_step(chat_speech_t *speech);
void chat_speech_dispose(chat_speech_t *speech);

/* Extract a bounded UTF-8 chunk from a parsed document; skips code entirely.
 * The cursor is retained across platform backpressure and partial appends. */
size_t chat_speech_extract(chat_speech_t *speech, char *out, size_t capacity);
#endif
