#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef unsigned rt_base_t;
typedef struct { bool closed; } epd_activity_work_t;
typedef struct {
    uint64_t nearby_demand;
    unsigned owners, work_revision, demand;
    bool foreground, cancelled, released, nearby_requested, work_deferred;
    bool stopped, work_foreground, work_nearby;
    epd_activity_work_t *nearby_pending, *work;
} document_t;
enum { EPD_RESOURCE_NEARBY, STORAGE_PATH_MAX = 256 };
static unsigned sequence, queued, closed, lock_depth;
static epd_activity_work_t tickets[8];
static document_t *cancel_during_queue;
static rt_base_t rt_hw_interrupt_disable(void) { return lock_depth++; }
static void rt_hw_interrupt_enable(rt_base_t old) { CHECK(lock_depth == old + 1); lock_depth = old; }
static bool cancelled(document_t *d) { return d->cancelled; }
static size_t epd_resource_file_io_peak(size_t bytes) { return bytes + 1; }
static void document_activity_wake(void *data) { CHECK(!data && !lock_depth); }
static void epd_activity_work_close(epd_activity_work_t *work)
{
    CHECK(!lock_depth);
    if (work) { CHECK(!work->closed); work->closed = true; ++closed; }
}
static epd_activity_work_t *epd_activity_work_queue(int kind, uint64_t parent, size_t bytes,
    size_t io, void (*wake)(void *), void *data)
{
    CHECK(!lock_depth && kind == EPD_RESOURCE_NEARBY && parent == 9);
    CHECK(bytes == 65536 && io == STORAGE_PATH_MAX + 1 && wake == document_activity_wake && !data);
    if (cancel_during_queue) cancel_during_queue->cancelled = true;
    CHECK(queued < sizeof(tickets) / sizeof(tickets[0]));
    return &tickets[queued++];
}
#include "nearby_requeue_production.inc"
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    document_t d = {.owners = 1};
    const uint64_t demand = (UINT64_C(3) << 32) | 4;
    if (!strcmp(argv[1], "cancel-during-queue")) {
        cancel_during_queue = &d;
        document_nearby_request(&d, demand, 9);
        CHECK(queued == 1 && closed == 1 && !d.nearby_pending && !d.work);
        document_nearby_request(&d, demand, 9);
        CHECK(queued == 1);
    } else {
        document_nearby_request(&d, demand, 9);
        CHECK(queued == 1 && d.nearby_pending && d.nearby_requested);
        unsigned revision = d.work_revision;
        document_nearby_request(&d, demand, 9);
        CHECK(queued == 1 && d.work_revision == revision);
        bool installed = !strcmp(argv[1], "active") || !strcmp(argv[1], "promotion");
        if (installed) {
            document_work_handoff(&d, false, false);
            CHECK(d.work == &tickets[0] && d.work_nearby && !d.nearby_pending);
            document_nearby_request(&d, demand, 9);
            CHECK(queued == 1);
        }
        if (!strcmp(argv[1], "promotion")) d.foreground = true;
        document_nearby_request(&d, demand, 0);
        document_work_handoff(&d, d.foreground, false);
        CHECK(!d.nearby_requested && !d.nearby_pending && !d.work && closed == 1);
        if (d.foreground) {
            document_nearby_request(&d, demand, 9);
            CHECK(queued == 1);
            d.foreground = false;
        }
        document_nearby_request(&d, demand, 9);
        CHECK(queued == 2 && d.nearby_requested && d.nearby_pending == &tickets[1]);
        document_work_handoff(&d, false, false);
        CHECK(d.work == &tickets[1] && d.work_nearby && !d.nearby_pending);
        d.cancelled = true;
        document_nearby_request(&d, demand, 9);
        document_work_handoff(&d, false, true);
        CHECK(!d.nearby_requested && !d.work && closed == 2);
    }
    printf("PASS %s\n", argv[1]);
    return 0;
}
