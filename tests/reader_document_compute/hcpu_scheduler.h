/* SPDX-License-Identifier: Apache-2.0 */
static bookshelf_item_t concurrent_file;
static document_anchor_t concurrent_anchor;
static document_t *concurrent_document;
static void duplicate_open_probe(void)
{
    allocation_probe = NULL;
    char error[96];
    concurrent_document = document_open_at(&concurrent_file, &concurrent_anchor, error, sizeof(error));
    assert(concurrent_document);
}
static void stop_open_probe(void) { allocation_probe = NULL; document_service_shutdown(); }
static void duplicate_adjacent_probe(void)
{
    allocation_probe = NULL;
    concurrent_document = document_adjacent(current_document, 1); assert(concurrent_document);
    assert(current_document->following == concurrent_document && concurrent_document->previous == current_document);
}
static void scheduler_tests(void)
{
    read_wire("epub-next"); next_wire_data = wire_data; next_wire_bytes = wire_bytes;
    read_wire("epub");
    document_t *old = lifecycle_open(false);
    memory_resource resource = {0};
    old->source.resources = (book_resources_t){&resource, memory_open};
    pipe_step_ticks = 2;
    unsigned before = pipe_steps;
    assert(document_worker_step(&service));
    assert(pipe_steps - before == 2 && old->converting && !old->status.complete);
    assert(old->remote && old->pipe && old->output >= 0 && old->observing && observers == 1);
    document_remote_host_t *host = old->remote;
    epd_compute_pipe_host_t pipe = old->pipe;
    before = pipe_steps; assert(document_worker_step(&service));
    assert(pipe_steps - before == 2 && old->remote == host && old->pipe == pipe);
    pipe_step_ticks = 0;
    before = pipe_steps; assert(document_worker_step(&service));
    assert(pipe_steps - before == DOCUMENT_CONVERT_STEPS && !old->status.complete);
    epd_compute_pipe_message_t message = {0}; uint8_t resource_data[EPD_COMPUTE_PIPE_BYTES];
    message.operation = EPD_PIPE_OPEN; strcpy(message.path, "/flash/custom.png");
    assert(remote_service(old->remote, &message, resource_data) == BOOK_OK && resource.opened == 1);
    bookshelf_item_t next_file = old->file; strcpy(next_file.path, "/flash/new-target.epub");
    char error[96]; pipe_shutdown_failures = 3;
    document_t *next = document_open_at(&next_file, &old->requested, error, sizeof(error)); assert(next);
    assert(old->cancelled && !next->claimed && conversion_document == old);
    unsigned opened = pipe_opens;
    for (unsigned i = 0; i < 3; ++i) {
        assert(document_worker_step(&service));
        assert(pipe_opens == opened && old->pipe == pipe && resource.closed == 0);
        assert(old->remote == host && old->output >= 0 && !next->claimed);
    }
    assert(document_worker_step(&service));
    assert(!old->converting && resource.closed == resource.opened && !observers);
    assert(old->status.result == BOOK_CANCELLED && !next->claimed);
    assert(document_worker_step(&service));
    assert(next->converting && next->pipe && pipe_opens == opened + 1 && !next->status.complete);
    document_close(old);
    pump(); assert(next->persistent); lifecycle_close(next);

    old = lifecycle_open(false);
    assert(document_worker_step(&service) && old->converting);
    old->retained = true; old->owners = 0;
    next_file = old->file; strcpy(next_file.path, "/flash/foreground.epub");
    next = document_open_at(&next_file, &old->requested, error, sizeof(error)); assert(next);
    assert(old->released && old->cancelled);
    assert(document_worker_step(&service));
    document_service_process(); assert(documents == next && !next->next && !conversion_document);
    assert(document_worker_step(&service) && next->converting);
    lifecycle_close(next);

    memset(&concurrent_file, 0, sizeof(concurrent_file));
    strcpy(concurrent_file.path, "/flash/double-open.epub"); concurrent_file.format = DOCUMENT_EPUB;
    concurrent_anchor = (document_anchor_t){BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, 0, 0};
    allocation_probe = duplicate_open_probe;
    document_t *same = document_open_at(&concurrent_file, &concurrent_anchor, error, sizeof(error));
    assert(same == concurrent_document && same->owners == 2 && documents == same && !same->next);
    document_close(same); lifecycle_close(same);

    allocation_probe = stop_open_probe;
    assert(!document_open_at(&concurrent_file, &concurrent_anchor, error, sizeof(error)));
    assert(document_open_error() == READER_ERROR_STOPPING);
    document_worker_shutdown(); idle_check(); worker_stopping = false;

    document_t *d = lifecycle_open(true); current_document = d;
    allocation_probe = duplicate_adjacent_probe;
    document_t *neighbor = document_adjacent(d, 1);
    assert(neighbor && neighbor == concurrent_document && neighbor->owners == 2);
    assert(d->following == neighbor && neighbor->previous == d && d->retained && neighbor->retained);
    document_close(neighbor); document_close(neighbor); pump();
    assert(!d->following); lifecycle_close(d);

    d = lifecycle_open(true); d->cache_touch = false; d->stopped = true;
    opened = pipe_opens;
    document_prefetch_enable(d, true);
    assert(document_worker_step(&service));
    neighbor = d->following;
    assert(neighbor && neighbor->previous == d && !neighbor->owners && !neighbor->claimed);
    assert(!d->adjacent_pending && pipe_opens == opened);
    assert(document_adjacent(d, 1) == neighbor && neighbor->owners == 1);
    document_close(d); document_close(neighbor); pump(); idle_check();
    assert(pipe_opens == opened);
    d = lifecycle_open(false);
    assert(document_worker_step(&service) && d->converting && d->chapter_count == 2);
    neighbor = document_adjacent(d, 1);
    assert(neighbor && neighbor->requested.chapter == 1 && d->cancelled && !d->status.complete);
    assert(neighbor->previous == d && d->following == neighbor);
    document_close(d); pump();
    assert(neighbor->persistent && neighbor->chapter == 1 && neighbor->chapter_count == 2);
    uint8_t *previous_wire = wire_data; size_t previous_bytes = wire_bytes;
    wire_data = next_wire_data; wire_bytes = next_wire_bytes;
    verify_records(neighbor, true);
    wire_data = previous_wire; wire_bytes = previous_bytes;
    lifecycle_close(neighbor);
    /* The real EPUB corpus emits a full first spine from source_begin=0. */
    d = lifecycle_open(true);
    assert(!d->source_begin && d->status.complete && !d->chapter_restore);
    d->requested.offset = d->requested.skip_utf8 = 0;
    neighbor = document_adjacent(d, 1); assert(neighbor && neighbor != d);
    committed = false; /* The host cache stub has one path for all chapters. */
    pump();
    assert(neighbor->chapter == 1 && neighbor->status.complete && !neighbor->source_begin);
    document_navigation_t *saved_navigation = d->navigation;
    document_location_t *saved_locations = d->locations;
    document_close(d);
    assert(!d->owners && d->retained && !d->cancelled);
    opened = pipe_opens; before = pipe_steps;
    assert(document_adjacent(neighbor, -1) == d);
    assert(d->owners == 1 && d->navigation == saved_navigation && d->locations == saved_locations);
    assert(!d->requested.offset && !d->requested.skip_utf8);
    pump();
    assert(pipe_opens == opened && pipe_steps == before);
    /* A stale link must not hide another retained full chapter. */
    document_close(d); neighbor->previous = NULL;
    assert(document_adjacent(neighbor, -1) == d && neighbor->previous == d);
    document_close(d);
    document_anchor_t prior = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, 0, UINT32_MAX, 0};
    assert(adjacent_complete_locked(neighbor, &prior) == d);
    d->source_begin = 1; assert(!adjacent_complete_locked(neighbor, &prior)); d->source_begin = 0;
    d->status.result = BOOK_IO_ERROR; assert(!adjacent_complete_locked(neighbor, &prior)); d->status.result = BOOK_OK;
    ++d->file.modified; assert(!adjacent_complete_locked(neighbor, &prior)); --d->file.modified;
    d->chapter_restore = true; assert(!adjacent_complete_locked(neighbor, &prior)); d->chapter_restore = false;
    prior.chapter = neighbor->chapter; assert(!adjacent_complete_locked(neighbor, &prior));
    lifecycle_close(neighbor);
    assert(pipe_opens == opened && pipe_steps == before);
    free(wire_data); wire_data = NULL;
    free(next_wire_data); next_wire_data = NULL;
    puts("HCPU scheduler: 8-step/4ms bounds, foreground preemption, single parser, resource lifetime, duplicate open and adjacent publication passed");
    puts("HCPU A->B->A: same complete document/navigation/source anchors, zero additional pipe opens or parser steps; partial/error/changed/restoring chapters rejected");
}
