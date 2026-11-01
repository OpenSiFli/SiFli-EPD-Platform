/* SPDX-License-Identifier: Apache-2.0 */
static int metadata_output(document_remote_host_t *host, const book_document_event_t *e,
                            const uint8_t *payload, const char *title)
{
    document_t *d = host->document;
    if (e->type == BOOK_DOCUMENT_METADATA && e->a == BOOK_DOCUMENT_META_BOOK && !host->image_id) {
        while (isspace((unsigned char)*title)) ++title;
        size_t n = strlen(title);
        while (n && isspace((unsigned char)title[n - 1])) --n;
        if (n >= sizeof(d->status.title)) {
            n = sizeof(d->status.title) - 1;
            while (n && ((unsigned char)title[n] & 0xc0) == 0x80) --n;
        }
        memcpy(d->status.title, title, n); d->status.title[n] = 0;
        return BOOK_OK;
    }
    if (e->type == BOOK_DOCUMENT_IMAGE_BEGIN) {
        if (!e->a || !e->b || !e->c || e->b > DOCUMENT_COVER_WIDTH || e->c > DOCUMENT_COVER_HEIGHT ||
            e->d != (e->b + 1) / 2 || host->image_id || d->cover.memory) return BOOK_ERROR;
        if (!bitmap_allocate(&d->cover, e->b, e->c)) return BOOK_NO_MEMORY;
        host->image_id = e->a; host->image_row = 0;
        host->image_width = e->b; host->image_height = e->c; host->image_stride = e->d;
        return BOOK_OK;
    }
    if (e->type == BOOK_DOCUMENT_IMAGE_DATA) {
        if (!host->image_id || e->a != host->image_id || e->b != host->image_row || !e->c ||
            e->c > host->image_height - host->image_row || e->d != host->image_stride ||
            e->length1 != e->c * e->d || e->length2) return BOOK_ERROR;
        for (unsigned y = 0; y < e->c; ++y) {
            uint16_t *out = (uint16_t *)(d->cover.image.data + (e->b + y) * d->cover.image.header.stride);
            for (unsigned x = 0; x < host->image_width; ++x) {
                unsigned gray = ((payload[y * e->d + x / 2] >> (x & 1 ? 0 : 4)) & 15) * 17;
                out[x] = (uint16_t)((gray >> 3) << 11 | (gray >> 2) << 5 | (gray >> 3));
            }
        }
        host->image_row += e->c;
        return BOOK_OK;
    }
    if (e->type == BOOK_DOCUMENT_IMAGE_END && host->image_id && e->a == host->image_id &&
        host->image_row == host->image_height) { host->image_id = 0; return BOOK_OK; }
    return BOOK_ERROR;
}

document_t *document_metadata_open(const bookshelf_item_t *file)
{
    if (!file || (file->format != DOCUMENT_EPUB && file->format != DOCUMENT_SPLIT_EPUB)) return NULL;
    if (worker_stopping) {
        if (documents || worker_service || worker_document) return NULL;
        worker_stopping = false;
    }
    document_t *d = allocate(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->metadata_only = true; d->file = *file; d->format = file->format; d->owners = 1;
    d->input = d->output = -1; d->generation = storage_card_session();
    d->opened_at = rt_tick_get();
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping) { rt_hw_interrupt_enable(key); epd_app_free(d); return NULL; }
    d->demand = d->serial = ++sequence;
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return d;
}

int document_metadata_take(document_t *d, char *title, size_t capacity, document_bitmap_t *cover)
{
    if (!d || !d->metadata_only || !title || !capacity || !cover || cancelled(d)) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    bool done = d->status.complete && !d->converting;
    rt_hw_interrupt_enable(key);
    if (!done) return 0;
    size_t n = strlen(d->status.title);
    if (n >= capacity) {
        n = capacity - 1;
        while (n && ((unsigned char)d->status.title[n] & 0xc0) == 0x80) --n;
    }
    memcpy(title, d->status.title, n); title[n] = 0;
    memset(cover, 0, sizeof(*cover));
    if (d->status.result == BOOK_OK) { *cover = d->cover; memset(&d->cover, 0, sizeof(d->cover)); }
    return 1;
}
