/* SPDX-License-Identifier: Apache-2.0 */
document_note_request_t *document_note_request(document_t *source, uint32_t record)
{
    if (!source || source->source.resources.open || cancelled(source) || worker_stopping ||
        (source->format != DOCUMENT_EPUB && source->format != DOCUMENT_SPLIT_EPUB &&
         source->format != DOCUMENT_XHTML)) return NULL;
    unsigned pending = 0;
    for (document_t *p = documents; p; p = p->next) if (p->note) ++pending;
    if (pending >= 2) return NULL;
    document_note_request_t *note = allocate(sizeof(*note));
    document_t *d = allocate(sizeof(*d));
    if (!note || !d) { epd_app_free(note); epd_app_free(d); return NULL; }
    memset(note, 0, sizeof(*note)); memset(d, 0, sizeof(*d));
    if (!document_link_target(source, record, note->target, sizeof(note->target)) ||
        !strchr(note->target, '#') || strstr(note->target, ":") ||
        !strncmp(note->target, "//", 2)) {
        epd_app_free(note); epd_app_free(d); return NULL;
    }
    d->note = note; note->document = d;
    d->file = source->file; d->format = source->format; d->owners = 1;
    d->input = d->output = -1; d->generation = source->generation;
    d->opened_at = rt_tick_get();
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping || cancelled(source)) {
        rt_hw_interrupt_enable(key); epd_app_free(note); epd_app_free(d); return NULL;
    }
    d->demand = d->serial = ++sequence;
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return note;
}

int document_note_take(document_note_request_t *note, char *text, size_t capacity, bool *truncated)
{
    if (!note || !text || !capacity) return -1;
    document_t *d = note->document;
    if (cancelled(d)) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    bool done = d->status.complete;
    int result = d->status.result;
    rt_hw_interrupt_enable(key);
    if (!done) return elapsed_ms(d->opened_at) > 10000u ? -1 : 0;
    if (result != BOOK_OK || !note->received || !note->used) return -1;
    size_t n = note->used < capacity ? note->used : capacity - 1;
    while (n && n < note->used && ((unsigned char)note->text[n] & 0xc0) == 0x80) --n;
    memcpy(text, note->text, n); text[n] = 0;
    if (truncated) *truncated = note->truncated || n < note->used;
    return 1;
}

void document_note_cancel(document_note_request_t *note)
{
    if (note) document_close(note->document);
}
