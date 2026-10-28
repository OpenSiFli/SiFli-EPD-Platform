#include "chat_speech.h"
#include "platform/epd_memory.h"
#include <string.h>

static void *allocate(void *context, size_t bytes)
{ (void)context; return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void release(void *context, void *pointer)
{ (void)context; epd_app_free(pointer); }
static epd_speech_request_t request(void)
{
    epd_speech_request_t r = {0};
    r.struct_size = sizeof(r); r.version = EPD_SPEECH_VERSION;
    r.language = EPD_SPEECH_ZH_EN; r.background = true;
    memcpy(r.title, "AI Chat", sizeof("AI Chat")); return r;
}
void chat_speech_init(chat_speech_t *s, epd_service_t *owner, const chat_speech_hooks_t *hooks)
{
    memset(s, 0, sizeof(*s)); s->owner = owner; s->hooks = *hooks;
}
static void clear_document(chat_speech_t *s)
{
    md_document_free(&s->document);
    s->node = s->run = s->end = 0; s->run_offset = 0; s->separator = false;
    s->pending_size = s->pending_offset = 0;
}
void chat_speech_stop_reading(chat_speech_t *s)
{
    clear_document(s); s->finishing = true;
    if (s->view.output_active) {
        epd_speech_error_t e = epd_speech_stop(s->owner, s->view.output.session);
        if (e == EPD_SPEECH_OK) s->view.output.state = EPD_SPEECH_STOPPING;
        else if (e != EPD_SPEECH_NOT_FOUND) s->view.output_error = e;
    }
}
void chat_speech_read(chat_speech_t *s, uint32_t chat, uint32_t message, bool automatic)
{
    if (s->view.output_active) { s->view.output_error = EPD_SPEECH_BUSY; return; }
    epd_speech_request_t r = request();
    epd_speech_session_t session = 0;
    s->view.output_error = epd_speech_start(s->owner, EPD_SPEECH_TTS, &r, &session);
    if (s->view.output_error != EPD_SPEECH_OK) return;
    clear_document(s);
    memset(&s->view.output, 0, sizeof(s->view.output));
    s->view.output.session = session; s->view.output.kind = EPD_SPEECH_TTS;
    s->view.output.state = EPD_SPEECH_PREPARING;
    s->chat = chat; s->message = message; s->blocks = 0;
    s->source_seen = false; s->source_bytes = 0; s->source_finished = s->document_finished = false;
    s->automatic = automatic; s->finishing = false; s->view.output_active = true;
    s->read_error = EPD_SPEECH_OK;
}
void chat_speech_pause_toggle(chat_speech_t *s)
{
    if (!s->view.output_active || s->view.output.state == EPD_SPEECH_STOPPING) return;
    bool paused = s->view.output.state == EPD_SPEECH_PAUSED || s->view.output.state == EPD_SPEECH_PAUSING;
    epd_speech_error_t e = paused ? epd_speech_resume(s->owner, s->view.output.session) :
                                   epd_speech_pause(s->owner, s->view.output.session);
    if (e == EPD_SPEECH_OK) {
        /* Resume acceptance does not confirm reacquisition of audio ownership. */
        if (!paused) {
            s->view.output.state = EPD_SPEECH_PAUSING;
            s->view.output_error = EPD_SPEECH_OK;
        }
    } else if (e != EPD_SPEECH_NOT_FOUND) s->view.output_error = e;
}
static bool output_terminal_pending(chat_speech_t *s)
{
    epd_speech_snapshot_t snapshot;
    if (epd_speech_snapshot(s->owner, s->view.output.session, &snapshot) == EPD_SPEECH_OK)
        s->view.output = snapshot;
    epd_speech_state_t state = s->view.output.state;
    if (state == EPD_SPEECH_STOPPING || state == EPD_SPEECH_COMPLETED ||
        state == EPD_SPEECH_STOPPED || state == EPD_SPEECH_FAILED) {
        clear_document(s); s->finishing = true; return true;
    }
    return false;
}
static bool text_node(md_node_kind_t kind)
{ return kind == MD_NODE_P || kind == MD_NODE_HEADING || kind == MD_NODE_TH || kind == MD_NODE_TD; }
size_t chat_speech_extract(chat_speech_t *s, char *out, size_t capacity)
{
    size_t used = 0;
    while (s->node < s->end && used < capacity) {
        const md_node_t *n = &s->document.nodes[s->node];
        if (!text_node(n->kind)) { ++s->node; s->run = 0; continue; }
        if (s->run == n->run_count) {
            if (s->separator) out[used++] = n->kind == MD_NODE_TH || n->kind == MD_NODE_TD ? ' ' : '\n';
            ++s->node; s->run = 0; s->separator = false; continue;
        }
        const md_run_t *r = &s->document.runs[n->first_run + s->run];
        if (r->style & MD_STYLE_CODE) {
            /* Keep the surrounding words separate without reading code. */
            if (s->separator) out[used++] = ' ';
            ++s->run; s->run_offset = 0; continue;
        }
        if (r->kind != MD_RUN_TEXT) {
            out[used++] = ' '; ++s->run; s->run_offset = 0; continue;
        }
        size_t remaining = r->text.length - s->run_offset;
        size_t count = remaining < capacity - used ? remaining : capacity - used;
        const char *text = s->document.text + r->text.offset + s->run_offset;
        while (count && count < remaining && ((unsigned char)text[count] & 0xc0) == 0x80) --count;
        if (!count && remaining) break;
        memcpy(out + used, text, count); used += count; s->run_offset += count;
        if (count) s->separator = true;
        if (s->run_offset == r->text.length) { ++s->run; s->run_offset = 0; }
    }
    return used;
}
static epd_speech_error_t prepare(chat_speech_t *s, bool *finished)
{
    char *source = epd_app_alloc(MD_INPUT_MAX + 1, EPD_APP_PSRAM);
    if (!source) return EPD_SPEECH_NO_MEMORY;
    size_t bytes = 0;
    if (!s->hooks.source(s->hooks.user, s->chat, s->message, source, MD_INPUT_MAX + 1, &bytes, finished)) {
        epd_app_free(source); return EPD_SPEECH_NOT_FOUND;
    }
    if (s->source_seen && bytes == s->source_bytes && *finished == s->source_finished) {
        epd_app_free(source); return EPD_SPEECH_OK;
    }
    s->source_seen = true; s->source_bytes = bytes; s->source_finished = *finished;
    /* Reference links may be resolved by definitions later in the response. */
    if (!*finished && memchr(source, '[', bytes)) { epd_app_free(source); return EPD_SPEECH_OK; }
    md_allocator_t a = {NULL, allocate, release};
    md_status_t result = md_document_parse(source, bytes, &a, NULL, &s->document);
    epd_app_free(source);
    if (result != MD_OK) return result == MD_OUT_OF_MEMORY ? EPD_SPEECH_NO_MEMORY : EPD_SPEECH_TEXT_TOO_LONG;
    uint32_t count = 0, first = s->document.node_count, end = s->document.node_count;
    for (uint32_t i = s->document.nodes[0].first_child; i != MD_INDEX_NONE;
         i = s->document.nodes[i].next_sibling) {
        if (count == s->blocks) first = i;
        if (!*finished && s->document.nodes[i].next_sibling == MD_INDEX_NONE) { end = i; break; }
        ++count;
    }
    s->document_blocks = count; s->node = first; s->end = end; s->run = 0; s->run_offset = 0;
    s->document_finished = *finished;
    return EPD_SPEECH_OK;
}
static void feed(chat_speech_t *s)
{
    if (s->finishing || s->view.output.state == EPD_SPEECH_STOPPING ||
        s->view.output.state == EPD_SPEECH_PAUSING || s->view.output.state == EPD_SPEECH_PAUSED) return;
    bool finished = false;
    epd_speech_error_t error = EPD_SPEECH_OK;
    if (!s->document.nodes) error = prepare(s, &finished);
    if (error == EPD_SPEECH_NOT_FOUND) { chat_speech_stop_reading(s); return; }
    if (error != EPD_SPEECH_OK) goto failed;
    if (!s->document.nodes) return;
    if (!s->pending_size) {
        s->pending_size = chat_speech_extract(s, s->pending, sizeof(s->pending)); s->pending_offset = 0;
    }
    if (s->pending_size) {
        int accepted = epd_speech_append(s->owner, s->view.output.session,
            s->pending + s->pending_offset, s->pending_size - s->pending_offset);
        if (accepted < 0) { error = (epd_speech_error_t)-accepted; goto failed; }
        s->pending_offset += (size_t)accepted;
        if (s->pending_offset == s->pending_size) s->pending_size = s->pending_offset = 0;
        return;
    }
    s->blocks = s->document_blocks;
    finished = s->document_finished;
    clear_document(s);
    if (finished) {
        error = epd_speech_finish(s->owner, s->view.output.session);
        if (error != EPD_SPEECH_OK) goto failed;
        s->finishing = true;
    }
    return;
failed:
    /* Completion can race append/finish. Only polling the terminal consumes it;
     * a rejected append must not replace the provider's actual failure. */
    if (output_terminal_pending(s)) return;
    s->read_error = s->view.output_error = error; chat_speech_stop_reading(s);
}
void chat_speech_step(chat_speech_t *s)
{
    epd_speech_event_t event;
    for (unsigned i = 0; i < 16 && epd_speech_poll(s->owner, &event) == EPD_SPEECH_OK; ++i) {
        if (s->view.output_active && event.snapshot.session == s->view.output.session) {
            s->view.output = event.snapshot;
            if (event.kind == EPD_SPEECH_EVENT_TERMINAL) {
                s->view.output_active = false;
                epd_speech_error_t error = event.snapshot.error;
                s->view.output_error = s->read_error != EPD_SPEECH_OK &&
                    (error == EPD_SPEECH_OK || error == EPD_SPEECH_CANCELLED) ? s->read_error : error;
                clear_document(s);
            } else if (event.kind == EPD_SPEECH_EVENT_STATE && s->read_error == EPD_SPEECH_OK)
                s->view.output_error = event.snapshot.error;
        }
        epd_speech_event_release(&event);
    }
    if (s->view.output_active && !output_terminal_pending(s)) feed(s);
}
void chat_speech_dispose(chat_speech_t *s)
{
    chat_speech_stop_reading(s);
    s->view.output_active = false;
}
