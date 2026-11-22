/* SPDX-License-Identifier: Apache-2.0 */
#include "document_runtime.h"
static const char *application_id;
#include "queued_support.inc"

static epd_memory_owner_t page, peer;
static unsigned admissions, transfers, base_descriptors;
static size_t peak_psram;

size_t epd_app_allocation_peak(size_t bytes) { return bytes > SIZE_MAX - 32 ? SIZE_MAX : bytes + 32; }

static void check_peer(void)
{
    epd_memory_account_snapshot_t state;
    CHECK(epd_memory_scope_snapshot(peer, &state));
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        CHECK(!state.associated[b] && !state.backend[b].unused && !state.backend[b].allocating);
        CHECK(!state.backend[b].allocations && !state.backend[b].peak_committed);
        for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) CHECK(!state.backend[b].physical[c]);
    }
}

void identity_runtime_start(const char *id, const char *peer_id)
{
    application_id = id;
    setup();
    epd_memory_metadata_snapshot_t metadata; epd_memory_metadata_snapshot(&metadata);
    CHECK(metadata.descriptors_used > 0); base_descriptors = metadata.descriptors_used - 1;
    CHECK(strcmp(id, peer_id)); peer = epd_memory_owner_create(peer_id);
    CHECK(peer != EPD_MEMORY_OWNER_INVALID && peer != app);
    page = epd_memory_scope_create(app, EPD_MEMORY_SCOPE_PAGE);
    CHECK(page != EPD_MEMORY_OWNER_INVALID);
    CHECK(epd_resource_runtime_page_state(page, 1, true) == EPD_RESOURCE_OK);
    actor = &actors[0];
    CHECK(rt_thread_mem_context_set(NULL, page) == 0);
}

void identity_runtime_pump(unsigned lane, uint32_t now)
{
    ticks = now;
    drain();
    actor = &actors[lane ? 2 : 0];
    CHECK(rt_thread_mem_context_set(NULL, page) == 0);
    check_peer();
}

void identity_runtime_admitted(uint64_t activity)
{
    epd_resource_activity_snapshot_t state;
    CHECK(epd_resource_activity_get(activity, &state) == EPD_RESOURCE_OK);
    CHECK(state.effective.kind == EPD_RESOURCE_INTERACTIVE);
    CHECK(state.state == EPD_RESOURCE_ADMITTED);
    ++admissions;
}

void identity_runtime_owned(const void *allocation)
{
    epd_memory_block_snapshot_t block;
    epd_memory_owner_snapshot_t owner;
    CHECK(epd_memory_block_get(allocation, &block) == EPD_MEMORY_OK);
    CHECK(epd_memory_owner_get(block.owner, &owner));
    CHECK(owner.kind == EPD_MEMORY_SCOPE_SHARED && owner.parent == app);
    CHECK(block.backend == backends[1] && block.physical >= block.size);
    ++transfers;
}

void *identity_alloc(size_t bytes)
{
    void *out = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (physical[1] > peak_psram) peak_psram = physical[1];
    return out;
}
void identity_free(void *memory) { epd_app_free(memory); }
size_t identity_size(const void *memory)
{
    epd_memory_block_snapshot_t block;
    CHECK(epd_memory_block_get(memory, &block) == EPD_MEMORY_OK);
    return block.size;
}
size_t identity_available(void) { return CAPACITY - physical[1]; }

void identity_runtime_finish(void)
{
    drain();
    CHECK(!task_count && !group_count && !blocked);
    finish();
    CHECK(epd_resource_runtime_close_owner(page) == EPD_RESOURCE_ACCEPTED);
    drain(); CHECK(!epd_resource_runtime_owner_busy(page));
    CHECK(epd_resource_runtime_close_owner(app) == EPD_RESOURCE_ACCEPTED);
    drain(); CHECK(!epd_resource_runtime_owner_busy(app));
    for (unsigned i = 0; i < 4; ++i) {
        actor = &actors[i]; CHECK(rt_thread_mem_context_set(NULL, EPD_MEMORY_OWNER_SYSTEM) == 0);
    }
    CHECK(epd_memory_owner_close(page)); epd_memory_owner_release(page);
    CHECK(epd_memory_owner_close(app)); epd_memory_owner_release(app);
    check_peer(); CHECK(epd_memory_owner_close(peer)); epd_memory_owner_release(peer);
    epd_memory_metadata_snapshot_t metadata; epd_memory_metadata_snapshot(&metadata);
    CHECK(!metadata.protocol_errors);
    CHECK(metadata.descriptors_used == base_descriptors);
    CHECK(!irq && !mutexes && !semaphores && !physical[0] && !physical[1]);
    printf("LEDGER admitted=%u adopted=%u peak_psram=%zu physical=0 unused=0 tasks=0 groups=0 peer_peak=0 owners_restored=1 errors=0\n",
           admissions, transfers, peak_psram);
}
