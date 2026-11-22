/* SPDX-License-Identifier: Apache-2.0 */
#ifndef REAL_FONT_PIPE
static remote_font_t *source_inflight_font;
static unsigned source_inflight_mode;

static void source_fixture(remote_font_t *font, uint32_t id)
{
    memset(font, 0, sizeof(*font));
    assert(reading_font_acquire(id, &font->lease));
    font->id = id; font->references = 1; font->leased = true;
    font->font.get_glyph_dsc = descriptor_cb;
    lock(); font->primary = acquire_source(id, &font->lease); unlock();
    assert(font->primary);
}

static void source_fixture_release(remote_font_t *font)
{
    release_source(font->primary);
    reading_font_release(&font->lease);
    /* These supply tests drive the worker callback directly, without starting
     * the asynchronous glyph worker. Retire any queued invalidation here. */
    lock(); work_head = work_tail = NULL; notified = false; unlock();
}

static crf_status source_request_hint(remote_font_t *font, uint32_t offset, uint32_t length, bool metadata)
{
    assert(length <= EPD_COMPUTE_PIPE_BYTES);
    uint8_t bytes[EPD_COMPUTE_PIPE_BYTES + 1]; memset(bytes, 0xa5, sizeof(bytes));
    batch_t batch = {0}; batch.font = font; batch.face = font->primary;
    epd_compute_pipe_message_t message = {0};
    message.values[0] = EPD_FONT_VERSION; message.handle = batch.face->handle;
    message.values[7] = metadata ? EPD_FONT_READ_METADATA : 0;
    memcpy(&message.values[1], &batch.face->identity, sizeof(batch.face->identity));
    message.offset = offset; message.length = length; message.size = fixture_size;
    int result = supply_read(&batch, &message, bytes);
    assert(!io_depth && bytes[length] == 0xa5);
    if (result == EPD_COMPUTE_OK) {
        assert(message.length == length && message.values[6] == CRF_OK);
        for (unsigned i = 0; i < length; ++i) {
            uint32_t at = offset + i;
            assert(bytes[i] == (uint8_t)(at ^ (at >> 9) ^ (at >> 17)));
        }
    } else assert(result == EPD_COMPUTE_COMPONENT_ERROR && !message.length);
    return (crf_status)message.values[6];
}
static crf_status source_request(remote_font_t *font, uint32_t offset, uint32_t length)
{ return source_request_hint(font, offset, length, false); }

typedef void (*source_case_fn)(void);
static DWORD WINAPI source_case_entry(LPVOID argument)
{
    (*(source_case_fn *)argument)();
    assert(!io_depth && !open_fds);
    return 0;
}
static void source_case_run(source_case_fn fn)
{
    assert(!initialized);
    assert(rt_mutex_init(&cache_lock, "source", 0) == 0);
    assert(rt_sem_init(&work_event, "source", 0, 0) == 0);
    initialized = true; pattern_reads = true; fixture_size = 1024u * 1024u + 333u;
    HANDLE thread = CreateThread(NULL, 0, source_case_entry, &fn, 0, NULL); assert(thread);
    if (source_inflight_mode) {
        assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
        lock(); assert(source_inflight_font->primary->blocks->busy); unlock();
        size_t bytes = source_cache_totals.bytes;
        assert(bytes == SOURCE_BLOCK_CHARGE);
        assert(!provider_reclaim_target(SIZE_MAX));
        assert(source_cache_totals.bytes == bytes);
        if (source_inflight_mode == 1) {
            ++fixture_session;
            lock(); source_cache_clear(source_inflight_font->primary); unlock();
        } else ui_reader_font_remote_cancel(&source_inflight_font->font);
        SetEvent(io_continue);
    }
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(!sources && !source_cache_totals.bytes && !allocations && !leases);
    assert(!work_head && !work_tail);
}

static void source_reads_worker(void)
{
    remote_font_t a, b; source_fixture(&a, 1); source_fixture(&b, 1);
    assert(a.primary == b.primary);
    b.style.size_px = 40;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < 4; ++i)
            assert(source_request(pass ? &b : &a, i * 1024u, 1024) == CRF_OK);
    assert(raw_reads == 1 && physical_bytes == 4096 && storage_locks == 1);
    assert(source_opens == 1 && source_stats == 1 && source_seeks == 1 && source_closes == 0);
    assert(a.primary->cache.hits == 7 && a.primary->cache.misses == 1);
    /* Arbitrary cross-block requests retain the exact-read fallback. */
    assert(source_request(&b, 4090, 20) == CRF_OK);
    assert(raw_reads == 2 && physical_bytes == 4116 && b.primary->cache.bypasses == 1);
    assert(source_request(&a, fixture_size - 100, 100) == CRF_OK);
    assert(physical_bytes == 4449);
    assert(source_request(&b, fixture_size - 200, 200) == CRF_OK && raw_reads == 3);
    source_fixture_release(&a); assert(source_cache_totals.bytes);
    source_fixture_release(&b); assert(!source_cache_totals.bytes);
    puts("Source cache: 8 x 1KiB requests across two sizes -> 1 x 4KiB physical read; tail and cross-block bytes match");
}
static void source_cache_reads(void) { source_case_run(source_reads_worker); }

static void source_bounds_worker(void)
{
    remote_font_t a, b; source_fixture(&a, 1); source_fixture(&b, 2);
    assert(a.primary != b.primary);
    unsigned capacity = (unsigned)(SOURCE_CACHE_LIMIT / SOURCE_BLOCK_CHARGE);
    for (unsigned i = 0; i < capacity; ++i) {
        assert(source_request(&a, i * 4096u, 1024) == CRF_OK);
        assert(source_request(&b, i * 4096u, 1024) == CRF_OK);
    }
    assert(a.primary->cache.bytes == capacity * SOURCE_BLOCK_CHARGE);
    assert(source_cache_totals.bytes == 2u * capacity * SOURCE_BLOCK_CHARGE);
    assert(source_cache_totals.bytes <= SOURCE_CACHE_TOTAL_LIMIT);
    LONG before = raw_reads;
    assert(source_request(&a, 0, 1024) == CRF_OK && raw_reads == before);
    assert(source_request(&a, capacity * 4096u, 1024) == CRF_OK && raw_reads == before + 1);
    assert(source_request(&a, 0, 1024) == CRF_OK && raw_reads == before + 1);
    assert(source_request(&a, 4096, 1024) == CRF_OK && raw_reads == before + 2);
    for (unsigned i = capacity + 1; i < 200; ++i) {
        assert(source_request(&a, i * 4096u, 1024) == CRF_OK);
        assert(source_request(&b, i * 4096u, 1024) == CRF_OK);
        assert(a.primary->cache.bytes <= SOURCE_CACHE_LIMIT && b.primary->cache.bytes <= SOURCE_CACHE_LIMIT);
        assert(source_cache_totals.bytes <= SOURCE_CACHE_TOTAL_LIMIT);
    }
    printf("Source cache: two sources, %u blocks/source, peak charge=%zu <= %u bytes including allocation headroom\n",
        capacity, source_cache_totals.peak, SOURCE_CACHE_TOTAL_LIMIT);
    assert(close_source(a.primary) && !a.primary->blocks);
    source_fixture_release(&a); source_fixture_release(&b);
}
static void source_cache_bounds(void) { source_case_run(source_bounds_worker); }

static void source_metadata_worker(void)
{
    remote_font_t a; source_fixture(&a, 1);
    unsigned capacity = (unsigned)(SOURCE_CACHE_LIMIT / SOURCE_BLOCK_CHARGE);
    unsigned protected_limit = (unsigned)SOURCE_METADATA_MAX;
    for (unsigned i = 0; i < protected_limit; ++i)
        assert(source_request_hint(&a, i * 4096u, 4096, true) == CRF_OK);
    for (unsigned i = protected_limit; i < 180; ++i)
        assert(source_request(&a, i * 4096u, 4096) == CRF_OK);
    assert(a.primary->cache.bytes == capacity * SOURCE_BLOCK_CHARGE);
    LONG before = raw_reads;
    for (unsigned i = 0; i < protected_limit; ++i)
        assert(source_request_hint(&a, i * 4096u, 4096, true) == CRF_OK);
    assert(raw_reads == before);
    /* A new metadata block evicts the oldest metadata, not the stream MRU. */
    assert(source_request_hint(&a, 220u * 4096u, 4096, true) == CRF_OK);
    assert(source_request(&a, 179u * 4096u, 4096) == CRF_OK && raw_reads == before + 1);
    assert(source_request_hint(&a, 0, 4096, true) == CRF_OK && raw_reads == before + 2);
    source_block_t *metadata, *ordinary;
    assert(source_cache_classes(a.primary, &metadata, &ordinary) == protected_limit);
    /* A later hint promotes an existing ordinary block without another read,
     * demoting the oldest metadata so the protected limit remains bounded. */
    before = raw_reads;
    assert(source_request_hint(&a, 179u * 4096u, 4096, true) == CRF_OK && raw_reads == before);
    assert(source_cache_classes(a.primary, &metadata, &ordinary) == protected_limit);
    assert(provider_reclaim_target(SIZE_MAX) == capacity * SOURCE_BLOCK_CHARGE);
    assert(!a.primary->blocks);
    available_bytes = 1;
    assert(source_request_hint(&a, 0, 1024, true) == CRF_OK && !a.primary->blocks);
    available_bytes = 64u * 1024u * 1024u;
    source_fixture_release(&a);
}
static void source_cache_metadata(void) { source_case_run(source_metadata_worker); }

static void source_metadata_sources_worker(void)
{
    remote_font_t a, b, shared;
    source_fixture(&a, 1); source_fixture(&b, 2); source_fixture(&shared, 1);
    assert(a.primary == shared.primary && a.primary != b.primary);
    unsigned capacity = (unsigned)(SOURCE_CACHE_LIMIT / SOURCE_BLOCK_CHARGE);
    for (unsigned i = 0; i < capacity; ++i) {
        bool metadata = i < SOURCE_METADATA_MAX;
        assert(source_request_hint(&a, i * 4096u, 4096, metadata) == CRF_OK);
        assert(source_request_hint(&b, i * 4096u, 4096, metadata) == CRF_OK);
    }
    assert(source_cache_totals.bytes == 2u * capacity * SOURCE_BLOCK_CHARGE);
    assert(source_cache_totals.bytes <= SOURCE_CACHE_TOTAL_LIMIT);
    LONG before = raw_reads;
    for (unsigned i = 0; i < SOURCE_METADATA_MAX; ++i)
        assert(source_request_hint(&shared, i * 4096u, 4096, true) == CRF_OK);
    assert(raw_reads == before);
    /* Unsupported hint bits are rejected before touching storage or cache. */
    batch_t batch = {0}; batch.font = &a; batch.face = a.primary;
    epd_compute_pipe_message_t m = {0}; uint8_t data;
    m.values[0] = EPD_FONT_VERSION; m.values[7] = EPD_FONT_READ_METADATA << 1;
    memcpy(&m.values[1], &a.primary->identity, sizeof(a.primary->identity));
    m.handle = a.primary->handle; m.size = fixture_size; m.length = 1;
    assert(supply_read(&batch, &m, &data) == EPD_COMPUTE_INVALID && raw_reads == before);
    source_fixture_release(&shared); source_fixture_release(&a);
    assert(source_cache_totals.bytes == capacity * SOURCE_BLOCK_CHARGE);
    assert(source_request_hint(&b, 0, 4096, true) == CRF_OK && raw_reads == before);
    source_fixture_release(&b);
}
static void source_cache_metadata_sources(void) { source_case_run(source_metadata_sources_worker); }

static void source_pressure_worker(void)
{
    remote_font_t a; source_fixture(&a, 1);
    available_bytes = 512u * 1024u;
    assert(source_request(&a, 0, 1024) == CRF_OK && a.primary->blocks);
    assert(provider_reclaim_target(epd_app_psram_available() + 1) == SOURCE_BLOCK_CHARGE);
    assert(!a.primary->blocks);
    fail_alloc = 1;
    assert(source_request(&a, 0, 1024) == CRF_OK && !a.primary->blocks);
    available_bytes = 512u * 1024u; allocation_race = 128;
    assert(source_request(&a, 0, 1024) == CRF_OK && a.primary->blocks);
    assert(!diagnostics.oom_postcheck);
    source_fixture_release(&a);
}

static void source_cache_pressure(void) { source_case_run(source_pressure_worker); }

static void source_invalidations_worker(void)
{
    for (unsigned reason = 0; reason < 6; ++reason) {
        remote_font_t a; source_fixture(&a, 1);
        assert(source_request_hint(&a, 0, 1024, true) == CRF_OK);
        LONG before = raw_reads, locks = storage_locks;
        switch (reason) {
        case 0: fixture_mounted = false; break;
        case 1: ++fixture_session; break;
        case 2: ++fixture_generation; break;
        case 3: fixture_writers = 1; break;
        case 4: ++fixture_signature; break;
        case 5: ++fixture_failure_revision; break;
        }
        assert(source_request(&a, 1024, 1024) == CRF_STALE_SOURCE);
        assert(raw_reads == before && storage_locks == locks && !a.primary->blocks);
        source_fixture_release(&a);
        fixture_mounted = true; fixture_writers = 0;
        source_fixture(&a, 1);
        assert(source_request(&a, 1024, 1024) == CRF_OK && raw_reads == before + 1);
        source_fixture_release(&a);
    }
}
static void source_cache_invalidations(void) { source_case_run(source_invalidations_worker); }

static void source_failures_worker(void)
{
    for (unsigned fault = 1; fault <= 5; ++fault) {
        remote_font_t a; source_fixture(&a, 1);
        assert(source_request_hint(&a, 0, 1024, true) == CRF_OK && a.primary->blocks);
        if (fault <= 2) {
            assert(close_source(a.primary));
            assert(source_fd_restart(&a.primary->fd_state));
        }
        source_fault = fault;
        crf_status result = source_request(&a, 4096, 1024);
        assert(result == (fault == 5 ? CRF_SHORT_READ : CRF_IO_ERROR));
        assert(!a.primary->blocks && !source_cache_totals.bytes);
        source_fixture_release(&a); source_fault = 0;
    }
}
static void source_cache_failures(void) { source_case_run(source_failures_worker); }

static void source_inflight_worker(void)
{
    remote_font_t a; source_fixture(&a, 1); source_inflight_font = &a;
    assert(source_request_hint(&a, 0, 1024, true) == CRF_STALE_SOURCE);
    assert(!a.primary->blocks && !source_cache_totals.bytes);
    source_fixture_release(&a);
}
static void source_cache_inflight(void)
{
    source_inflight_mode = 1; hold_io = 1; source_case_run(source_inflight_worker);
    /* Repeat with explicit cancellation while the filling block is pinned. */
    initialized = false; rt_mutex_detach(&cache_lock); rt_sem_detach(&work_event);
    ResetEvent(io_entered); ResetEvent(io_continue);
    source_inflight_mode = 2; source_case_run(source_inflight_worker);
}
#endif
