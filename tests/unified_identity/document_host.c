/* SPDX-License-Identifier: Apache-2.0 */
#include "document_runtime.h"
#include "hcpu_host.h"
#define EPD_MEMORY_H
typedef int epd_app_memory_t;
#include "epd_activity_work.h"
#include "epd_reader_render.h"
static bool transport_blocked;
#include "document_ports.inc"

/* Persistent dependency-cache and remote transport doubles are inherited from
 * the document fixture. The in-memory spool reader/cache below is production. */
size_t document_cache_memory_bytes(void) { return 0; }
size_t document_cache_memory_reclaim(size_t bytes) { (void)bytes; return 0; }

static unsigned released_users;
static void release_source(void *user) { assert(user == &released_users); ++released_users; }
static void runtime_step(void)
{
    identity_runtime_pump(1, ++host_ticks);
    document_worker_step(&service);
    identity_runtime_pump(0, host_ticks);
    document_service_process();
}
static void load_input(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file);
    fseek(file, 0, SEEK_END); wire_bytes = (size_t)ftell(file); rewind(file);
    wire_data = malloc(wire_bytes); assert(wire_data);
    assert(fread(wire_data, 1, wire_bytes, file) == wire_bytes); fclose(file);
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    const char *id = argv[1], *scenario = argv[2];
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    snprintf(test_root, sizeof(test_root), "%s", argv[3]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    load_input(argv[4]); identity_runtime_start(id, argv[5]);
    runtime_step();
    document_source_t source = {0};
    source.app_id = id; source.cache_path = "/flash/identity-cache";
    source.identity = "shared-input-revision-1"; source.path = "/flash/identity.txt";
    source.title = "Identity document"; source.format = DOCUMENT_TXT;
    source.release_user = release_source; source.user = &released_users;
    char error[160] = {0};
    document_t *d = document_open_source(&source, error, sizeof(error));
    assert(d && d->work && !pipe_opens && !strcmp(d->app_id, id));
    identity_runtime_owned(d);
    unsigned hits = 0, cancelled = 0;
    if (!strcmp(scenario, "cancel_queued")) {
        document_close(d); ++cancelled;
        assert(!pipe_opens);
    } else {
        identity_runtime_pump(1, host_ticks);
        assert(epd_activity_work_ready(d->work) == 1);
        identity_runtime_admitted(epd_activity_work_activity(d->work));
        if (!strcmp(scenario, "cancel_active")) {
            transport_blocked = true;
            for (unsigned i = 0; i < 100 && !pipe_opens; ++i) runtime_step();
            assert(pipe_opens == 1 && !d->status.complete);
            document_close(d); ++cancelled;
        } else {
            assert(!strcmp(scenario, "normal"));
            for (unsigned i = 0; i < 1000 && !d->status.complete; ++i) runtime_step();
            assert(d->status.complete && d->status.result == BOOK_OK && d->end);
            identity_runtime_pump(1, host_ticks);
            document_reader_t *reader = document_reader_create(d); assert(reader);
            identity_runtime_owned(reader);
            document_record_t first, second;
            assert(document_reader_record(reader, 0, &first));
            assert(first.kind == DOCUMENT_TEXT && first.length == strlen("First source paragraph."));
            char body[64] = {0};
            assert(document_reader_read(reader, sizeof(first), body, first.length));
            assert(!strcmp(body, "First source paragraph."));
            unsigned reads = io_reads, opens = io_opens; LONG allocated = allocations;
            assert(document_reader_record(reader, 0, &second));
            assert(!memcmp(&first, &second, sizeof(first)));
            assert(io_reads == reads && io_opens == opens && allocations == allocated);
            ++hits;
            document_reader_destroy(reader);
            document_record_request_t *request = document_record_request(d, 0, true); assert(request);
            document_record_cancel(request); ++cancelled;
            identity_runtime_pump(0, host_ticks); document_close(d);
        }
    }
    for (unsigned i = 0; i < 1000 && documents; ++i) runtime_step();
    assert(!documents && released_users == 1 && !fd_live && !allocated_bytes);
    identity_runtime_pump(1, host_ticks); document_worker_shutdown();
    identity_runtime_pump(0, host_ticks); document_service_process();
    assert_compute_released(); identity_runtime_finish();
    free(wire_data);
    printf("PASS identity document app=%s scenario=%s hit=%u cancelled=%u release=1 pipes=%u/%u fd=0 ticks=%u\n",
           id, scenario, hits, cancelled, pipe_opens, pipe_closes, RT_TICK_PER_SECOND);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return 0;
}
