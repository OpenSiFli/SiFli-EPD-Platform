/* SPDX-License-Identifier: Apache-2.0 */
#define main unified_memory_main
#include "../unified_memory/host.c"
#undef main
#include "epd_json.h"
#include <cJSON.h>
#include <errno.h>

static const char document[] = "{\"id\":\"probe\",\"version\":1,\"items\":[true,null,\"value\"]}";

static void tree_owner(const cJSON *json, epd_memory_owner_t owner)
{
    for (; json; json = json->next) {
        assert(header((void *)json)->pool_ptr == heaps[0]);
        assert(header((void *)json)->owner == owner);
        if (json->string) assert(header(json->string)->owner == owner);
        if (json->valuestring) assert(header(json->valuestring)->owner == owner);
        tree_owner(json->child, owner);
    }
}

static epd_memory_credit_t reserve(epd_memory_backend_t backend, size_t bytes)
{
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 0};
    epd_memory_credit_t credit;
    spec.bytes[backend] = bytes;
    assert(epd_memory_credit_request(0, &spec, &credit) == EPD_MEMORY_OK);
    return credit;
}

static void sram_reserved(void)
{
    cJSON *legacy = cJSON_ParseWithOpts("{\"old\":1}", NULL, 1);
    assert(legacy);
    struct rt_memheap *system_heap = header(legacy)->pool_ptr;
    assert(system_heap != heaps[0]);
    void *holes[4], *barriers[4];
    for (size_t i = 0; i < 4; ++i) {
        holes[i] = rt_malloc(64); barriers[i] = rt_malloc(16);
        assert(holes[i] && barriers[i]);
    }
    for (size_t i = 0; i < 4; ++i) rt_free(holes[i]);
    assert(epd_memory_sdk_init() == EPD_MEMORY_OK);
    epd_memory_backend_t backend;
    assert(epd_memory_backend_find(system_heap, &backend));
    epd_memory_credit_t credit = reserve(backend, system_heap->available_size);
    assert(epd_memory_rejection_start(4, 100) == EPD_MEMORY_OK);
    assert(!cJSON_ParseWithOpts(document, NULL, 1));
    epd_memory_rejection_snapshot_t rejection;
    assert(epd_memory_rejection_read(&rejection));
    assert(rejection.reason == EPD_MEMORY_REJECT_RESERVED && rejection.backend == backend);

    app_memory_init();
    size_t before = heaps[0]->available_size;
    cJSON *json = NULL;
    rt_set_errno(ENOMEM);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    assert(rt_get_errno() == ENOMEM);
    tree_owner(json, 0);
    epd_json_delete(json);
    assert(heaps[0]->available_size == before);
    /* The unchanged generic free also accepts trees created before hooks. */
    epd_json_delete(legacy);
    for (size_t i = 0; i < 4; ++i) rt_free(barriers[i]);
    epd_memory_credit_end(credit); epd_memory_owner_release(credit);
    assert(!pressure_calls);
    audit();
}

static void psram_denied(void)
{
    app_memory_init();
    size_t before = heaps[0]->available_size;
    epd_memory_credit_t credit = reserve(backends[0], before);
    cJSON *json = (cJSON *)1;
    rt_set_errno(71);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_NO_MEMORY);
    assert(!json && rt_get_errno() == 71 && heaps[0]->available_size == before);
    epd_memory_credit_end(credit); epd_memory_owner_release(credit);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    epd_json_delete(json);
    assert(heaps[0]->available_size == before && !pressure_calls);
    audit();
}

static void closed_owner(void)
{
    app_memory_init();
    epd_memory_owner_t owner = epd_memory_owner_create("json-client");
    epd_memory_owner_t previous = epd_memory_owner_enter(owner);
    size_t before = heaps[0]->available_size;
    cJSON *json = NULL;
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    tree_owner(json, owner);
    epd_memory_owner_leave(previous);
    epd_memory_owner_close(owner);
    epd_memory_owner_release(owner);
    assert(account_bytes(owner, backends[0]) > 0);
    epd_json_delete(json);
    assert(heaps[0]->available_size == before);
    epd_memory_owner_snapshot_t snapshot;
    assert(!epd_memory_owner_get(owner, &snapshot));
    audit();
}

static void psram_exhausted(void)
{
    app_memory_init();
    size_t before = heaps[0]->available_size;
    void *occupied = rt_memheap_alloc(heaps[0], before);
    assert(occupied);
    size_t exhausted = heaps[0]->available_size;
    cJSON *json = (cJSON *)1;
    rt_set_errno(72);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_NO_MEMORY);
    assert(!json && rt_get_errno() == 72 && heaps[0]->available_size == exhausted);
    rt_memheap_free(occupied);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    epd_json_delete(json);
    assert(heaps[0]->available_size == before && !pressure_calls);
    audit();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    setup();
    assert(epd_memory_init() == EPD_MEMORY_OK);
    if (!strcmp(argv[1], "sram_reserved")) sram_reserved();
    else if (!strcmp(argv[1], "psram_denied")) psram_denied();
    else if (!strcmp(argv[1], "closed_owner")) closed_owner();
    else if (!strcmp(argv[1], "psram_exhausted")) psram_exhausted();
    else if (!strcmp(argv[1], "backend_init")) test_backend_init();
    else return 2;
    printf("PASS JSON core %s ticks=%u\n", argv[1], RT_TICK_PER_SECOND);
    return 0;
}
