/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#define EPD_COMPUTE_ACPU
#include "../../src/platform/epd_compute.c"
void epd_compute_pm_report(void) {}
#define changed sleep_guard_changed
#include "../../src/services/sleep_guard.c"
#undef changed

static CRITICAL_SECTION irq;
static __declspec(thread) unsigned irq_depth, critical_depth, interrupt_depth;
static __declspec(thread) bool scheduler_not_started;
static __declspec(thread) rt_thread_t thread_self;
static struct rt_thread test_thread;
static LONG power_events;
void epd_power_notify(void) { InterlockedIncrement(&power_events); }
rt_uint8_t rt_interrupt_get_nest(void) { return (rt_uint8_t)interrupt_depth; }
rt_uint16_t rt_critical_level(void) { return (rt_uint16_t)critical_depth; }
rt_thread_t rt_thread_self(void)
{ return scheduler_not_started ? NULL : thread_self ? thread_self : &test_thread; }
static CONDITION_VARIABLE changed;
static HANDLE transport_gate;
static bool gated, fail_transport, fail_open, ignore_cancel;
static bool fail_closes;
static unsigned fail_open_handle;
#ifdef COMPUTE_MEMORY_TEST
static void *memory_alloc(size_t, epd_app_memory_t);
static void memory_free(void *);
static void memory_response(epd_compute_wire_t *);
#endif
static LONG semaphores;
static LONG allocations, fail_allocation;
uintptr_t test_sram_start = 1, test_sram_size = UINTPTR_MAX - 1;
static unsigned init_failure, init_detaches, transport_detaches;
static void *resident_stack;
static unsigned calls, remote_sessions, next_session, opens, closes, cleans, invalidates;
static unsigned log_session[4096], log_operation[4096], log_capability[4096];
static const epd_compute_request_t empty_request = {0};
static LONG diagnostic_lines;
static bool manual_clock;
static LONG manual_ticks;
static unsigned transport_ticks, maintenance_ticks;
static LONG remote_summaries;
static bool inject_summary, inspect_prefix;
#ifdef COMPUTE_CACHE_MODEL
void model_alloc(void *, size_t, bool);
void model_free(void *);
void model_cache(uint32_t, uint32_t, bool);
void model_switch(epd_compute_wire_t *, bool);
bool model_transport_fail(epd_compute_wire_t *);
#endif
static int last_remote_status;
rt_tick_t rt_tick_get(void)
{ return manual_clock ? (uint32_t)InterlockedCompareExchange(&manual_ticks, 0, 0) : GetTickCount(); }
void rt_kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char line[256];
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fputs(line, stdout);
    if (!strncmp(line, "cmp A id=", 9) && strstr(line, "end="))
    {
        const char *status = strstr(line, "status=");
        assert(status); last_remote_status = atoi(status + 7);
        InterlockedIncrement(&remote_summaries);
    }
    InterlockedIncrement(&diagnostic_lines);
}

#ifndef COMPUTE_REAL_COMPONENTS
const epd_compute_registration_t epd_compute_capabilities[] = {
    {"shared", 1, 7, EPD_COMPUTE_VALIDATED | EPD_COMPUTE_STEP_SHARED, 16},
    {"exclusive", 1, 7, EPD_COMPUTE_VALIDATED, 16},
    {"background", 1, EPD_COMPUTE_PURPOSE(EPD_COMPUTE_BACKGROUND),
     EPD_COMPUTE_VALIDATED | EPD_COMPUTE_STEP_SHARED, 16},
    {"disabled", 1, 7, EPD_COMPUTE_STEP_SHARED, 16}
};
const uint32_t epd_compute_capability_count = 4;
#endif

rt_base_t rt_hw_interrupt_disable(void)
{ EnterCriticalSection(&irq); return irq_depth++ ? 1 : 0; }
void rt_hw_interrupt_enable(rt_base_t level)
{
    assert(irq_depth && (level != 0) == (irq_depth > 1));
    --irq_depth;
    WakeAllConditionVariable(&changed);
    LeaveCriticalSection(&irq);
}
int rt_sem_init(struct rt_semaphore *s, const char *name, unsigned value, unsigned flags)
{
    if (init_failure == 4) return -RT_ENOMEM;
    (void)name; (void)flags;
    s->handle = CreateSemaphoreA(NULL, (LONG)value, 65535, NULL);
    if (s->handle) InterlockedIncrement(&semaphores);
    return s->handle ? RT_EOK : -RT_ENOMEM;
}
int rt_sem_take(struct rt_semaphore *s, int ticks)
{
    /* A finite timeout would reintroduce cancellation/busy polling. */
    assert(ticks == RT_WAITING_FOREVER || ticks == 0);
    if (ticks) assert(!irq_depth && !critical_depth && !interrupt_depth && !scheduler_not_started);
    return WaitForSingleObject(s->handle, ticks == 0 ? 0 : INFINITE) == WAIT_OBJECT_0 ? RT_EOK : -1;
}
int rt_sem_release(struct rt_semaphore *s) { assert(ReleaseSemaphore(s->handle, 1, NULL)); return RT_EOK; }
int rt_sem_detach(struct rt_semaphore *s)
{ assert(CloseHandle(s->handle)); InterlockedDecrement(&semaphores); return RT_EOK; }
static DWORD WINAPI thread_entry(LPVOID p)
{
    struct rt_thread *t = p;
    thread_self = t;
    t->entry(t->argument);
    return 0;
}
int rt_thread_init(rt_thread_t t, const char *name, void (*entry)(void *), void *arg,
                   void *stack, size_t size, unsigned priority_value, unsigned ticks)
{
    (void)name; (void)priority_value; (void)ticks;
    assert(stack && size == DISPATCHER_STACK_SIZE);
    assert((uintptr_t)stack >= test_sram_start &&
           size <= test_sram_size - ((uintptr_t)stack - test_sram_start));
    if (init_failure == 5) return -RT_ENOMEM;
    resident_stack = stack;
    t->entry = entry; t->argument = arg;
    return RT_EOK;
}
int rt_thread_startup(rt_thread_t t)
{
    if (init_failure == 7) return -RT_ERROR;
    t->handle = CreateThread(NULL, 0, thread_entry, t, 0, NULL);
    assert(t->handle);
    return RT_EOK;
}
int rt_thread_detach(rt_thread_t t) { assert(t == &dispatcher); ++init_detaches; resident_stack = NULL; return RT_EOK; }
void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    (void)region;
    if (init_failure == 1 || InterlockedExchange(&fail_allocation, 0)) return NULL;
#ifdef COMPUTE_MEMORY_TEST
    void *p = memory_alloc(size, region);
#else
    void *p = malloc(size);
#endif
    if (p && init_failure == 2) test_sram_size = (uintptr_t)p - test_sram_start;
    if (p && init_failure == 3) { test_sram_start = (uintptr_t)p; test_sram_size = size - 1; }
    if (p && init_failure == 8) { test_sram_start = (uintptr_t)p; test_sram_size = size; }
#ifdef COMPUTE_CACHE_MODEL
    if (p) model_alloc(p, size, region == EPD_APP_PSRAM);
#endif
    if (p) InterlockedIncrement(&allocations);
    return p;
}
void epd_app_free(void *p) { if (p) {
#ifdef COMPUTE_CACHE_MODEL
    model_free(p);
#endif
    InterlockedDecrement(&allocations);
#ifdef COMPUTE_MEMORY_TEST
    memory_free(p);
#else
    free(p);
#endif
    } }
void epd_compute_barrier(void) { MemoryBarrier(); }
void epd_compute_cache_clean(uint32_t address, uint32_t capacity)
{ assert(address && capacity); ++cleans; InterlockedExchangeAdd(&manual_ticks, (LONG)maintenance_ticks);
#ifdef COMPUTE_CACHE_MODEL
    model_cache(address, capacity, true);
#endif
}
void epd_compute_cache_invalidate(uint32_t address, uint32_t capacity)
{ assert(address && capacity); ++invalidates; InterlockedExchangeAdd(&manual_ticks, (LONG)maintenance_ticks);
#ifdef COMPUTE_CACHE_MODEL
    model_cache(address, capacity, false);
#endif
}
void epd_compute_cache_clean_invalidate(uint32_t address, uint32_t capacity)
{
    epd_compute_cache_clean(address, capacity);
    epd_compute_cache_invalidate(address, capacity);
}
int epd_compute_transport_init(rt_thread_t owner) { assert(owner == &dispatcher); return init_failure == 6 ? -RT_ENOMEM : RT_EOK; }
static unsigned transport_starts;
void epd_compute_transport_start(void)
{
    assert(rt_thread_self() == &dispatcher && epd_compute_can_wait());
    assert(!transport_starts && !calls);
    ++transport_starts;
}
void epd_compute_transport_detach(void) { ++transport_detaches; }
int epd_compute_transport_run(void *p, uint32_t size, uint8_t *error)
{
    assert(transport_starts == 1);
    epd_compute_wire_t *wire = p;
    assert(size == sizeof(*wire));
    assert(!wire->diagnostics.flags);
    if (inspect_prefix) assert(wire->diagnostics.attempts == 0x12345678u);
    rt_base_t level = rt_hw_interrupt_disable();
    assert(running && running->state == SESSION_RUNNING && calls < 4096);
    log_session[calls] = wire->session;
    log_capability[calls] = wire->capability;
    log_operation[calls++] = wire->operation;
    bool wait = gated;
    rt_hw_interrupt_enable(level);
    if (wait) assert(WaitForSingleObject(transport_gate, 5000) == WAIT_OBJECT_0);
    InterlockedExchangeAdd(&manual_ticks, (LONG)transport_ticks);
#ifdef COMPUTE_REAL_COMPONENTS
#ifdef COMPUTE_CACHE_MODEL
    if (model_transport_fail(wire)) return -1;
    model_switch(wire, true);
#endif
    epd_compute_remote_dispatch(wire);
#ifdef COMPUTE_CACHE_MODEL
    model_switch(wire, false);
#endif
    level = rt_hw_interrupt_disable();
    if (wire->status == EPD_COMPUTE_OK) {
        if (wire->operation == EPD_COMPUTE_OPEN) { ++opens; ++remote_sessions; }
        if (wire->operation == EPD_COMPUTE_CLOSE) { ++closes; --remote_sessions; }
    }
    *error = ACPU_ERR_OK;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
#else
    level = rt_hw_interrupt_disable();
    if (fail_transport || (fail_closes && wire->operation == EPD_COMPUTE_CLOSE))
    {
        fail_transport = false;
        rt_hw_interrupt_enable(level);
        return -1;
    }
    wire->status = EPD_COMPUTE_OK;
    if (wire->operation == EPD_COMPUTE_OPEN)
    {
        ++opens;
        if (fail_open) wire->status = EPD_COMPUTE_COMPONENT_ERROR;
        else { wire->session = ++next_session; ++remote_sessions; }
        if (fail_open_handle) {
            wire->status = EPD_COMPUTE_COMPONENT_ERROR;
            if (fail_open_handle == 2) { rt_hw_interrupt_enable(level); return -1; }
        }
    }
    else if (wire->operation == EPD_COMPUTE_CLOSE)
    {
        assert(remote_sessions); --remote_sessions; ++closes; wire->session = 0;
    }
    else if ((running->signals & EPD_COMPUTE_CANCEL) && !ignore_cancel)
        wire->status = EPD_COMPUTE_CANCELLED;
    else if (wire->operation == EPD_COMPUTE_STEP) wire->status = EPD_COMPUTE_MORE;
    wire->completed_sequence = wire->sequence;
    if (inject_summary)
    {
        inject_summary = false;
        if (wire->operation == EPD_COMPUTE_STEP) wire->status = EPD_COMPUTE_NO_MEMORY;
        epd_compute_diag_wire_t *d = &wire->diagnostics;
        memset(d, 0, sizeof(*d));
        d->status = wire->status; d->session = running->diagnostics.remote_id; d->tick_hz = 1000;
        d->ticks[1] = 321; d->calls[1] = 20; d->step_max = 22;
        d->charged_peak = 54321; d->heap_peak = 60000; d->system_peak = 2000;
        d->flags = EPD_COMPUTE_DIAG_VALID | EPD_COMPUTE_DIAG_PSRAM_PRESENT |
                   EPD_COMPUTE_DIAG_SYSTEM_PRESENT;
        if (wire->operation == EPD_COMPUTE_CLOSE) d->flags |= EPD_COMPUTE_DIAG_CLOSE;
    }
    *error = ACPU_ERR_OK;
#ifdef COMPUTE_MEMORY_TEST
    memory_response(wire);
#endif
    rt_hw_interrupt_enable(level);
    return RT_EOK;
#endif
}

typedef struct
{
    HANDLE thread;
    epd_compute_session_t session;
    epd_compute_wait_t wait;
    unsigned capability, operation;
    epd_compute_purpose_t purpose;
    epd_compute_request_t request;
    int result;
} job_t;

static DWORD WINAPI run_job(LPVOID p)
{
    job_t *j = p;
    if (j->operation == EPD_COMPUTE_OPEN)
    {
        epd_compute_capability_t c = {0}; c.index = j->capability;
        epd_compute_requirements_t req = {j->purpose, 1};
        j->result = j->wait ? epd_compute_open_wait(&c, &req, &j->request, j->wait, &j->session) :
                             epd_compute_open(&c, &req, &j->request, &j->session);
    }
    else if (j->operation == EPD_COMPUTE_CLOSE) j->result = epd_compute_close(j->session);
    else j->result = epd_compute_step(j->session, &j->request);
    return 0;
}
static void start(job_t *j)
{
    j->result = 999;
    j->thread = CreateThread(NULL, 0, run_job, j, 0, NULL);
    assert(j->thread);
}
static void join(job_t *j, int result)
{
    assert(WaitForSingleObject(j->thread, 5000) == WAIT_OBJECT_0);
    assert(j->result == result);
    CloseHandle(j->thread); j->thread = NULL;
}
static epd_compute_session_t open_as(unsigned capability, epd_compute_purpose_t purpose)
{
    epd_compute_capability_t c = {0}; c.index = capability;
    epd_compute_requirements_t req = {purpose, 1};
    epd_compute_session_t s;
    assert(epd_compute_open(&c, &req, &empty_request, &s) == EPD_COMPUTE_OK);
    return s;
}
static void close_session(epd_compute_session_t s) { assert(epd_compute_close(s) == EPD_COMPUTE_OK); }
static job_t step_job(epd_compute_session_t s)
{
    job_t j = {0}; j.session = s; j.operation = EPD_COMPUTE_STEP; return j;
}
static job_t open_job(unsigned capability)
{
    job_t j = {0}; j.capability = capability; j.operation = EPD_COMPUTE_OPEN;
    j.purpose = EPD_COMPUTE_BACKGROUND;
    assert(epd_compute_wait_create(&j.wait) == EPD_COMPUTE_OK);
    return j;
}
static unsigned queued_count(void)
{
    unsigned count = 0;
    for (epd_compute_session_t s = queue_head; s; s = s->queue_next) ++count;
    return count;
}
static unsigned admission_count(void)
{
    unsigned count = 0;
    for (epd_compute_wait_t w = admission_head; w; w = w->next) ++count;
    return count;
}
/* Test coordination waits for state transitions; deadlines only detect hangs. */
static void await_counts(unsigned transports, unsigned queued, unsigned admissions)
{
    EnterCriticalSection(&irq);
    while (calls < transports || queued_count() != queued || admission_count() != admissions)
        assert(SleepConditionVariableCS(&changed, &irq, 5000));
    LeaveCriticalSection(&irq);
}
static void gate_on(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    assert(!running); gated = true; calls = 0;
    rt_hw_interrupt_enable(level);
}
static void permit(void) { assert(ReleaseSemaphore(transport_gate, 1, NULL)); }
static void gate_off(void)
{
    rt_base_t level = rt_hw_interrupt_disable(); gated = false; rt_hw_interrupt_enable(level);
}
static void finish_wait(job_t *j) { assert(epd_compute_wait_destroy(j->wait) == EPD_COMPUTE_OK); }

static void shared_persistence(void)
{
    epd_compute_session_t reader = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_session_t audio = open_as(0, EPD_COMPUTE_CONTINUOUS);
    unsigned id = reader->wire.session;
    for (unsigned i = 0; i < 10; ++i)
    {
        assert(epd_compute_set_purpose(reader, i % 2 ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND) == 0);
        assert(epd_compute_step(audio, &empty_request) == EPD_COMPUTE_MORE);
        assert(epd_compute_step(reader, &empty_request) == EPD_COMPUTE_MORE);
        assert(reader->wire.session == id);
    }
    assert(epd_compute_reset(reader, &empty_request) == 0 && reader->wire.session == id);
    assert(opens == 2 && closes == 0);
    close_session(audio); close_session(reader);
}

static void priorities_fifo(void)
{
    epd_compute_session_t s[4];
    s[0] = open_as(0, EPD_COMPUTE_BACKGROUND);
    s[1] = open_as(0, EPD_COMPUTE_FOREGROUND);
    s[2] = open_as(0, EPD_COMPUTE_FOREGROUND);
    s[3] = open_as(0, EPD_COMPUTE_CONTINUOUS);
    gate_on();
    job_t j[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        j[i] = step_job(s[i]); start(&j[i]); await_counts(1, i, 0);
    }
    assert(s[0]->signals & EPD_COMPUTE_YIELD);
    const unsigned order[] = {0, 3, 1, 2};
    for (unsigned i = 0; i < 4; ++i)
    {
        await_counts(i + 1, 3 - i, 0);
        assert(log_session[i] == s[order[i]]->wire.session);
        permit(); join(&j[order[i]], EPD_COMPUTE_MORE);
    }
    gate_off();
    for (unsigned i = 0; i < 4; ++i) close_session(s[i]);
}

static void bounded_background(void)
{
    enum { N = 14 };
    epd_compute_session_t s[N]; job_t j[N];
    for (unsigned i = 0; i < N; ++i)
        s[i] = open_as(0, i == 1 ? EPD_COMPUTE_BACKGROUND : EPD_COMPUTE_CONTINUOUS);
    gate_on();
    for (unsigned i = 0; i < N; ++i)
    { j[i] = step_job(s[i]); start(&j[i]); await_counts(1, i, 0); }
    unsigned background_at = N;
    for (unsigned i = 0; i < N; ++i)
    {
        await_counts(i + 1, N - i - 1, 0);
        if (log_session[i] == s[1]->wire.session)
        {
            background_at = i;
            assert(!(s[1]->signals & EPD_COMPUTE_YIELD));
        }
        permit();
    }
    for (unsigned i = 0; i < N; ++i) join(&j[i], EPD_COMPUTE_MORE);
    assert(background_at == EPD_COMPUTE_MAX_BYPASSES + 1);
    gate_off();
    for (unsigned i = 0; i < N; ++i) close_session(s[i]);
}

static void change_purpose(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_CONTINUOUS);
    epd_compute_session_t b = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_session_t c = open_as(0, EPD_COMPUTE_FOREGROUND);
    gate_on(); job_t ja = step_job(a), jb = step_job(b), jc = step_job(c);
    start(&ja); await_counts(1, 0, 0);
    start(&jb); await_counts(1, 1, 0);
    start(&jc); await_counts(1, 2, 0);
    assert(!(a->signals & EPD_COMPUTE_YIELD));
    assert(epd_compute_set_purpose(a, EPD_COMPUTE_BACKGROUND) == 0);
    assert(a->signals & EPD_COMPUTE_YIELD);
    assert(epd_compute_set_purpose(c, EPD_COMPUTE_BACKGROUND) == 0);
    assert(!(a->signals & EPD_COMPUTE_YIELD));
    assert(epd_compute_set_purpose(c, EPD_COMPUTE_CONTINUOUS) == 0);
    permit(); join(&ja, EPD_COMPUTE_MORE); await_counts(2, 1, 0);
    assert(log_session[1] == c->wire.session);
    permit(); join(&jc, EPD_COMPUTE_MORE); await_counts(3, 0, 0);
    permit(); join(&jb, EPD_COMPUTE_MORE);
    gate_off(); close_session(a); close_session(b); close_session(c);
}

static void queued_cancel(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_session_t b = open_as(0, EPD_COMPUTE_CONTINUOUS);
    gate_on(); job_t ja = step_job(a), jb = step_job(b);
    start(&ja); await_counts(1, 0, 0); start(&jb); await_counts(1, 1, 0);
    epd_compute_cancel(b); epd_compute_cancel(b);
    join(&jb, EPD_COMPUTE_CANCELLED);
    assert(calls == 1 && !(a->signals & EPD_COMPUTE_YIELD));
    assert(epd_compute_step(b, &empty_request) == EPD_COMPUTE_CANCELLED);
    permit(); join(&ja, EPD_COMPUTE_MORE);
    gate_off(); close_session(b); close_session(a);
}

static void running_cancel(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_FOREGROUND);
    gate_on(); job_t ja = step_job(a);
    epd_compute_region_t region = {0x1000, 32, 16, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE};
    ja.request.regions = &region; ja.request.region_count = 1;
    start(&ja); await_counts(1, 0, 0);
    assert(epd_compute_step(a, &empty_request) == EPD_COMPUTE_BUSY);
    assert(epd_compute_reset(a, &empty_request) == EPD_COMPUTE_BUSY);
    assert(epd_compute_close(a) == EPD_COMPUTE_BUSY);
    epd_compute_cancel(a);
    assert(WaitForSingleObject(ja.thread, 0) == WAIT_TIMEOUT);
    assert(remote_sessions == 1 && allocations == 2);
    permit(); join(&ja, EPD_COMPUTE_CANCELLED);
    assert(cleans == 1 && invalidates == 2);
    gate_off(); close_session(a);
}

static void exclusive_admission(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_CONTINUOUS);
    job_t exclusive = open_job(1), shared = open_job(0);
    start(&exclusive); await_counts(1, 0, 1);
    start(&shared); await_counts(1, 0, 2);
    epd_compute_capability_t c = {0}; epd_compute_requirements_t req = {EPD_COMPUTE_BACKGROUND, 1};
    epd_compute_session_t result;
    assert(epd_compute_open(&c, &req, &empty_request, &result) == EPD_COMPUTE_BUSY && !result);
    close_session(a); join(&exclusive, EPD_COMPUTE_OK);
    assert(WaitForSingleObject(shared.thread, 0) == WAIT_TIMEOUT);
    assert(epd_compute_set_purpose(exclusive.session, EPD_COMPUTE_BACKGROUND) == 0);
    assert(epd_compute_open(&c, &req, &empty_request, &result) == EPD_COMPUTE_BUSY);
    close_session(exclusive.session); join(&shared, EPD_COMPUTE_OK);
    close_session(shared.session); finish_wait(&exclusive); finish_wait(&shared);
}

static void nested_optional_admission(void)
{
    epd_compute_session_t document = open_as(0, EPD_COMPUTE_FOREGROUND);
    job_t exclusive = open_job(1);
    start(&exclusive); await_counts(1, 0, 1);
    epd_compute_capability_t data = {0};
    epd_compute_requirements_t req = {EPD_COMPUTE_BACKGROUND, 1};
    epd_compute_session_t nested = NULL;
    assert(epd_compute_open(&data, &req, &empty_request, &nested) == EPD_COMPUTE_BUSY && !nested);
    /* The document provider can return without waiting behind a request that
     * needs its own session to close. Existing document work still advances. */
    assert(epd_compute_step(document, &empty_request) == EPD_COMPUTE_MORE);
    close_session(document); join(&exclusive, EPD_COMPUTE_OK);
    close_session(exclusive.session); finish_wait(&exclusive);
}

static void admission_cancel(void)
{
    epd_compute_session_t a = open_as(1, EPD_COMPUTE_BACKGROUND);
    job_t j = open_job(0); start(&j); await_counts(1, 0, 1);
    assert(epd_compute_wait_destroy(j.wait) == EPD_COMPUTE_BUSY);
    job_t duplicate = open_job(0); finish_wait(&duplicate); duplicate.wait = j.wait;
    start(&duplicate); join(&duplicate, EPD_COMPUTE_BUSY);
    epd_compute_wait_cancel(j.wait); epd_compute_wait_cancel(j.wait);
    join(&j, EPD_COMPUTE_CANCELLED); assert(!j.session && remote_sessions == 1);
    start(&j); join(&j, EPD_COMPUTE_CANCELLED);
    finish_wait(&j); close_session(a);
}

static void cancelled_head_unblocks(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_BACKGROUND);
    job_t first = open_job(1), second = open_job(0);
    start(&first); await_counts(1, 0, 1); start(&second); await_counts(1, 0, 2);
    epd_compute_wait_cancel(first.wait); join(&first, EPD_COMPUTE_CANCELLED);
    join(&second, EPD_COMPUTE_OK);
    assert(remote_sessions == 2);
    close_session(second.session); close_session(a); finish_wait(&first); finish_wait(&second);
}

static void open_cancel_cleanup(void)
{
    job_t j = open_job(0); gate_on(); start(&j); await_counts(1, 0, 0);
    epd_compute_wait_cancel(j.wait);
    assert(WaitForSingleObject(j.thread, 0) == WAIT_TIMEOUT);
    permit(); await_counts(2, 0, 0);
    assert(log_operation[1] == EPD_COMPUTE_CLOSE);
    assert(epd_compute_wait_destroy(j.wait) == EPD_COMPUTE_BUSY);
    permit(); join(&j, EPD_COMPUTE_CANCELLED);
    assert(!j.session && remote_sessions == 0 && opens == 1 && closes == 1);
    gate_off(); finish_wait(&j);
}

static void open_cleanup_failure(unsigned failure)
{
    job_t opening = {0}; opening.capability = 1; opening.operation = EPD_COMPUTE_OPEN;
    opening.purpose = EPD_COMPUTE_BACKGROUND;
    if (!failure) assert(epd_compute_wait_create(&opening.wait) == EPD_COMPUTE_OK);
    fail_open_handle = failure;
    gate_on(); start(&opening); await_counts(1, 0, 0);
    if (opening.wait) epd_compute_wait_cancel(opening.wait);
    permit(); await_counts(2, 0, 0);
    assert(remote_sessions == 1 && log_operation[1] == EPD_COMPUTE_CLOSE);
    if (opening.wait) assert(epd_compute_wait_destroy(opening.wait) == EPD_COMPUTE_BUSY);
    fail_transport = true; permit();
    join(&opening, !failure ? EPD_COMPUTE_CANCELLED : failure == 1 ?
         EPD_COMPUTE_COMPONENT_ERROR : EPD_COMPUTE_UNAVAILABLE);
    gate_off(); fail_open_handle = 0;
    if (opening.wait) finish_wait(&opening);
    epd_compute_session_t retained = opening.session;
    assert(retained && retained == active && remote_sessions == 1 && !closes);
    assert(allocations == 2 && semaphores == 2 && calls == 2);
    assert(epd_compute_step(retained, &empty_request) == EPD_COMPUTE_CANCELLED);
    assert(epd_compute_reset(retained, &empty_request) == EPD_COMPUTE_CANCELLED);
    assert(calls == 2); /* No implicit retry or work on a failed OPEN. */
    job_t waiting = open_job(0); start(&waiting); await_counts(2, 0, 1);
    for (unsigned i = 0; i < 3; ++i) {
        fail_transport = true;
        assert(epd_compute_close(retained) == EPD_COMPUTE_UNAVAILABLE);
        assert(active == retained && remote_sessions == 1 && calls == 3 + i && !closes);
        assert(WaitForSingleObject(waiting.thread, 0) == WAIT_TIMEOUT);
    }
    close_session(retained); join(&waiting, EPD_COMPUTE_OK);
    assert(closes == 1 && remote_sessions == 1 && opens == 2);
    close_session(waiting.session); finish_wait(&waiting);
    assert(closes == 2 && allocations == 1 && semaphores == 1);
}
static void open_cancel_close_failure(void) { open_cleanup_failure(0); }
static void open_error_close_failure(void) { open_cleanup_failure(1); }
static void open_transport_close_failure(void) { open_cleanup_failure(2); }

static epd_compute_buffer_t *owned_buffers(void)
{
    epd_compute_buffer_t *b = epd_app_alloc(4 * sizeof(*b), EPD_APP_PSRAM);
    assert(b); memset(b, 0, 4 * sizeof(*b));
    for (unsigned i = 0; i < 4; ++i) {
        assert(epd_compute_buffer_alloc(b + i, 64)); memset(b[i].data, 0xa5, 64);
    }
    return b;
}
static void resident_cleanup(void)
{
    epd_compute_session_t s = open_as(1, EPD_COMPUTE_BACKGROUND);
    epd_compute_buffer_t *b = owned_buffers();
    fail_transport = true; assert(epd_compute_close(s) == EPD_COMPUTE_UNAVAILABLE);
    unsigned before = calls;
    epd_compute_close_later(s, b, 4);
    assert(calls == before && allocations == 7 && s->owned_buffers == b);
    epd_compute_capability_t capability = {0};
    epd_compute_requirements_t requirements = {EPD_COMPUTE_BACKGROUND, 1};
    epd_compute_session_t denied = NULL;
    fail_transport = true;
    assert(epd_compute_open(&capability, &requirements, &empty_request, &denied) == EPD_COMPUTE_BUSY);
    assert(!denied && calls == before + 1 && allocations == 7 && !running && !queue_head);
    before = calls;
    for (unsigned i = 0; i < 3; ++i) {
        fail_transport = true;
        assert(epd_compute_retry_cleanup() == EPD_COMPUTE_UNAVAILABLE);
        assert(calls == before + i + 1 && remote_sessions == 1 && allocations == 7);
        for (unsigned j = 0; j < 4; ++j) assert(((unsigned char *)b[j].data)[63] == 0xa5);
    }
    /* A subsequent OPEN performs one cleanup attempt before admission. */
    epd_compute_session_t next = open_as(0, EPD_COMPUTE_BACKGROUND);
    assert(closes == 1 && allocations == 2 && remote_sessions == 1);
    close_session(next);
    before = calls;
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_OK && calls == before);
}
static void resident_cleanup_rotation(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_session_t b = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_close_later(a, owned_buffers(), 4);
    epd_compute_close_later(b, owned_buffers(), 4);
    fail_transport = true;
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_UNAVAILABLE);
    assert(active == a && a->active_next == b);
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_OK && active == b && allocations == 7);
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_OK && !active && allocations == 1);
}
static DWORD WINAPI cleanup_thread(LPVOID p)
{ *(int *)p = epd_compute_retry_cleanup(); return 0; }
static void resident_cleanup_concurrent(void)
{
    epd_compute_session_t s = open_as(1, EPD_COMPUTE_BACKGROUND);
    epd_compute_close_later(s, owned_buffers(), 4);
    int result = 999;
    gate_on();
    HANDLE thread = CreateThread(NULL, 0, cleanup_thread, &result, 0, NULL); assert(thread);
    await_counts(1, 0, 0);
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_OK); /* Already claimed. */
    assert(calls == 1 && allocations == 7 && remote_sessions == 1);
    permit(); assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread)); gate_off();
    assert(result == EPD_COMPUTE_OK && calls == 1 && allocations == 1 && !active);
}
static void resident_cleanup_bounded(void)
{
    epd_compute_session_t retired = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_close_later(retired, owned_buffers(), 4);
    epd_compute_capability_t capability = {0};
    epd_compute_requirements_t requirements = {EPD_COMPUTE_BACKGROUND, 1};
    unsigned before = calls;
    fail_closes = true;
    for (unsigned i = 0; i < 32; ++i) {
        epd_compute_session_t denied = NULL;
        assert(epd_compute_open(&capability, &requirements, &empty_request, &denied) == EPD_COMPUTE_BUSY);
        assert(!denied && remote_sessions == 1 && allocations == 7 && opens == 1);
        assert(calls == before + i + 1); /* Exactly one attempt per new event. */
    }
    job_t blocked = open_job(0); unsigned expected = calls + 1;
    start(&blocked); await_counts(expected, 0, 1);
    job_t unrelated = open_job(2); start(&unrelated); join(&unrelated, EPD_COMPUTE_OK);
    assert(remote_sessions == 2 && opens == 2 && WaitForSingleObject(blocked.thread, 0) == WAIT_TIMEOUT);
    assert(epd_compute_step(unrelated.session, &empty_request) == EPD_COMPUTE_MORE);
    fail_closes = false;
    close_session(unrelated.session); finish_wait(&unrelated);
    assert(epd_compute_retry_cleanup() == EPD_COMPUTE_OK);
    join(&blocked, EPD_COMPUTE_OK); close_session(blocked.session); finish_wait(&blocked);
    assert(allocations == 1 && !remote_sessions && opens == closes);
}

static void queued_open_cancel(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_CONTINUOUS);
    job_t step = step_job(a), opening = open_job(0);
    gate_on(); start(&step); await_counts(1, 0, 0);
    start(&opening); await_counts(1, 1, 0);
    epd_compute_wait_cancel(opening.wait); join(&opening, EPD_COMPUTE_CANCELLED);
    assert(!opening.session && opens == 1 && remote_sessions == 1 && calls == 1);
    /* Cancellation leaves at most one coalesced dispatcher notification. */
    assert(WaitForSingleObject(pending.handle, 0) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(pending.handle, 0) == WAIT_TIMEOUT);
    rt_sem_release(&pending);
    permit(); join(&step, EPD_COMPUTE_MORE);
    gate_off(); close_session(a); finish_wait(&opening);
}

static void close_failure_isolation(void)
{
    epd_compute_session_t a = open_as(1, EPD_COMPUTE_BACKGROUND);
    job_t opening = open_job(0); start(&opening); await_counts(1, 0, 1);
    fail_transport = true;
    assert(epd_compute_close(a) == EPD_COMPUTE_UNAVAILABLE);
    assert(remote_sessions == 1 && active == a);
    assert(WaitForSingleObject(opening.thread, 0) == WAIT_TIMEOUT);
    close_session(a); join(&opening, EPD_COMPUTE_OK);
    close_session(opening.session); finish_wait(&opening);
}

static bool cancel_callback(void *context) { ++*(unsigned *)context; return true; }
static void validation_failure(void)
{
    epd_compute_capability_t c = {0}; epd_compute_session_t s;
    epd_compute_requirements_t req = {EPD_COMPUTE_BACKGROUND, 1};
    c.index = 3;
    assert(epd_compute_open(&c, &req, &empty_request, &s) == EPD_COMPUTE_UNAVAILABLE);
    c.index = 0; fail_allocation = 1;
    assert(epd_compute_open(&c, &req, &empty_request, &s) == EPD_COMPUTE_NO_MEMORY);
    unsigned checks = 0; epd_compute_request_t request = {0};
    request.cancelled = cancel_callback; request.context = &checks;
    assert(epd_compute_open(&c, &req, &request, &s) == EPD_COMPUTE_CANCELLED && checks == 1);
    fail_open = true;
    assert(epd_compute_open(&c, &req, &empty_request, &s) == EPD_COMPUTE_COMPONENT_ERROR);
    assert(!s && !active && allocations == 1); fail_open = false;
    fail_transport = true;
    assert(epd_compute_open(&c, &req, &empty_request, &s) == EPD_COMPUTE_UNAVAILABLE);
    assert(!s && !active && allocations == 1);
    s = open_as(2, EPD_COMPUTE_BACKGROUND);
    assert(epd_compute_set_purpose(s, EPD_COMPUTE_CONTINUOUS) == EPD_COMPUTE_UNSUPPORTED);
    assert(epd_compute_set_purpose(s, (epd_compute_purpose_t)-1) == EPD_COMPUTE_INVALID);
    assert(epd_compute_set_purpose(NULL, EPD_COMPUTE_BACKGROUND) == EPD_COMPUTE_INVALID);
    request.region_count = 1;
    assert(epd_compute_step(s, &request) == EPD_COMPUTE_INVALID);
    fail_transport = true;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    close_session(s);
}

static void wait_reuse(void)
{
    job_t j = open_job(0);
    for (unsigned i = 0; i < 20; ++i)
    {
        epd_compute_session_t a = open_as(1, EPD_COMPUTE_BACKGROUND);
        start(&j); await_counts(calls, 0, 1);
        close_session(a); join(&j, EPD_COMPUTE_OK); close_session(j.session);
    }
    finish_wait(&j);
}

static void diagnostic_timing(void)
{
    manual_clock = true; manual_ticks = -3; transport_ticks = 9; maintenance_ticks = 50;
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    for (unsigned i = 0; i < 100; ++i) assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(!diagnostic_lines && s->diagnostics.rtt_ticks == 909 && s->diagnostics.rtt_max == 9);
    epd_compute_region_t region = {0x20000000, 32, 32, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE};
    epd_compute_request_t request = {0}; request.regions = &region; request.region_count = 1;
    assert(epd_compute_step(s, &request) == EPD_COMPUTE_MORE);
    assert(s->diagnostics.rtt_ticks == 918 && !s->diagnostics.queue_ticks);
    fail_transport = true; assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_UNAVAILABLE);
    assert(diagnostic_lines == 3 && s->diagnostics.errors == 1);
    for (unsigned i = 0; i < 20; ++i)
    { fail_transport = true; assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_UNAVAILABLE); }
    assert(diagnostic_lines == 3 && s->diagnostics.errors == 21);
    s->diagnostics.rtt_ticks = UINT32_MAX - 1; s->diagnostics.rtt_calls = UINT32_MAX;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(s->diagnostics.saturated && s->diagnostics.rtt_ticks == UINT32_MAX && s->diagnostics.rtt_calls == UINT32_MAX);
    close_session(s); assert(diagnostic_lines == 6);
    s = open_as(0, EPD_COMPUTE_BACKGROUND);
    LONG before_close = diagnostic_lines;
    close_session(s); assert(diagnostic_lines == before_close + (EPD_COMPUTE_TRACE ? 3 : 0));
}

static void diagnostic_queue_cancel(void)
{
    manual_clock = true;
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_BACKGROUND), b = open_as(0, EPD_COMPUTE_BACKGROUND);
    gate_on(); job_t ja = step_job(a), jb = step_job(b);
    start(&ja); await_counts(1, 0, 0); start(&jb); await_counts(1, 1, 0);
    InterlockedExchangeAdd(&manual_ticks, 17);
    epd_compute_cancel(b); join(&jb, EPD_COMPUTE_CANCELLED);
    assert(b->diagnostics.queue_ticks == 17 && b->diagnostics.queue_max == 17);
    assert(b->diagnostics.queue_calls == 2 && b->diagnostics.rtt_calls == 1);
    permit(); join(&ja, EPD_COMPUTE_MORE); gate_off();
    assert(a->diagnostics.rtt_ticks == 17 && !a->diagnostics.queue_ticks);
    close_session(a); close_session(b);
}

static void diagnostic_wire(void)
{
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    memset(&s->wire.diagnostics, 0x5a, sizeof(s->wire.diagnostics));
    transfer.diagnostics.attempts = 0x12345678u; inspect_prefix = true;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(!diagnostic_lines && !s->wire.diagnostics.flags);
    assert(s->wire.diagnostics.attempts == 0x5a5a5a5au); inspect_prefix = false;
    inject_summary = true;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_NO_MEMORY);
    assert(remote_summaries == 1 && last_remote_status == EPD_COMPUTE_NO_MEMORY);
    assert(s->wire.diagnostics.ticks[1] == 321 && s->wire.diagnostics.heap_peak == 60000);
    assert(!s->wire.diagnostics.flags);
    LONG lines = diagnostic_lines;
    for (unsigned i = 0; i < 100; ++i) assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(diagnostic_lines == lines && remote_summaries == 1);
    fail_transport = true; inject_summary = true;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_UNAVAILABLE);
    assert(remote_summaries == 1); inject_summary = false;
    inject_summary = true; close_session(s);
    assert(remote_summaries == (EPD_COMPUTE_TRACE ? 2 : 1));
    if (EPD_COMPUTE_TRACE) assert(last_remote_status == 0);
    fail_open = true; inject_summary = true;
    epd_compute_capability_t capability = {0};
    epd_compute_requirements_t requirements = {EPD_COMPUTE_BACKGROUND, 1};
    assert(epd_compute_open(&capability, &requirements, &empty_request, &s) == EPD_COMPUTE_COMPONENT_ERROR);
    assert(!s && !active && remote_summaries == (EPD_COMPUTE_TRACE ? 3 : 2) &&
           last_remote_status == EPD_COMPUTE_COMPONENT_ERROR);
    fail_open = false;
}

static void diagnostic_success_and_errors(void)
{
    manual_clock = true; transport_ticks = 7;
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    for (unsigned i = 0; i < 16; ++i)
        assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(!diagnostic_lines && s->diagnostics.rtt_calls == 17 && s->diagnostics.rtt_ticks == 119);
    epd_compute_diag_wire_t *d = &s->wire.diagnostics;
    d->flags = EPD_COMPUTE_DIAG_VALID | EPD_COMPUTE_DIAG_CLOSE;
    d->status = EPD_COMPUTE_OK; d->calls[1] = 16; d->ticks[1] = 112;
    diagnostic_remote_result(s);
    assert(!d->flags && d->calls[1] == 16 && d->ticks[1] == 112);
    assert(EPD_COMPUTE_TRACE ? diagnostic_lines > 0 : diagnostic_lines == 0);
    LONG before = diagnostic_lines;
    d->flags = EPD_COMPUTE_DIAG_VALID | EPD_COMPUTE_DIAG_CLOSE;
    d->errors = 1;
    diagnostic_remote_result(s);
    assert(!d->flags && diagnostic_lines > before);
    before = diagnostic_lines;
    close_session(s);
    assert(diagnostic_lines == before + (EPD_COMPUTE_TRACE ? 3 : 0));
}

static void diagnostic_admission(void)
{
    manual_clock = true;
    epd_compute_session_t a = open_as(1, EPD_COMPUTE_BACKGROUND);
    job_t opening = open_job(0); start(&opening); await_counts(calls, 0, 1);
    InterlockedExchangeAdd(&manual_ticks, 23); close_session(a); join(&opening, EPD_COMPUTE_OK);
    assert(opening.session->diagnostics.admission_ticks == 23);
    assert(!opening.session->diagnostics.queue_ticks && !opening.session->diagnostics.rtt_ticks);
    close_session(opening.session); finish_wait(&opening);
}

typedef struct { const char *name; void (*run)(void); } test_t;
static void invalid_wait_context(void)
{
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_FOREGROUND);
    epd_compute_wait_t w;
    assert(epd_compute_wait_create(&w) == EPD_COMPUTE_OK);
    epd_compute_capability_t cap = {0};
    epd_compute_requirements_t req = {EPD_COMPUTE_BACKGROUND, 1};
    unsigned before_calls = calls, sequence = s->wire.sequence;
    LONG before_allocations = allocations;
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        rt_base_t level = 0;
        if (mode == 0) level = rt_hw_interrupt_disable();
        if (mode == 1) critical_depth = 1;
        if (mode == 2) interrupt_depth = 1;
        if (mode == 3) scheduler_not_started = true;
        if (mode == 4) thread_self = &dispatcher;
        epd_compute_session_t opened = s;
        assert(epd_compute_open(&cap, &req, &empty_request, &opened) == EPD_COMPUTE_INVALID && !opened);
        assert(epd_compute_open_wait(&cap, &req, &empty_request, w, &opened) == EPD_COMPUTE_INVALID && !opened);
        assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_INVALID);
        assert(epd_compute_reset(s, &empty_request) == EPD_COMPUTE_INVALID);
        assert(epd_compute_close(s) == EPD_COMPUTE_INVALID);
        assert(epd_compute_retry_cleanup() == EPD_COMPUTE_INVALID);
        assert(calls == before_calls && allocations == before_allocations && !queue_head && !admission_head);
        assert(s->wire.sequence == sequence && s->state == SESSION_IDLE);
        if (mode == 0) rt_hw_interrupt_enable(level);
        critical_depth = interrupt_depth = 0; scheduler_not_started = false; thread_self = NULL;
    }
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    assert(epd_compute_wait_destroy(w) == EPD_COMPUTE_OK);
    close_session(s);
}

static void await_parked(unsigned expected)
{
    EnterCriticalSection(&irq);
    while (pending_notified || running || queued_count() != expected)
        assert(SleepConditionVariableCS(&changed, &irq, 5000));
    assert(guard.state == SLEEP_GUARD_SLEEPING && !guard.held_count);
    LeaveCriticalSection(&irq);
}

static void sleep_busy_batch(void)
{
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    /* A retained but idle session does not block sleep. */
    assert(sleep_guard_prepare());
    gate_on(); job_t job = step_job(s); start(&job); await_counts(1, 0, 0);
    assert(!sleep_guard_commit() && sleep_guard_prepare());
    sleep_guard_snapshot_t snapshot;
    sleep_guard_snapshot(&snapshot);
    assert(snapshot.state == SLEEP_GUARD_PREPARING && snapshot.held_count == 1 && snapshot.draining_count == 1);
    permit(); join(&job, EPD_COMPUTE_MORE); gate_off();
    assert(sleep_guard_prepare() && sleep_guard_commit());
    sleep_guard_cancel(); epd_compute_resume_dispatch();
    close_session(s);
}

static void sleep_drain_activity(void)
{
    sleep_guard_owner_t drain = {.id = "batch"}, activity = {.id = "user"};
    sleep_guard_snapshot_t before, during;
    sleep_guard_snapshot(&before);
    assert(sleep_guard_acquire_drain(&drain));
    sleep_guard_snapshot(&during);
    assert(during.revision == before.revision && during.held_count == 1 && during.draining_count == 1);
    assert(sleep_guard_prepare() && !sleep_guard_commit());
    assert(sleep_guard_acquire(&activity));
    sleep_guard_snapshot(&during);
    assert(during.state == SLEEP_GUARD_AWAKE && during.held_count == 2 && during.draining_count == 1);
    assert(!sleep_guard_prepare());
    sleep_guard_release(&activity);
    assert(sleep_guard_prepare());
    /* Promoting an existing drain hold to user activity cancels preparation. */
    assert(sleep_guard_acquire(&drain));
    sleep_guard_snapshot(&during);
    assert(during.state == SLEEP_GUARD_AWAKE && !during.draining_count);
    sleep_guard_release(&drain);
    assert(sleep_guard_prepare());
    sleep_guard_snapshot(&before);
    assert(sleep_guard_acquire_drain(&drain) && sleep_guard_acquire_drain(&drain));
    sleep_guard_snapshot(&during);
    assert(during.revision == before.revision && during.state == SLEEP_GUARD_PREPARING);
    assert(during.held_count == 1 && during.draining_count == 1 && !sleep_guard_commit());
    sleep_guard_release(&drain); sleep_guard_release(&drain);
    assert(sleep_guard_commit() && !sleep_guard_acquire_drain(&drain));
    sleep_guard_snapshot(&during);
    assert(!during.held_count && !during.draining_count);
    sleep_guard_cancel(); epd_compute_resume_dispatch();
}

static void sleep_resume_queue(void)
{
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    unsigned id = s->wire.session;
    for (unsigned i = 0; i < 40; ++i)
    {
        assert(sleep_guard_prepare() && sleep_guard_commit());
        unsigned before_calls = calls;
        job_t job = step_job(s); start(&job); await_parked(1);
        assert(calls == before_calls && WaitForSingleObject(job.thread, 0) == WAIT_TIMEOUT);
        /* An early notification cannot dispatch while sleep is committed. */
        epd_compute_resume_dispatch(); await_parked(1); assert(calls == before_calls);
        sleep_guard_cancel(); epd_compute_resume_dispatch();
        join(&job, EPD_COMPUTE_MORE);
        assert(calls == before_calls + 1 && s->wire.session == id);
    }
    assert(opens == 1); close_session(s);
}

static void sleep_cancel_queue(void)
{
    epd_compute_session_t a = open_as(0, EPD_COMPUTE_BACKGROUND);
    epd_compute_session_t b = open_as(0, EPD_COMPUTE_FOREGROUND);
    assert(sleep_guard_prepare() && sleep_guard_commit());
    unsigned before_calls = calls;
    job_t ja = step_job(a), jb = step_job(b);
    start(&ja); start(&jb); await_parked(2);
    epd_compute_cancel(a); join(&ja, EPD_COMPUTE_CANCELLED);
    assert(calls == before_calls);
    sleep_guard_cancel(); epd_compute_resume_dispatch();
    join(&jb, EPD_COMPUTE_MORE);
    assert(calls == before_calls + 1);
    close_session(a); close_session(b);
}

static void sleep_cancel_open(void)
{
    assert(sleep_guard_prepare() && sleep_guard_commit());
    job_t job = open_job(0); start(&job); await_parked(1);
    epd_compute_wait_cancel(job.wait); join(&job, EPD_COMPUTE_CANCELLED);
    assert(!job.session && !calls && !active && !queue_head);
    sleep_guard_cancel(); epd_compute_resume_dispatch(); finish_wait(&job);
    close_session(open_as(0, EPD_COMPUTE_BACKGROUND));
}

static void sleep_failed_transport(void)
{
    epd_compute_session_t s = open_as(0, EPD_COMPUTE_BACKGROUND);
    fail_transport = true;
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_UNAVAILABLE);
    assert(sleep_guard_prepare() && sleep_guard_commit());
    sleep_guard_cancel(); epd_compute_resume_dispatch();
    assert(epd_compute_step(s, &empty_request) == EPD_COMPUTE_MORE);
    close_session(s);
}

static void check_init_failures(void)
{
    for (unsigned failure = 1; failure <= 7; ++failure)
    {
        init_failure = failure;
        init_detaches = transport_detaches = 0;
#ifdef COMPUTE_MEMORY_TEST
        epd_memory_owner_t original = epd_memory_owner_current();
#endif
        assert(compute_init() == (failure == 7 ? -RT_ERROR : -RT_ENOMEM));
        assert(!ready && !allocations && !semaphores && !resident_stack);
        assert(init_detaches == (unsigned)(failure >= 6));
        assert(transport_detaches == (unsigned)(failure == 7));
#ifdef COMPUTE_MEMORY_TEST
        assert(epd_memory_owner_current() == original);
#endif
        test_sram_start = 1; test_sram_size = UINTPTR_MAX - 1;
    }
    /* The last byte may exactly match the SRAM boundary. */
    init_failure = 8;
}
static void dispatcher_stack_retained(void)
{
    assert(ready && resident_stack && allocations == 1 && semaphores == 1);
    assert(test_sram_start == (uintptr_t)resident_stack && test_sram_size == DISPATCHER_STACK_SIZE);
}

static const test_t tests[] = {
    {"init_failures", dispatcher_stack_retained},
    {"invalid_wait_context", invalid_wait_context}, {"sleep_busy_batch", sleep_busy_batch},
    {"sleep_drain_activity", sleep_drain_activity},
    {"sleep_resume_queue", sleep_resume_queue}, {"sleep_cancel_queue", sleep_cancel_queue},
    {"sleep_cancel_open", sleep_cancel_open}, {"sleep_failed_transport", sleep_failed_transport},
    {"shared_persistence", shared_persistence}, {"priorities_fifo", priorities_fifo},
    {"bounded_background", bounded_background}, {"change_purpose", change_purpose},
    {"queued_cancel", queued_cancel}, {"running_cancel", running_cancel},
    {"exclusive_admission", exclusive_admission}, {"admission_cancel", admission_cancel},
    {"nested_optional_admission", nested_optional_admission},
    {"cancelled_head_unblocks", cancelled_head_unblocks}, {"open_cancel_cleanup", open_cancel_cleanup},
    {"open_cancel_close_failure", open_cancel_close_failure},
    {"open_error_close_failure", open_error_close_failure},
    {"open_transport_close_failure", open_transport_close_failure},
    {"resident_cleanup", resident_cleanup}, {"resident_cleanup_rotation", resident_cleanup_rotation},
    {"resident_cleanup_concurrent", resident_cleanup_concurrent},
    {"resident_cleanup_bounded", resident_cleanup_bounded},
    {"queued_open_cancel", queued_open_cancel}, {"close_failure_isolation", close_failure_isolation},
    {"validation_failure", validation_failure}, {"wait_reuse", wait_reuse},
    {"diagnostic_timing", diagnostic_timing}, {"diagnostic_queue_cancel", diagnostic_queue_cancel},
    {"diagnostic_admission", diagnostic_admission}, {"diagnostic_wire", diagnostic_wire},
    {"diagnostic_success_and_errors", diagnostic_success_and_errors}
};
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    assert(argc == 2);
    if (!strcmp(argv[1], "--list"))
    {
        for (unsigned i = 0; i < sizeof(tests) / sizeof(*tests); ++i) puts(tests[i].name);
        return 0;
    }
    InitializeCriticalSection(&irq); InitializeConditionVariable(&changed);
    transport_gate = CreateSemaphoreA(NULL, 0, 65535, NULL); assert(transport_gate);
    if (!strcmp(argv[1], "init_failures")) check_init_failures();
    assert(compute_init() == RT_EOK);
    for (unsigned i = 0; i < sizeof(tests) / sizeof(*tests); ++i)
        if (!strcmp(argv[1], tests[i].name))
        {
            tests[i].run();
            assert(!active && !queue_head && !queue_tail && !running);
            assert(!admission_head && !admission_tail && !remote_sessions && allocations == 1);
            assert(!guard.held_count);
            assert(semaphores == 1); /* Only the process-lifetime dispatcher. */
            printf("PASS %s\n", tests[i].name); return 0;
        }
    return 1;
}
