/* SPDX-License-Identifier: Apache-2.0 */
static bool source_cancelled_flag;
static unsigned source_releases;
static bool source_cancel_callback(void *user) { (void)user; return source_cancelled_flag; }
static void source_release_callback(void *user) { (void)user; ++source_releases; }
static void cancel_source_wait(void)
{
    source_cancelled_flag = true; document_service_process();
    assert(current_document->compute_wait->cancelled);
}
static void remove_media_wait(void)
{
    media_present = false; document_service_process();
    assert(current_document->compute_wait->cancelled);
}
static void image_cancel_before_wait(void)
{
    allocation_probe = NULL;
    assert(cancel_image_job->active && !current_document->compute_wait);
    document_image_cancel(cancel_image_job);
}
static void stop_during_save(void)
{
    assert(!storage_depth);
    borrowed_save = saves; assert(borrowed_save && borrowed_save->active);
    document_service_shutdown();
    assert(!saves && borrowed_save->references == 1 && borrowed_save->active);
    borrowed_save = NULL;
}
static void assert_terminal(document_t *d, int result)
{
    document_progress_t progress; document_status_t status;
    document_progress(d, &progress); document_status(d, &status);
    assert(progress.complete && progress.result == result && status.complete && status.result == result);
    assert(!d->compute_wait && !d->pipe && !d->converting && !d->remote && !d->observing);
}
static void input_audit(void)
{
    char error[96]; bookshelf_item_t f = {0};
    assert(!document_open_at(NULL, NULL, error, sizeof(error)) && document_open_error() == READER_ERROR_INPUT);
    strcpy(f.path, "relative.epub"); f.format = DOCUMENT_EPUB;
    assert(!document_open(&f, error, sizeof(error)) && document_open_error() == READER_ERROR_INPUT);
    strcpy(f.path, "/flash/../bad.epub");
    assert(!document_open(&f, error, sizeof(error)) && document_open_error() == READER_ERROR_INPUT);
    memset(f.path, 'a', sizeof(f.path));
    assert(!document_open(&f, error, sizeof(error)) && document_open_error() == READER_ERROR_INPUT);
    strcpy(f.path, "/flash/input.epub"); f.format = DOCUMENT_UNKNOWN;
    assert(!document_open(&f, error, sizeof(error)) && document_open_error() == READER_ERROR_TYPE);
    document_source_t source = {.app_id = "test", .cache_path = "/flash/cache", .path = "/flash/file",
        .identity = "1", .format = DOCUMENT_UNKNOWN, .release_user = source_release_callback};
    unsigned releases = source_releases;
    assert(!document_open_source(&source, error, sizeof(error)) && document_open_error() == READER_ERROR_TYPE);
    assert(source_releases == releases); idle_check();
}
static void error_audit(void)
{
    const int opens[] = {EPD_COMPUTE_NO_MEMORY, EPD_COMPUTE_BUSY, EPD_COMPUTE_UNAVAILABLE, EPD_COMPUTE_UNSUPPORTED};
    const int results[] = {BOOK_NO_MEMORY, BOOK_ERROR, BOOK_ERROR, BOOK_UNSUPPORTED};
    for (unsigned i = 0; i < sizeof(opens) / sizeof(opens[0]); ++i) {
        document_t *d = lifecycle_open(false); pipe_open_status = opens[i];
        document_record_request_t *job = document_record_request(d, 0, true); assert(job);
        pump(); assert_terminal(d, results[i]);
        document_record_data_t record; assert(document_record_take(job, &record) == -1);
        if (opens[i] == EPD_COMPUTE_BUSY || opens[i] == EPD_COMPUTE_UNAVAILABLE)
            assert(d->status.error_code == READER_ERROR_UNAVAILABLE);
        document_record_cancel(job); pipe_open_status = 0; lifecycle_close(d);
    }
    const int done[] = {BOOK_NO_MEMORY, BOOK_IO_ERROR, BOOK_UNSUPPORTED, BOOK_ERROR};
    for (unsigned i = 0; i < sizeof(done) / sizeof(done[0]); ++i) {
        document_t *d = lifecycle_open(false); pipe_done_error = done[i];
        document_image_request_t *job = document_image_request(d, UINT32_MAX, 10, 10); assert(job);
        pump(); assert_terminal(d, done[i]);
        document_bitmap_t bitmap = {0}; assert(document_image_take(job, &bitmap) == -1);
        document_image_cancel(job); pipe_done_error = 0; lifecycle_close(d);
    }
    document_t *d = lifecycle_open(true);
    bookshelf_item_t file = d->file; document_anchor_t anchor = d->requested;
    lifecycle_close(d); char error[96];
    d = document_open_at(&file, &anchor, error, sizeof(error)); assert(d); current_document = d;
    psram_capacity = 0; pump(); psram_capacity = 8u * 1024u * 1024u;
    assert_terminal(d, BOOK_NO_MEMORY);
    uint32_t record, byte;
    assert(document_target_cursor(d, &record, &byte) == -1 && !document_navigation_ready(d));
    lifecycle_close(d);
}
static void cancel_audit(void)
{
    document_t *d = lifecycle_open(false);
    d->source.cancelled = source_cancel_callback; d->source.release_user = source_release_callback;
    unsigned releases = source_releases;
    source_cancelled_flag = true; document_service_process(); pump();
    assert_terminal(d, BOOK_CANCELLED); lifecycle_close(d);
    assert(source_releases == releases + 1); source_cancelled_flag = false;

    d = lifecycle_open(false); d->source.cancelled = source_cancel_callback;
    pipe_wait_probe = cancel_source_wait; pump(); pipe_wait_probe = NULL;
    assert_terminal(d, BOOK_CANCELLED); lifecycle_close(d); source_cancelled_flag = false;

    d = lifecycle_open(false); pipe_wait_probe = remove_media_wait;
    pump(); pipe_wait_probe = NULL; assert_terminal(d, BOOK_CANCELLED);
    media_present = true; lifecycle_close(d);

    sd_media = true; d = lifecycle_open(false);
    assert(document_worker_step(&service) && d->converting);
    ++media_generation; document_service_process(); pump(); assert_terminal(d, BOOK_CANCELLED);
    lifecycle_close(d); sd_media = false;

    d = lifecycle_open(false);
    assert(document_worker_step(&service) && d->converting);
    epd_compute_pipe_message_t m = {0}; uint8_t data[EPD_COMPUTE_PIPE_BYTES];
    m.operation = EPD_PIPE_OPEN; strcpy(m.path, "/flash/__missing_document_audit__.epub");
    pipe_done_error = remote_service(d->remote, &m, data); assert(pipe_done_error == BOOK_IO_ERROR);
    pump(); pipe_done_error = 0; assert_terminal(d, BOOK_IO_ERROR); lifecycle_close(d);

    d = lifecycle_open(true); cancel_image_job = queue_image(d);
    unsigned opened = render_opens;
    allocation_probe = image_cancel_before_wait; pump(); allocation_probe = NULL;
    assert(render_opens == opened && !image_requests && !d->cancelled); lifecycle_close(d);
}
static void rapid_targets(void)
{
    document_t *old = lifecycle_open(false);
    assert(document_worker_step(&service) && old->converting);
    bookshelf_item_t file = old->file; char error[96];
    document_anchor_t anchor = old->requested;
    document_t *last = old;
    unsigned opened = pipe_opens;
    for (unsigned i = 0; i < 40; ++i) {
        snprintf(file.path, sizeof(file.path), "/flash/rapid-%u.epub", i);
        anchor.offset = 300 + i;
        document_t *next = document_open_at(&file, &anchor, error, sizeof(error)); assert(next);
        document_close(last); last = next;
    }
    assert(old->cancelled && !last->claimed && pipe_opens == opened);
    document_close(last); pump(); idle_check();
    assert(pipe_opens == opened);

    old = lifecycle_open(false); concurrent_file = old->file;
    strcpy(concurrent_file.path, "/flash/new-wait-target.epub"); concurrent_anchor = old->requested;
    pipe_wait_probe = duplicate_open_probe;
    assert(document_worker_step(&service)); pipe_wait_probe = NULL;
    assert_terminal(old, BOOK_CANCELLED);
    assert(concurrent_document && !concurrent_document->claimed);
    document_close(old); document_close(concurrent_document); pump(); idle_check();
}
static void publication_and_save_audit(void)
{
    document_t *old = lifecycle_open(false); document_close(old);
    assert(document_worker_step(&service) && old->cleanup_done);
    concurrent_file = old->file; strcpy(concurrent_file.path, "/flash/head-insert.epub");
    concurrent_anchor = old->requested; concurrent_document = NULL;
    irq_probe = duplicate_open_probe; document_service_process();
    assert(concurrent_document && documents == concurrent_document && !documents->next);
    lifecycle_close(concurrent_document);

    uint8_t payload[37] = {1, 2, 3};
    assert(document_save_async(index_file, NULL, 0, payload, sizeof(payload)));
    replace_enabled = true; replace_probe = stop_during_save; save_work();
    replace_probe = NULL; replace_enabled = false;
    document_worker_shutdown(); idle_check(); worker_stopping = false;
}
static void image_during_convert(void)
{
    document_t *d = lifecycle_open(false);
    pipe_events_per_step = 1; pipe_step_ticks = 4;
    unsigned opened = pipe_opens;
    for (unsigned i = 0; ; ++i) {
        assert(i < 2000 && document_worker_step(&service) && d->converting);
        epd_compute_pipe_host_t p = d->pipe;
        if (p->message.operation == EPD_PIPE_OUTPUT) {
            book_document_event_t event; memcpy(&event, p->data, sizeof(event));
            if (event.type == BOOK_DOCUMENT_IMAGE_END) break;
        }
    }
    assert(!d->status.complete && pipe_opens == opened + 1);
    assert(cache_publish(d, true) == BOOK_OK);
    document_image_request_t *job = queue_image(d);
    assert(document_worker_step(&service) && d->converting && !d->status.complete);
    document_bitmap_t bitmap = {0};
    assert(document_image_take(job, &bitmap) == 1 && bitmap.memory);
    assert(pipe_opens == opened + 1 && pipe_closes == opened);
    epd_app_free(bitmap.memory); document_image_cancel(job); document_service_process();
    pipe_events_per_step = pipe_step_ticks = 0;
    pump(); assert_terminal(d, BOOK_OK); lifecycle_close(d);
    puts("HCPU image while document parser remains live: render completed before DONE with no second document OPEN");
}
static void idle_and_retained_audit(void)
{
    document_t *d = lifecycle_open(false);
    document_prefetch_enable(d, false); pump();
    assert(d->converting && !d->status.complete && d->target_ready && d->status.end > d->target_record);
    epd_compute_pipe_host_t pipe = d->pipe;
    document_remote_host_t *host = d->remote;
    unsigned steps = pipe_steps, opened = pipe_opens;
    for (unsigned i = 0; i < 20; ++i) assert(!document_worker_step(&service));
    assert(pipe_steps == steps && d->pipe == pipe && d->remote == host);
    document_record_data_t record;
    document_record_request_t *background = document_record_request(d, d->status.end, false); assert(background);
    assert(!document_worker_step(&service) && document_record_take(background, &record) == 0);
    document_record_cancel(background); document_service_process();
    document_record_request_t *foreground = document_record_request(d, d->status.end, true); assert(foreground);
    pump(); assert(document_record_take(foreground, &record) == 1 && pipe_steps > steps);
    document_record_cancel(foreground); document_service_process();
    assert(d->pipe == pipe && pipe_opens == opened && !d->status.complete);
    steps = pipe_steps; assert(!document_worker_step(&service) && pipe_steps == steps);
    d->chapter_count = 1;
    document_prefetch_enable(d, true); pump(); assert_terminal(d, BOOK_OK);
    assert(pipe_opens == opened); lifecycle_close(d);

    d = lifecycle_open(true); document_image_request_t *job = queue_image(d);
    render_shutdown_failures = 2;
    assert(document_worker_step(&service) && d->image_retained.memory && d->image_render);
    document_image_cancel(job); document_service_process(); pump();
    assert(!d->image_retained.memory && !d->image_render && !d->image_packed.allocation);
    uint8_t payload[64] = {1}; unsigned hashes = hash_calls;
    assert(document_index_save(d, 3, payload, sizeof(payload)));
    pump(); assert(hash_calls == hashes && d->owners == 1); lifecycle_close(d);
    puts("HCPU demand idle: no parser STEP without foreground demand, resume uses same pipe; retained render cleanup progresses without new requests");
}
static void audit_tests(void)
{
    read_wire("epub");
    input_audit(); error_audit(); cancel_audit(); rapid_targets(); publication_and_save_audit(); image_during_convert();
    idle_and_retained_audit();
    free(wire_data); wire_data = NULL;
    puts("HCPU terminal/lock audit: input, ACPU OOM/unavailable, missing source/media, source cancellation, rapid targets, restore OOM and shutdown publication passed");
}

static void suspend_tests(void)
{
    read_wire("epub");
    document_t *d = lifecycle_open(false);
    reader_suspended = true; document_prefetch_enable(d, false);
    document_record_request_t *job = document_record_request(d, 0, true); assert(job);
    unsigned opens = pipe_opens, steps = pipe_steps, reads = io_reads, files = io_opens;
    for (unsigned i = 0; i < 20; ++i) assert(!document_worker_step(&service));
    assert(pipe_opens == opens && pipe_steps == steps && io_reads == reads && io_opens == files);
    reader_suspended = false; document_prefetch_enable(d, false); pump();
    document_record_data_t record; assert(document_record_take(job, &record) == 1);
    document_record_cancel(job); document_service_process();
    assert(d->converting && !d->status.complete && d->target_ready);
    d->chapter_count = 1; document_prefetch_enable(d, true); pump(); assert_terminal(d, BOOK_OK);
    lifecycle_close(d);

    d = lifecycle_open(false); pipe_step_ticks = 4;
    assert(document_worker_step(&service) && d->converting && !d->status.end);
    epd_compute_pipe_host_t pipe = d->pipe;
    reader_suspended = true; document_prefetch_enable(d, false);
    job = document_record_request(d, 0, true); assert(job);
    steps = pipe_steps; opens = pipe_opens;
    for (unsigned i = 0; i < 20; ++i) assert(!document_worker_step(&service));
    assert(d->pipe == pipe && pipe_steps == steps && pipe_opens == opens);
    reader_suspended = false; document_prefetch_enable(d, false); pump();
    assert(d->pipe == pipe && pipe_opens == opens && document_record_take(job, &record) == 1);
    document_record_cancel(job); document_service_process();
    reader_suspended = true; document_prefetch_enable(d, false);
    lifecycle_close(d); reader_suspended = false; pipe_step_ticks = 0;

    d = lifecycle_open(true); document_image_request_t *image_job = queue_image(d);
    render_shutdown_failures = 2;
    assert(document_worker_step(&service) && d->image_retained.memory);
    document_image_cancel(image_job); document_service_process();
    reader_suspended = true; document_prefetch_enable(d, false); pump();
    assert(!d->image_retained.memory && !d->image_render && !buffer_count);
    lifecycle_close(d); reader_suspended = false;
    free(wire_data); wire_data = NULL;
    puts("HCPU suspend: no admission/STEP/read before resume, exact parser context retained, cancel and retained cleanup remain runnable");
}

static void optional_metadata_test(void)
{
    read_wire("epub");
    document_t *d = lifecycle_open(false);
    unsigned before = cache_commits;
    metadata_over_limit = true;
    pump();
    assert_terminal(d, BOOK_OK);
    assert(!d->persistent && !committed && cache_commits == before);
    verify_records(d, true);
    metadata_over_limit = false;
    lifecycle_close(d);
    free(wire_data); wire_data = NULL;
    puts("HCPU optional metadata limit: no persistent commit, complete body and source anchors remain readable");
}
