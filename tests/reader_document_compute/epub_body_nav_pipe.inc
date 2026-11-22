/* SPDX-License-Identifier: Apache-2.0 */
static uint8_t *phase_body, *phase_nav, *phase_restore, *phase_note;
static size_t phase_body_bytes, phase_nav_bytes, phase_restore_bytes, phase_note_bytes;
static unsigned nav_opens, body_opens, note_opens, resumed_opens;

int epd_compute_pipe_open(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length, epd_compute_pipe_host_t *out)
{
    (void)data; (void)length;
    assert(!strcmp(name, BOOK_DOCUMENT_COMPONENT) && version == BOOK_DOCUMENT_VERSION);
    current_document = worker_document;
    assert(pipe_opens == pipe_closes);
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->message = *initial; (*out)->document = current_document; ++pipe_opens;
    uint32_t flags = initial->values[6];
    if (flags & BOOK_DOCUMENT_NAV_ONLY) {
        assert(purpose == EPD_COMPUTE_BACKGROUND && current_document->body_end);
        assert(initial->values[4] == current_document->chapter && initial->size == current_document->nav_cursor);
        (*out)->wire = phase_nav; (*out)->wire_length = phase_nav_bytes; ++nav_opens;
        if (initial->size) ++resumed_opens;
        unsigned skipped = 0;
        while (skipped < initial->size) {
            book_document_event_t e;
            assert((*out)->at + sizeof(e) <= (*out)->wire_length);
            memcpy(&e, (*out)->wire + (*out)->at, sizeof(e));
            (*out)->at += e.bytes;
            if (e.type == BOOK_DOCUMENT_CHAPTER ||
                (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC)) ++skipped;
        }
    } else if (flags & BOOK_DOCUMENT_NOTE) {
        assert(current_document->note && !conversion_document); ++note_opens;
        (*out)->wire = phase_note; (*out)->wire_length = phase_note_bytes;
    } else {
        assert(flags & BOOK_DOCUMENT_BODY_ONLY); ++body_opens;
        assert(initial->size == current_document->requested.skip_utf8);
        if (flags & BOOK_DOCUMENT_FIRST_BODY) {
            assert(current_document->first_body);
            (*out)->wire = wire_data; (*out)->wire_length = wire_bytes;
        } else {
            assert(!current_document->first_body && initial->values[4] == current_document->requested.chapter);
            (*out)->wire = initial->values[5] ? phase_restore : phase_body;
            (*out)->wire_length = initial->values[5] ? phase_restore_bytes : phase_body_bytes;
        }
    }
    return EPD_COMPUTE_OK;
}
