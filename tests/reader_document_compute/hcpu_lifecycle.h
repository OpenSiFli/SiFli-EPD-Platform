/* SPDX-License-Identifier: Apache-2.0 */
static void idle_check(void)
{
    document_service_process();
    assert(!documents && !allocated_bytes && !saves && !image_requests && !record_requests);
    assert_compute_released();
}
static document_t *lifecycle_open(bool ready)
{
    char error[160]; bookshelf_item_t file = {0};
    strcpy(file.path, "/flash/lifecycle.epub");
    file.size = 100000; file.modified = 1; file.format = DOCUMENT_EPUB;
    expected_open = (document_anchor_t){BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, 300, 7};
    committed = false; _unlink(persistent_path); _unlink(index_file);
    document_t *d = document_open_at(&file, &expected_open, error, sizeof(error)); assert(d);
    current_document = d;
    if (ready) { pump(); assert(d->persistent && d->status.complete && d->status.result == BOOK_OK); }
    return d;
}
static void lifecycle_close(document_t *d)
{ document_close(d); pump(); idle_check(); }
static void close_during_operation(void)
{
    document_t *d = current_document;
    document_close(d);
    assert(d->cancelled && d->released);
    document_service_process();
    assert(documents == d && !d->cleanup_done);
}
static void shutdown_during_operation(void)
{
    document_service_shutdown();
    assert(current_document->cancelled && !current_document->cleanup_done);
}
static void worker_cancel_during_admission(void)
{
    unsigned reads = io_reads, opens = io_opens;
    document_worker_cancel();
    assert(worker_stopping && current_document->cancelled);
    assert(current_document->compute_wait && current_document->compute_wait->cancelled);
    assert(!current_document->released && !current_document->cleanup_done);
    assert(io_reads == reads && io_opens == opens);
}
static document_image_request_t *cancel_image_job;
static void cancel_image_only(void)
{
    document_image_cancel(cancel_image_job);
    assert(!current_document->cancelled && cancel_image_job->released);
    assert((current_document->compute_wait && current_document->compute_wait->cancelled) ||
           (current_document->image_render && current_document->image_render->cancelled));
}
static uint32_t first_image(document_t *d)
{
    document_reader_t *r = document_reader_create(d); assert(r);
    uint32_t at = 0;
    while (at < d->status.end) {
        document_record_t record; assert(document_reader_record(r, at, &record));
        if (record.kind == DOCUMENT_IMAGE) { document_reader_destroy(r); return at; }
        at += sizeof(record) + record.length;
    }
    assert(false); return 0;
}
static document_image_request_t *queue_image(document_t *d)
{
    document_image_request_t *job = document_image_request(d, first_image(d), 19, 37); assert(job);
    document_bitmap_t bitmap = {0}; assert(document_image_take(job, &bitmap) == 0);
    return job;
}
static void pipe_lifecycle(void)
{
    document_t *d = lifecycle_open(false);
    unsigned opened = pipe_opens, cancelled_waits = wait_cancels;
    pipe_wait_probe = close_during_operation;
    pump(); pipe_wait_probe = NULL;
    assert(pipe_opens == opened && wait_cancels > cancelled_waits); idle_check();

    d = lifecycle_open(false); wait_failures = 1;
    pump(); assert(d->status.result == BOOK_NO_MEMORY && !d->compute_wait);
    lifecycle_close(d);

    d = lifecycle_open(false); pipe_shutdown_failures = 4;
    while (!pipe_shutdown_retries) assert(document_worker_step(&service));
    assert(d->pipe && d->converting && d->status.complete && d->status.result == BOOK_ERROR &&
           d->output >= 0 && observers == 1 && !committed);
    epd_compute_pipe_host_t retained_pipe = d->pipe;
    document_close(d); assert(document_worker_step(&service));
    assert(d->pipe == retained_pipe && !d->cleanup_done);
    pump(); idle_check();

    d = lifecycle_open(false); pipe_wait_probe = shutdown_during_operation;
    assert(document_worker_step(&service)); pipe_wait_probe = NULL;
    document_worker_shutdown(); idle_check(); worker_stopping = false;

    d = lifecycle_open(false);
    opened = pipe_opens; cancelled_waits = wait_cancels;
    pipe_wait_probe = worker_cancel_during_admission;
    assert(document_worker_step(&service)); pipe_wait_probe = NULL;
    assert(pipe_opens == opened && wait_cancels > cancelled_waits);
    document_service_shutdown(); document_worker_shutdown(); idle_check(); worker_stopping = false;
    puts("HCPU pipe admission cancel/OOM/deferred shutdown/service shutdown passed");
}
static void image_lifecycle(void)
{
    document_t *d = lifecycle_open(true);
    document_image_request_t *job = queue_image(d);
    assert(document_worker_step(&service));
    document_bitmap_t bitmap = {0};
    assert(document_image_take(job, &bitmap) == 1 && bitmap.memory);
    assert(bitmap.image.header.w == 19 && bitmap.image.header.h == 37);
    epd_app_free(bitmap.memory);
    document_image_cancel(job); document_service_process(); lifecycle_close(d);

    d = lifecycle_open(true); cancel_image_job = queue_image(d);
    render_wait_probe = cancel_image_only;
    pump(); render_wait_probe = NULL;
    assert(!d->cancelled && !image_requests); lifecycle_close(d);

    d = lifecycle_open(true); cancel_image_job = queue_image(d);
    render_step_probe = cancel_image_only;
    pump(); render_step_probe = NULL;
    assert(!d->cancelled && !image_requests); lifecycle_close(d);

    d = lifecycle_open(true); queue_image(d); render_wait_probe = close_during_operation;
    unsigned opened = render_opens, cancellations = wait_cancels;
    pump(); render_wait_probe = NULL;
    assert(render_opens == opened && wait_cancels > cancellations); idle_check();

    d = lifecycle_open(true); queue_image(d); render_step_probe = close_during_operation;
    cancellations = render_cancels;
    pump(); render_step_probe = NULL;
    assert(render_cancels > cancellations); idle_check();

    d = lifecycle_open(true); job = queue_image(d); render_shutdown_failures = 3;
    assert(document_worker_step(&service));
    assert(document_image_take(job, &bitmap) == -1 && !job->bitmap.memory);
    assert(d->image_render && d->image_retained.memory && d->image_packed.allocation);
    epd_reader_render_t retained_render = d->image_render;
    void *retained_frame = d->image_retained.memory, *retained_packed = d->image_packed.allocation;
    document_image_cancel(job); document_service_process();
    job = queue_image(d); opened = render_opens;
    assert(document_worker_step(&service));
    assert(document_image_take(job, &bitmap) == -1 && render_opens == opened);
    assert(d->image_render == retained_render && d->image_retained.memory == retained_frame);
    assert(d->image_packed.allocation == retained_packed);
    document_image_cancel(job); document_service_process();
    document_close(d); assert(document_worker_step(&service));
    assert(!d->cleanup_done && d->image_render == retained_render);
    pump(); idle_check();

    d = lifecycle_open(true); queue_image(d); render_wait_probe = shutdown_during_operation;
    assert(document_worker_step(&service)); render_wait_probe = NULL;
    document_worker_shutdown(); idle_check(); worker_stopping = false;

    d = lifecycle_open(true); queue_image(d); render_shutdown_failures = 2;
    assert(document_worker_step(&service)); assert(d->image_retained.memory);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    idle_check(); worker_stopping = false;
    puts("HCPU real render kernel: image wait/command cancel, retained buffers and shutdown retry passed");
}
static void index_lifecycle(void)
{
    uint8_t payload[12345];
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 37u + i / 7u);
    document_t *d = lifecycle_open(true);
    unsigned calls = hash_calls, clients = hash_opens;
    assert(document_index_save(d, 73, payload, sizeof(payload)));
    void *data = NULL; size_t size = 0;
    assert(document_index_load(d, 73, &data, &size));
    assert(size == sizeof(payload) && !memcmp(data, payload, size)); epd_app_free(data);
    committed = false;
    assert(!document_index_save(d, 73, payload, sizeof(payload)));
    committed = true;
    assert(!index_load_queued(d, index_file, 74, &data, &size, false));
    replace_enabled = true; pump(); assert(!saves); replace_enabled = false;
    FILE *f = fopen(index_file, "rb"); assert(f);
    document_index_header_t header; assert(fread(&header, sizeof(header), 1, f) == 1); fclose(f);
    assert(header.version == 3 && header.bytes == sizeof(payload));
    assert(header.checksum == reference_fnv(&header, offsetof(document_index_header_t, checksum)));
    assert(document_index_load(d, 73, &data, &size));
    assert(size == sizeof(payload) && !memcmp(data, payload, size)); epd_app_free(data);
    unsigned reads = io_reads, opens = io_opens;
    assert(document_index_load_async(d, 73, &data, &size) == 0);
    assert(io_reads == reads && io_opens == opens);
    pump(); assert(document_index_load_async(d, 73, &data, &size) == 1);
    assert(size == sizeof(payload) && !memcmp(data, payload, size)); epd_app_free(data);
    /* Payload integrity belongs to reader_view's ACPU index_validate. */
    f = fopen(index_file, "r+b"); assert(f);
    fseek(f, sizeof(header) + 101, SEEK_SET); fputc(payload[101] ^ 1, f); fclose(f);
    assert(document_index_load(d, 73, &data, &size));
    assert(((uint8_t *)data)[101] == (uint8_t)(payload[101] ^ 1)); epd_app_free(data);
    for (unsigned field = 0; field < 7; ++field) {
        document_index_header_t bad = header;
        ((uint32_t *)&bad)[field] ^= 1;
        f = fopen(index_file, "r+b"); assert(f); assert(fwrite(&bad, sizeof(bad), 1, f) == 1); fclose(f);
        assert(!document_index_load(d, 73, &data, &size) && !data && !size);
    }
    for (unsigned field = 0; field < 6; ++field) {
        document_index_header_t bad = header;
        ((uint32_t *)&bad)[field] ^= 1;
        bad.checksum = index_header_checksum(&bad);
        f = fopen(index_file, "r+b"); assert(f); assert(fwrite(&bad, sizeof(bad), 1, f) == 1); fclose(f);
        assert(!document_index_load(d, 73, &data, &size));
    }
    f = fopen(index_file, "wb"); assert(f); assert(fwrite(&header, sizeof(header), 1, f) == 1); fclose(f);
    assert(!document_index_load(d, 73, &data, &size));
    service_cancelled = true;
    assert(!document_index_save(d, 73, payload, sizeof(payload)));
    assert(!document_index_load(d, 73, &data, &size));
    service_cancelled = false;
    media_present = false;
    assert(!document_index_load(d, 73, &data, &size)); media_present = true;
    assert(hash_calls == calls && hash_opens == clients);
    lifecycle_close(d);
    puts("HCPU index envelope v3: queued/disk/async restore, header corruption, identity/layout/end/version/length/cancel/media guards; zero payload hash clients or scans");
}
static void lifecycle_tests(void)
{
    read_wire("epub");
    pipe_lifecycle(); image_lifecycle(); index_lifecycle();
    free(wire_data); wire_data = NULL;
    printf("HCPU compute lifecycle: waits=%u/%u render=%u/%u hash=%u/%u retries(pipe/image/hash)=%u/%u/%u\n",
        waits_created, waits_destroyed, render_opens, render_closes, hash_opens, hash_closes,
        pipe_shutdown_retries, render_retries, hash_retries);
}
