/* SPDX-License-Identifier: Apache-2.0 */
/* Synchronous admission hooks exercise cancellation while handles are published. */
static unsigned waits_created, waits_destroyed, wait_cancels, wait_failures;
static unsigned pipe_shutdown_failures, pipe_shutdown_retries;
static unsigned render_opens, render_closes, render_cancels, render_shutdown_failures, render_retries;
static unsigned hash_opens, hash_closes, hash_calls, hash_cancels, hash_close_failures, hash_retries;
static unsigned buffer_count;
static int pipe_open_status;
static bool pipe_open_partial;
static void (*pipe_wait_probe)(void), (*render_wait_probe)(void), (*render_step_probe)(void);
static void (*hash_probe)(void);

struct epd_compute_wait { bool cancelled, active; };
int epd_compute_wait_create(epd_compute_wait_t *out)
{
    *out = NULL;
    if (wait_failures) { --wait_failures; return EPD_COMPUTE_NO_MEMORY; }
    *out = calloc(1, sizeof(**out)); assert(*out); ++waits_created;
    return EPD_COMPUTE_OK;
}
void epd_compute_wait_cancel(epd_compute_wait_t w)
{ if (w) { w->cancelled = true; ++wait_cancels; } }
int epd_compute_wait_destroy(epd_compute_wait_t w)
{
    if (!w) return EPD_COMPUTE_OK;
    assert(!w->active);
    for (document_t *d = documents; d; d = d->next) assert(d->compute_wait != w);
    ++waits_destroyed; free(w); return EPD_COMPUTE_OK;
}
static void admission_probe(epd_compute_wait_t w, void (*probe)(void))
{
    if (worker_document) current_document = worker_document;
    assert(w && current_document->compute_wait == w && !w->active);
    w->active = true;
    if (probe) probe();
    assert(current_document->compute_wait == w);
    w->active = false;
}
int epd_compute_pipe_open_wait(const char *name, uint32_t version, epd_compute_purpose_t purpose,
    const epd_compute_pipe_message_t *initial, const void *data, size_t length,
    epd_compute_wait_t wait, epd_compute_pipe_host_t *out)
{
    *out = NULL; admission_probe(wait, pipe_wait_probe);
    assert(!irq_depth && !storage_depth);
    if (wait->cancelled) return EPD_COMPUTE_CANCELLED;
    if (pipe_open_status) {
        if (pipe_open_partial) assert(epd_compute_pipe_open(name, version, purpose, initial, data, length, out) == EPD_COMPUTE_OK);
        return pipe_open_status;
    }
    return epd_compute_pipe_open(name, version, purpose, initial, data, length, out);
}
int epd_compute_pipe_shutdown(epd_compute_pipe_host_t *p)
{
    if (!*p) return EPD_COMPUTE_OK;
    epd_compute_pipe_cancel(*p);
    if (pipe_shutdown_failures) {
        --pipe_shutdown_failures; ++pipe_shutdown_retries; return EPD_COMPUTE_COMPONENT_ERROR;
    }
    epd_compute_pipe_close(*p); *p = NULL; return EPD_COMPUTE_OK;
}

struct epd_reader_render_client {
    struct epd_reader_render_client *next;
    epd_compute_buffer_t frame, source;
    rr_kernel kernel;
    unsigned id;
    bool cancelled, active;
};
static epd_reader_render_t live_renders;
static void hcpu_before_free(void *memory)
{
    if (!memory) return;
    assert(memory != borrowed_save);
    for (epd_reader_render_t r = live_renders; r; r = r->next) {
        assert(memory != r->frame.allocation);
        assert(memory != r->source.allocation);
    }
}
bool epd_compute_buffer_alloc(epd_compute_buffer_t *b, size_t n)
{
    memset(b, 0, sizeof(*b));
    b->capacity = (uint32_t)RT_ALIGN(n, EPD_COMPUTE_CACHE_LINE);
    b->allocation = epd_app_alloc(b->capacity + EPD_COMPUTE_CACHE_LINE - 1, EPD_APP_PSRAM);
    if (!b->allocation) return false;
    b->data = (void *)RT_ALIGN((uintptr_t)b->allocation, EPD_COMPUTE_CACHE_LINE);
    ++buffer_count; return true;
}
void epd_compute_buffer_free(epd_compute_buffer_t *b)
{
    if (b->allocation) { assert(buffer_count); --buffer_count; epd_app_free(b->allocation); }
    memset(b, 0, sizeof(*b));
}
int epd_reader_render_open(const epd_compute_buffer_t *b, rr_frame f, epd_compute_purpose_t p,
    epd_reader_render_t *out)
{
    (void)p;
    epd_reader_render_t r = calloc(1, sizeof(*r)); assert(r);
    assert(rr_kernel_bind(&r->kernel, f, b->data, b->capacity) == RR_OK);
    r->frame = *b; r->next = live_renders; live_renders = r;
    *out = r; ++render_opens; return EPD_COMPUTE_OK;
}
int epd_reader_render_open_wait(const epd_compute_buffer_t *b, rr_frame f,
    epd_compute_purpose_t p, epd_compute_wait_t w, epd_reader_render_t *out)
{
    *out = NULL; admission_probe(w, render_wait_probe);
    assert(!irq_depth && !storage_depth);
    if (w->cancelled) return EPD_COMPUTE_CANCELLED;
    return epd_reader_render_open(b, f, p, out);
}
void epd_reader_render_cancel(epd_reader_render_t r)
{ if (r) { r->cancelled = true; ++render_cancels; } }
int epd_reader_render_command(epd_reader_render_t r, const rr_command *command, const void *p, size_t n,
    bool (*cancel)(void *), void *u, rr_result *out)
{
    assert(r && !r->active && current_document->image_render == r);
    assert(!irq_depth && !storage_depth);
    r->active = true;
    if (command->operation == RR_SCALE_GRAY4_TO_RGB565 && render_step_probe) render_step_probe();
    if (r->cancelled || (cancel && cancel(u))) { r->active = false; return EPD_COMPUTE_CANCELLED; }
    rr_command c = *command; c.id = ++r->id;
    rr_result result; int rc;
    do { rc = rr_kernel_step(&r->kernel, &c, p, n, NULL, NULL, &result); } while (rc == RR_MORE);
    assert(rc == RR_OK);
    if (out) *out = result;
    r->active = false; return EPD_COMPUTE_OK;
}
int epd_reader_render_command_shared(epd_reader_render_t r, const rr_command *c,
    const epd_compute_buffer_t *p, uint32_t n, bool (*cancel)(void *), void *u, rr_result *out)
{
    assert(n <= p->capacity); r->source = *p;
    return epd_reader_render_command(r, c, p->data, n, cancel, u, out);
}
int epd_reader_render_close(epd_reader_render_t r)
{
    if (!r) return EPD_COMPUTE_OK;
    assert(!r->active);
    if (render_shutdown_failures) {
        --render_shutdown_failures; ++render_retries; return EPD_COMPUTE_COMPONENT_ERROR;
    }
    epd_reader_render_t *link = &live_renders;
    while (*link && *link != r) link = &(*link)->next;
    assert(*link == r); *link = r->next;
    ++render_closes; free(r); return EPD_COMPUTE_OK;
}
int epd_reader_render_shutdown(epd_reader_render_t *r)
{
    epd_reader_render_cancel(*r);
    int rc = epd_reader_render_close(*r);
    if (rc == EPD_COMPUTE_OK) *r = NULL;
    return rc;
}

struct epd_reading_data_client { bool active, cancelled; };
int epd_reading_data_create(epd_compute_purpose_t purpose, epd_reading_data_t *out)
{
    (void)purpose; *out = calloc(1, sizeof(**out)); assert(*out); ++hash_opens;
    return EPD_COMPUTE_OK;
}
void epd_reading_data_cancel(epd_reading_data_t c)
{ if (c) { c->cancelled = true; ++hash_cancels; } }
static uint32_t reference_fnv(const void *data, size_t size)
{
    const uint8_t *p = data; uint32_t h = EPD_READING_DATA_FNV_SEED;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}
int epd_reading_data_hash(epd_reading_data_t c, const void *data, size_t size, uint32_t *hash)
{
    assert(!c->active && *hash == EPD_READING_DATA_FNV_SEED);
    assert(!irq_depth && !storage_depth);
    c->active = true; ++hash_calls;
    if (hash_probe) hash_probe();
    c->active = false;
    if (c->cancelled) return EPD_COMPUTE_CANCELLED;
    *hash = reference_fnv(data, size); return EPD_COMPUTE_OK;
}
int epd_reading_data_close(epd_reading_data_t *c)
{
    if (!*c) return EPD_COMPUTE_OK;
    assert(!(*c)->active);
    if (hash_close_failures) {
        --hash_close_failures; ++hash_retries; return EPD_COMPUTE_COMPONENT_ERROR;
    }
    ++hash_closes; free(*c); *c = NULL; return EPD_COMPUTE_OK;
}
static void assert_compute_released(void)
{
    assert(waits_created == waits_destroyed && pipe_opens == pipe_closes);
    assert(render_opens == render_closes && hash_opens == hash_closes);
    assert(!buffer_count && !live_renders);
    assert(!observers && observe_begins == observe_ends && !conversion_document);
}
