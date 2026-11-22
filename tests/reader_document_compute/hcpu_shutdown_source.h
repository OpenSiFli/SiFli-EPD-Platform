static HANDLE shutdown_waiting, shutdown_recover;
static void shutdown_block_probe(void)
{
    assert(documents && !documents->cleanup_done && documents->pipe && documents->remote);
    assert(worker_service && pipe_shutdown_failures);
    SetEvent(shutdown_waiting);
    assert(WaitForSingleObject(shutdown_recover, 5000) == WAIT_OBJECT_0);
    pipe_shutdown_failures = 0;
    shutdown_delay_probe = NULL;
}
static DWORD WINAPI shutdown_thread(void *unused)
{ (void)unused; document_worker_shutdown(); return 0; }
static void shutdown_ownership_case(bool partial_open)
{
    document_t *d = lifecycle_open(false);
    if (partial_open) {
        pipe_open_partial = true; pipe_open_status = EPD_COMPUTE_COMPONENT_ERROR;
        pipe_shutdown_failures = UINT_MAX;
    }
    assert(document_worker_step(&service) && d->converting);
    pipe_open_partial = false; pipe_open_status = 0;
    pipe_shutdown_failures = UINT_MAX;
    document_service_shutdown();
    shutdown_waiting = CreateEvent(NULL, TRUE, FALSE, NULL);
    shutdown_recover = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(shutdown_waiting && shutdown_recover);
    shutdown_delay_probe = shutdown_block_probe;
    HANDLE worker = CreateThread(NULL, 0, shutdown_thread, NULL, 0, NULL); assert(worker);
    assert(WaitForSingleObject(shutdown_waiting, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(worker, 0) == WAIT_TIMEOUT);
    assert(d->pipe && d->remote && !d->cleanup_done && pipe_opens != pipe_closes);
    SetEvent(shutdown_recover);
    assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
    CloseHandle(worker); CloseHandle(shutdown_waiting); CloseHandle(shutdown_recover);
    idle_check(); worker_stopping = false;
    unsigned delays = shutdown_delays;
    cache_retired_attempts = 3;
    document_worker_shutdown();
    assert(!cache_retired_attempts && shutdown_delays >= delays + 2);
    worker_stopping = false;
}
static void shutdown_ownership_test(void)
{
    read_wire("epub");
    shutdown_ownership_case(false);
    shutdown_ownership_case(true);
    free(wire_data); wire_data = NULL;
    puts("HCPU shutdown ownership: normal and failed OPEN retain worker/pipe until CLOSE succeeds; cache cleanup also gates exit");
}
static document_record_data_t read_one(document_t *d, uint32_t record)
{
    document_record_data_t out;
    document_record_request_t *job = document_record_request(d, record, true); assert(job);
    pump(); assert(document_record_take(job, &out) == 1);
    document_record_cancel(job); document_service_process(); return out;
}
static void spool_position_test(const char *name, document_format_t format)
{
    read_wire(name);
    bookshelf_item_t file = {0}; char error[96];
    snprintf(file.path, sizeof(file.path), "/flash/%s.%s", name, format == DOCUMENT_MD ? "md" : "mobi");
    file.format = format; file.size = 10000; file.modified = 1;
    committed = false; _unlink(persistent_path);
    document_t *d = document_open(&file, error, sizeof(error)); assert(d);
    pump(); assert_terminal(d, BOOK_OK); assert(d->persistent);
    uint32_t record = 0;
    document_record_data_t original;
    for (;;) {
        assert(record < d->status.end);
        original = read_one(d, record);
        if (original.record.kind == DOCUMENT_TEXT && original.record.length >= 6) break;
        record += sizeof(document_record_t) + original.record.length;
    }
    uint32_t byte = 3;
    while (byte && ((unsigned char)original.text[byte] & 0xc0) == 0x80) --byte;
    unsigned reads = io_reads, opens = io_opens;
    document_anchor_t saved;
    assert(document_source_position(d, record, byte, &saved));
    assert(saved.unit == DOCUMENT_SOURCE_SPOOL && saved.offset == record && saved.skip_utf8 == byte && !saved.chapter);
    assert(reads == io_reads && opens == io_opens);
    lifecycle_close(d);
    for (unsigned cached = 1; ; --cached) {
        if (!cached) { committed = false; _unlink(persistent_path); }
        unsigned before = pipe_opens;
        d = document_open_at(&file, &saved, error, sizeof(error)); assert(d);
        document_prefetch_enable(d, false); pump();
        uint32_t target_record, target_byte;
        assert(document_target_cursor(d, &target_record, &target_byte) == 1);
        assert(target_record == record && target_byte == byte);
        assert(pipe_opens == before + (cached ? 0u : 1u));
        document_record_data_t restored = read_one(d, target_record);
        assert(restored.record.length == original.record.length &&
               !memcmp(restored.text, original.text, original.record.length));
        document_prefetch_enable(d, true); pump(); assert_terminal(d, BOOK_OK);
        lifecycle_close(d);
        if (!cached) break;
    }
    free(wire_data); wire_data = NULL;
    printf("HCPU %s saved spool anchor: memory-only save, cache hit and cacheless reopen preserve text+byte\n", name);
}
