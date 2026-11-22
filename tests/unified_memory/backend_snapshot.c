/* SPDX-License-Identifier: Apache-2.0 */
#define main original_host_main
#include "host.c"
#undef main
#include "epd_compute_protocol.h"

static unsigned injected_registration_failures;
static epd_memory_result_t observed_register(const void *key,
    const epd_memory_backend_config_t *config, epd_memory_backend_t *out)
{
    if (injected_registration_failures) {
        --injected_registration_failures;
        return EPD_MEMORY_NO_METADATA;
    }
    return epd_memory_backend_register(key, config, out);
}
#define epd_memory_backend_register observed_register
#include "remote_observe.inc"
#undef epd_memory_backend_register

static const unsigned char backend_keys[EPD_MEMORY_BACKEND_COUNT + 2];

static epd_compute_diag_wire_t observation(void)
{
    epd_compute_diag_wire_t d = {0};
    d.flags = EPD_COMPUTE_DIAG_MEMORY_VALID | EPD_COMPUTE_DIAG_PSRAM_PRESENT |
        EPD_COMPUTE_DIAG_SYSTEM_PRESENT;
    d.heap_size = 1048576; d.heap_used = 262144;
    d.system_size = 65536; d.system_used = 8192;
    return d;
}

static epd_memory_backend_snapshot_t remote_backend(unsigned index)
{
    epd_memory_backend_t b;
    epd_memory_backend_snapshot_t result;
    assert(epd_memory_backend_find(&remote_heap_keys[index], &b));
    assert(epd_memory_backend_get(b, &result));
    return result;
}

static void first_observation(void)
{
    epd_compute_diag_wire_t d = observation();
    uint32_t epoch = epd_memory_capacity_epoch();
    unsigned events = capacity_events;
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch + 2 && capacity_events == events + 1);
    epd_memory_backend_snapshot_t a = remote_backend(0), b = remote_backend(1);
    assert(a.config.capacity == d.heap_size && a.config.unavailable == d.heap_used);
    assert(b.config.capacity == d.system_size && b.config.unavailable == d.system_used);
    assert(!strcmp(a.name, "acpu_algorithm") && !strcmp(b.name, "acpu_system"));
    assert(a.config.parent == EPD_MEMORY_BACKEND_NONE && b.config.parent == EPD_MEMORY_BACKEND_NONE);
    size_t capacity; epd_memory_usage_t usage;
    assert(epd_memory_region_snapshot(EPD_MEMORY_REGION_ACPU, &capacity, &usage) == EPD_MEMORY_OK);
    assert(capacity == d.heap_size - d.heap_used + d.system_size - d.system_used);
    assert(!usage.allocations && !usage.sized);
}

static void same_observation(void)
{
    epd_compute_diag_wire_t d = observation();
    observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch();
    unsigned events = capacity_events;
    for (unsigned i = 0; i < 1000; ++i) observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch);
    assert(capacity_events == events);
}

static void usage_changes(void)
{
    epd_compute_diag_wire_t d = observation();
    observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    ++d.heap_used; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    assert(remote_backend(0).config.unavailable == d.heap_used);
    d.heap_used = 0; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    d.system_used += 16; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    assert(remote_backend(1).config.unavailable == d.system_used);
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
}

static void capacity_changes(void)
{
    epd_compute_diag_wire_t d = observation();
    observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    d.heap_size *= 2; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    d.heap_size /= 4; d.system_size /= 2; observe_remote_memory(&d);
    epoch += 2;
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == ++events);
    assert(remote_backend(0).config.capacity == d.heap_size);
    assert(remote_backend(1).config.capacity == d.system_size);
}

static void remote_restart(void)
{
    epd_compute_diag_wire_t d = observation(), unavailable = {0};
    observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    observe_remote_memory(&unavailable);
    d.session = 77; d.calls[0] = 1; d.heap_peak = d.heap_size; d.errors = 3;
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    d.heap_used = d.system_used = 0;
    observe_remote_memory(&d); epoch += 2;
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == ++events);
    assert(!remote_backend(0).config.unavailable && !remote_backend(1).config.unavailable);
    d.heap_used = 4096; d.system_used = 128;
    observe_remote_memory(&d); epoch += 2;
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == ++events);
    assert(remote_backend(0).config.unavailable == 4096);
}

static void manager_initialization(void)
{
    epd_memory_metadata_snapshot_t metadata;
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.initialized);
    epd_compute_diag_wire_t d = observation();
    observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    assert(epd_memory_init() == EPD_MEMORY_OK);
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(epd_memory_init() == EPD_MEMORY_OK);
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    ++d.heap_used; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch + 1 && capacity_events == events + 1);
    assert(remote_backend(0).config.unavailable == d.heap_used);
}

static void registration_retry(void)
{
    epd_compute_diag_wire_t d = observation();
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    injected_registration_failures = 1;
    observe_remote_memory(&d);
    epd_memory_backend_t id;
    assert(!epd_memory_backend_find(&remote_heap_keys[0], &id));
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == ++epoch && capacity_events == ++events);
    assert(remote_backend(0).config.capacity == d.heap_size);
    ++d.heap_used;
    injected_registration_failures = 1;
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(remote_backend(0).config.unavailable == d.heap_used - 1);
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch + 1 && capacity_events == events + 1);
    assert(remote_backend(0).config.unavailable == d.heap_used);
}

static void invalid_observations(void)
{
    epd_compute_diag_wire_t d = observation();
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    d.flags = 0; observe_remote_memory(&d);
    d = observation(); d.heap_size = 0; d.system_used = d.system_size + 1;
    observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    d = observation(); observe_remote_memory(&d); epoch += 2; ++events;
    d.heap_used = d.heap_size + 1; d.system_size = 0; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(remote_backend(0).config.unavailable == 262144);
    d = observation(); d.heap_used = 2048; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch + 1 && capacity_events == events + 1);
}

static void semantic_configuration(void)
{
    epd_memory_backend_config_t a, b;
    memset(&a, 0x19, sizeof(a)); memset(&b, 0x72, sizeof(b));
    a.region = b.region = EPD_MEMORY_REGION_ACPU;
    a.capacity = b.capacity = 4096; a.unavailable = b.unavailable = 512;
    a.parent = b.parent = EPD_MEMORY_BACKEND_NONE;
    epd_memory_backend_t id, same;
    assert(epd_memory_backend_register(&backend_keys[0], &a, &id) == EPD_MEMORY_OK);
    uint32_t epoch = epd_memory_capacity_epoch();
    assert(epd_memory_backend_register(&backend_keys[0], &b, &same) == EPD_MEMORY_OK && same == id);
    assert(epd_memory_capacity_epoch() == epoch);
    b.region = EPD_MEMORY_REGION_SRAM;
    assert(epd_memory_backend_register(&backend_keys[0], &b, &same) == EPD_MEMORY_OK && same == id);
    assert(epd_memory_capacity_epoch() == ++epoch);
    b.parent = backends[0];
    assert(epd_memory_backend_register(&backend_keys[0], &b, &same) == EPD_MEMORY_OK && same == id);
    assert(epd_memory_capacity_epoch() == ++epoch);
    b.parent = EPD_MEMORY_BACKEND_NONE;
    assert(epd_memory_backend_register(&backend_keys[0], &b, &same) == EPD_MEMORY_OK && same == id);
    assert(epd_memory_capacity_epoch() == ++epoch);
    epd_memory_backend_snapshot_t saved; assert(epd_memory_backend_get(id, &saved));
    assert(saved.config.region == b.region && saved.config.parent == b.parent);
}

static void invalid_registration_retry(void)
{
    epd_memory_backend_config_t c = {EPD_MEMORY_REGION_ACPU, 4096, 0, EPD_MEMORY_BACKEND_NONE};
    epd_memory_backend_t id, out = EPD_MEMORY_BACKEND_NONE;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    uint32_t epoch = epd_memory_capacity_epoch();
    c.parent = id;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &out) == EPD_MEMORY_INVALID);
    assert(out == EPD_MEMORY_BACKEND_NONE && epd_memory_capacity_epoch() == epoch);
    c.parent = EPD_MEMORY_BACKEND_COUNT;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &out) == EPD_MEMORY_INVALID);
    c.parent = EPD_MEMORY_BACKEND_NONE; c.unavailable = c.capacity + 1;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &out) == EPD_MEMORY_INVALID);
    c.unavailable = 64;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &out) == EPD_MEMORY_OK && out == id);
    assert(epd_memory_capacity_epoch() == epoch + 1);
    assert(epd_memory_backend_register(NULL, &c, &out) == EPD_MEMORY_INVALID);
    assert(epd_memory_backend_register(&backend_keys[0], NULL, &out) == EPD_MEMORY_INVALID);
    assert(epd_memory_backend_register(&backend_keys[0], &c, NULL) == EPD_MEMORY_INVALID);
}

static void pending_notification(void)
{
    epd_compute_diag_wire_t d = observation(); observe_remote_memory(&d);
    epd_memory_backend_snapshot_t saved = remote_backend(0);
    epd_memory_backend_config_t c = saved.config; ++c.unavailable;
    epd_memory_backend_t id;
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    assert(epd_memory_backend_register(&remote_heap_keys[0], &c, &id) == EPD_MEMORY_OK);
    assert(capacity_events == events);
    d.heap_used = (uint32_t)c.unavailable; observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch + 1 && capacity_events == events + 1);
    observe_remote_memory(&d); assert(capacity_events == events + 1);
}

static void notification_under_irq(void)
{
    epd_compute_diag_wire_t d = observation(); observe_remote_memory(&d);
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    rt_base_t level = rt_hw_interrupt_disable();
    ++d.heap_used; observe_remote_memory(&d); observe_remote_memory(&d);
    assert(capacity_events == events && epd_memory_capacity_epoch() == epoch + 1);
    rt_hw_interrupt_enable(level);
    observe_remote_memory(&d);
    assert(capacity_events == events + 1 && epd_memory_capacity_epoch() == epoch + 1);
}

static void real_heap_ledger(void)
{
    epd_memory_owner_t owner = epd_memory_owner_create("backend-ledger");
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    void *p = rt_memheap_alloc(heaps[0], 224); assert(p);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_t credit = request(owner, 1024, 0, EPD_MEMORY_OK);
    epd_memory_backend_snapshot_t before, after;
    assert(epd_memory_backend_get(backends[0], &before));
    epd_memory_dispatch_changes();
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    epd_memory_backend_t id;
    for (unsigned i = 0; i < 100; ++i) {
        assert(epd_memory_backend_register(heaps[0], &before.config, &id) == EPD_MEMORY_OK);
        assert(id == backends[0]); epd_memory_dispatch_changes();
    }
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(epd_memory_backend_get(id, &after));
    assert(!memcmp(&before.usage, &after.usage, sizeof(before.usage)));
    assert(account_bytes(owner, id) == physical(p));
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    rt_memheap_free(p); epd_memory_owner_release_trusted(owner); audit();
}

static void waiter_capacity_change(void)
{
    epd_memory_backend_config_t c = {EPD_MEMORY_REGION_ACPU, 4096, 4032, EPD_MEMORY_BACKEND_NONE};
    epd_memory_backend_t id;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    epd_memory_credit_spec_t spec = {0};
    spec.priority = EPD_MEMORY_PRIORITY_NORMAL;
    spec.bytes[id] = 64;
    epd_memory_credit_t credit, held;
    assert(epd_memory_credit_request(EPD_MEMORY_OWNER_SYSTEM, &spec, &held) == EPD_MEMORY_OK);
    spec.timeout_ms = 1000;
    assert(epd_memory_credit_request(EPD_MEMORY_OWNER_SYSTEM, &spec, &credit) == EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t status;
    epd_memory_dispatch_changes(); uint32_t epoch = epd_memory_capacity_epoch();
    unsigned events = capacity_events;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    epd_memory_dispatch_changes(); epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(epd_memory_credit_get(credit, &status) && status.state == EPD_MEMORY_CREDIT_WAITING);
    c.unavailable = 0;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    epd_memory_dispatch_changes();
    assert(epd_memory_capacity_epoch() == epoch + 1 && capacity_events == events + 1);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(credit, &status) && status.state == EPD_MEMORY_CREDIT_LIVE);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); audit();
}

static void full_backend_table(void)
{
    epd_memory_backend_config_t c = {EPD_MEMORY_REGION_ACPU, 4096, 0, EPD_MEMORY_BACKEND_NONE};
    epd_memory_backend_t id = EPD_MEMORY_BACKEND_NONE;
    unsigned i = 0;
    while (epd_memory_backend_register(&backend_keys[i], &c, &id) == EPD_MEMORY_OK) {
        assert(++i < EPD_MEMORY_BACKEND_COUNT);
    }
    assert(i); epd_memory_dispatch_changes();
    uint32_t epoch = epd_memory_capacity_epoch(); unsigned events = capacity_events;
    epd_compute_diag_wire_t d = observation();
    observe_remote_memory(&d); observe_remote_memory(&d);
    assert(epd_memory_capacity_epoch() == epoch && capacity_events == events);
    assert(!epd_memory_backend_find(&remote_heap_keys[0], &id));
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    assert(epd_memory_capacity_epoch() == epoch);
    ++c.unavailable;
    assert(epd_memory_backend_register(&backend_keys[0], &c, &id) == EPD_MEMORY_OK);
    assert(epd_memory_capacity_epoch() == epoch + 1);
}

static const struct { const char *name; void (*run)(void); } snapshot_cases[] = {
    {"snapshot_first", first_observation}, {"snapshot_same", same_observation},
    {"snapshot_usage", usage_changes}, {"snapshot_capacity", capacity_changes},
    {"snapshot_remote_restart", remote_restart}, {"snapshot_manager_init", manager_initialization},
    {"snapshot_retry", registration_retry}, {"snapshot_invalid", invalid_observations},
    {"snapshot_semantics", semantic_configuration}, {"snapshot_invalid_retry", invalid_registration_retry},
    {"snapshot_pending_notify", pending_notification}, {"snapshot_irq_notify", notification_under_irq},
    {"snapshot_heap_ledger", real_heap_ledger}, {"snapshot_waiter", waiter_capacity_change},
    {"snapshot_full_table", full_backend_table},
};

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--snapshots")) {
        for (unsigned i = 0; i < sizeof(snapshot_cases) / sizeof(snapshot_cases[0]); ++i)
            puts(snapshot_cases[i].name);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(snapshot_cases) / sizeof(snapshot_cases[0]); ++i) {
        if (strcmp(argv[1], snapshot_cases[i].name)) continue;
        setup();
        if (strcmp(argv[1], "snapshot_manager_init")) assert(epd_memory_init() == EPD_MEMORY_OK);
        epd_memory_set_capacity_notifier(capacity_event);
        snapshot_cases[i].run();
        assert(!irq_depth && !heap_locks && !critical_depth);
        printf("PASS %s\n", argv[1]);
        return 0;
    }
    return original_host_main(argc, argv);
}
