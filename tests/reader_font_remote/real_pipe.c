/* Transport and ACPU stack doubles; both pipe implementations are production. */
#include "epd_compute_pipe.h"
#include "epd_compute_fiber.h"
#include "epd_compute_memory.h"
#include "epd_memory.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
extern const epd_compute_ops_t epd_reader_font_remote_ops;

typedef struct buffer { struct buffer *next; void *data; uint32_t capacity, id; } buffer_t;
static buffer_t *buffers;
static uint32_t buffer_id;
static bool close_fault, open_fault;
static volatile LONG allocation_fault;
void real_allocation_fault(bool enabled) { InterlockedExchange(&allocation_fault, enabled); }
void real_open_fault(bool enabled) { open_fault = enabled; }
void real_close_fault(bool enabled) { close_fault = enabled; }
void real_no_buffers(void) { assert(!buffers); }
struct epd_compute_session {
    void *state;
    epd_compute_wire_t wire;
    epd_compute_region_t regions[2];
    volatile LONG cancelled;
};
static __declspec(thread) epd_compute_session_t current;
void *rt_calloc(size_t n, size_t size) { return calloc(n, size); }
void rt_free(void *p) { free(p); }
void *epd_compute_malloc(size_t bytes) { return allocation_fault ? NULL : malloc(bytes); }
void *epd_compute_calloc(size_t n, size_t bytes) { return calloc(n, bytes); }
void epd_compute_free(void *p) { free(p); }
bool epd_compute_buffer_alloc(epd_compute_buffer_t *out, size_t size)
{
    buffer_t *b = calloc(1, sizeof(*b)); assert(b);
    b->capacity = (uint32_t)((size + 31) & ~(size_t)31);
    b->data = epd_app_alloc(b->capacity + 31u, EPD_APP_PSRAM); assert(b->data); memset(b->data, 0, b->capacity);
    b->id = ++buffer_id; b->next = buffers; buffers = b;
    out->allocation = b; out->data = b->data; out->capacity = b->capacity; return true;
}
void epd_compute_buffer_free(epd_compute_buffer_t *out)
{
    buffer_t *b = out->allocation; if (!b) return;
    buffer_t **p = &buffers; while (*p != b) p = &(*p)->next; *p = b->next;
    epd_app_free(b->data); free(b); memset(out, 0, sizeof(*out));
}
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *b, uint32_t length, uint32_t access)
{ epd_compute_region_t r = {((buffer_t *)b->allocation)->id, b->capacity, length, access}; return r; }
void *epd_compute_resolve(const epd_compute_wire_t *wire, epd_compute_ref_t ref, uint32_t access)
{
    (void)wire; assert(current);
    if (ref.region >= 2) return NULL;
    epd_compute_region_t *r = &current->regions[ref.region];
    if ((r->access & access) != access || ref.offset > r->length || ref.length > r->length - ref.offset) return NULL;
    for (buffer_t *b = buffers; b; b = b->next) if (b->id == r->address) return (uint8_t *)b->data + ref.offset;
    return NULL;
}
uint32_t epd_compute_signals(const epd_compute_wire_t *wire)
{ (void)wire; return current && current->cancelled ? EPD_COMPUTE_CANCEL : 0; }
static void begin(epd_compute_session_t s, const epd_compute_request_t *req)
{
    current = s;
    if (req) {
        assert(req->region_count == 2); memcpy(s->regions, req->regions, sizeof(s->regions));
        s->wire.params = req->params; s->wire.result = req->result; s->wire.region_count = 2;
    }
}
int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *cap)
{ (void)name; (void)version; memset(cap, 0, sizeof(*cap)); cap->ready = true;
  cap->registration.flags = EPD_COMPUTE_STEP_SHARED; return 0; }
int epd_compute_open(const epd_compute_capability_t *cap, const epd_compute_requirements_t *requirements,
    const epd_compute_request_t *req, epd_compute_session_t *out)
{
    (void)cap; (void)requirements;
    if (!IsThreadAFiber()) assert(ConvertThreadToFiber(NULL));
    epd_compute_session_t s = calloc(1, sizeof(*s)); assert(s); begin(s, req);
    int result = epd_reader_font_remote_ops.open(&s->state, &s->wire); current = NULL;
    if (result) { free(s); return result; }
    *out = s; return open_fault ? EPD_COMPUTE_COMPONENT_ERROR : 0;
}
int epd_compute_open_wait(const epd_compute_capability_t *cap, const epd_compute_requirements_t *requirements,
    const epd_compute_request_t *req, epd_compute_wait_t wait, epd_compute_session_t *out)
{
    /* This integration has no admission contention; scheduler wait semantics
     * are covered by tests/compute_scheduler. The font uses ordinary open. */
    (void)cap; (void)requirements; (void)req; (void)wait; (void)out;
    assert(!"font integration unexpectedly used admission wait"); return EPD_COMPUTE_INVALID;
}
int epd_compute_step(epd_compute_session_t s, const epd_compute_request_t *req)
{ begin(s, req); int result = epd_reader_font_remote_ops.step(s->state, &s->wire); current = NULL; return result; }
int epd_compute_set_purpose(epd_compute_session_t s, epd_compute_purpose_t purpose)
{ (void)s; (void)purpose; return 0; }
void epd_compute_cancel(epd_compute_session_t s) { InterlockedExchange(&s->cancelled, 1); }
int epd_compute_close(epd_compute_session_t s)
{
    if (close_fault) return EPD_COMPUTE_COMPONENT_ERROR;
    begin(s, NULL); epd_reader_font_remote_ops.close(s->state, &s->wire); current = NULL; free(s); return 0;
}

struct epd_compute_fiber {
    void *parent, *fiber;
    epd_compute_fiber_entry_t entry;
    void *user;
    const epd_compute_wire_t *request;
    bool closing, done;
    int32_t status;
};
static VOID WINAPI fiber_entry(void *user)
{
    epd_compute_fiber_t *f = user;
    f->status = f->closing ? EPD_COMPUTE_CANCELLED : f->entry(f, f->user);
    f->done = true; SwitchToFiber(f->parent);
}
epd_compute_fiber_t *epd_compute_fiber_create(const char *name, size_t stack,
    epd_compute_fiber_entry_t entry, void *user)
{
    (void)name; (void)stack; epd_compute_fiber_t *f = calloc(1, sizeof(*f)); assert(f);
    f->entry = entry; f->user = user; f->parent = GetCurrentFiber();
    f->fiber = CreateFiber(256 * 1024, fiber_entry, f); assert(f->fiber); return f;
}
int32_t epd_compute_fiber_step(epd_compute_fiber_t *f, const epd_compute_wire_t *r)
{
    if (f->done) return f->status;
    f->request = r; SwitchToFiber(f->fiber); f->request = NULL;
    return f->done ? f->status : EPD_COMPUTE_MORE;
}
bool epd_compute_fiber_cancelled(const epd_compute_fiber_t *f)
{ return !f || f->closing || !f->request || epd_compute_signals(f->request) & EPD_COMPUTE_CANCEL; }
bool epd_compute_fiber_should_yield(const epd_compute_fiber_t *f) { (void)f; return false; }
bool epd_compute_fiber_yield(epd_compute_fiber_t *f)
{ if (epd_compute_fiber_cancelled(f)) return false; SwitchToFiber(f->parent); return !epd_compute_fiber_cancelled(f); }
const epd_compute_wire_t *epd_compute_fiber_request(const epd_compute_fiber_t *f) { return f->request; }
void epd_compute_fiber_close(epd_compute_fiber_t *f)
{
    f->closing = true;
    if (!f->done) { SwitchToFiber(f->fiber); assert(f->done); }
    DeleteFiber(f->fiber); free(f);
}

/* Host execution serializes remote steps and close. */
void epd_compute_barrier(void) {}
