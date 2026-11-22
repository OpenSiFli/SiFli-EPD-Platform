/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#define app identity_account
#define main existing_resource_tests_main
#include "../unified_resource/runtime_host.c"
#undef main
#undef app
#include "../../src/services/app_background.h"
#include "../../src/platform/epd_cold_runtime.h"
#include "../../modules/hello/hello_state.h"

/* This scenario has no external service. Cold storage is a bounded callback
 * port; the counter model, background policy and memory/resource runtime are real. */
#define EPD_APP_SERVICE_INTERNAL_H
void app_service_background_changed(epd_memory_owner_t owner) { CHECK(owner == identity_account); }
epd_memory_result_t app_service_quiesce(const char *id)
{ epd_memory_owner_snapshot_t s; CHECK(epd_memory_owner_get(identity_account, &s) && !strcmp(s.appid, id)); return EPD_MEMORY_OK; }
#include "../../src/services/app_background.c"
#define app identity_account

static epd_checkpoint_spec_t checkpoint_spec;
static epd_cold_runtime_snapshot_t checkpoint_state;
bool epd_checkpoint_register(const epd_checkpoint_spec_t *spec, epd_checkpoint_t *out)
{
    CHECK(!checkpoint_state.registered && spec->maximum_bytes == 4 && spec->capture && spec->restore);
    checkpoint_spec = *spec; checkpoint_state.registered = true; *out = 1; return true;
}
bool epd_checkpoint_register_edits(const epd_checkpoint_spec_t *spec,
    epd_checkpoint_restore_edits_t restore_edits, epd_checkpoint_t *out)
{
    CHECK(restore_edits);
    return epd_checkpoint_register(spec, out);
}
bool epd_checkpoint_changed(epd_checkpoint_t value)
{
    CHECK(value == 1 && checkpoint_state.registered);
    return app_background_dirty(app, &checkpoint_state.data_revision) == EPD_MEMORY_OK;
}
bool epd_checkpoint_get(epd_checkpoint_t value, epd_cold_runtime_snapshot_t *out)
{ CHECK(value == 1); *out = checkpoint_state; return checkpoint_state.registered; }
bool epd_checkpoint_retry(epd_checkpoint_t value) { (void)value; CHECK(!"unexpected retry"); return false; }
bool epd_cold_runtime_get(epd_memory_owner_t owner, epd_cold_runtime_snapshot_t *out)
{ CHECK(owner == app); *out = checkpoint_state; return checkpoint_state.registered; }
epd_memory_result_t epd_cold_runtime_checkpoint(epd_memory_owner_t owner, uint32_t policy, uint32_t dirty)
{ (void)owner; (void)policy; (void)dirty; CHECK(!"unexpected cold request"); return EPD_MEMORY_INVALID; }

static void setup_identity(const char *id)
{
    epd_memory_backend_config_t config = {EPD_MEMORY_REGION_HCPU_PSRAM, CAPACITY, 0, EPD_MEMORY_BACKEND_NONE};
    actors[0].id = 1; actors[1].id = 2;
    CHECK(epd_memory_backend_register(&app_image_psram_memheap, &config, &backend) == EPD_MEMORY_OK);
    CHECK(epd_memory_init() == EPD_MEMORY_OK);
    app = epd_memory_owner_create(id); CHECK(app != EPD_MEMORY_OWNER_INVALID);
    CHECK(app_background_should_start(id, true));
    CHECK(app_background_bind(id, app) == EPD_MEMORY_OK);
    pages[0] = epd_memory_scope_create(app, EPD_MEMORY_SCOPE_PAGE);
    pages[1] = epd_memory_scope_create(app, EPD_MEMORY_SCOPE_PAGE);
    service = epd_memory_scope_create(app, EPD_MEMORY_SCOPE_SERVICE);
    CHECK(epd_resource_runtime_init(NULL) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[0], 1, true) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_page_state(pages[1], 2, true) == EPD_RESOURCE_OK);
    CHECK(epd_resource_runtime_task_state(service, 3, true) == EPD_RESOURCE_OK);
}
static void counter_business(void)
{
    hello_state_t counter = {0}; unsigned char saved[4]; size_t bytes = 0;
    epd_memory_owner_t old = epd_memory_owner_enter(pages[0]);
    CHECK(hello_state_open(&counter));
    epd_app_background_snapshot_t bg;
    CHECK(epd_app_background_get(&bg) == EPD_MEMORY_OK && bg.effective == EPD_APP_BG_RELEASE);
    for (unsigned i = 0; i < 6; ++i) CHECK(hello_state_increment(&counter));
    CHECK(counter.count == 6 && epd_app_background_get(&bg) == EPD_MEMORY_OK);
    CHECK(bg.effective == EPD_APP_BG_STATE && bg.dirty_revision == 6 && !bg.saved_revision);
    CHECK(checkpoint_spec.capture(checkpoint_spec.user, saved, sizeof(saved), &bytes) && bytes == 4);
    CHECK(saved[0] == 6 && !saved[1] && !saved[2] && !saved[3]);
    counter.count = 0; CHECK(checkpoint_spec.restore(checkpoint_spec.user, saved, bytes) && counter.count == 6);
    CHECK(app_background_saved(app, bg.dirty_revision));
    CHECK(app_background_foreground(app, false) == EPD_MEMORY_OK);
    CHECK(epd_app_background_get(&bg) == EPD_MEMORY_OK);
    CHECK(app_background_report(app, bg.revision, EPD_APP_BG_RETAINED) == EPD_MEMORY_OK);
    CHECK(epd_app_background_get(&bg) == EPD_MEMORY_OK && bg.actual == EPD_APP_BG_RETAINED);
    CHECK(app_background_foreground(app, true) == EPD_MEMORY_OK);
    epd_app_task_group_t group; epd_memory_owner_t task;
    CHECK(epd_app_task_group_create(&group) == EPD_MEMORY_OK);
    CHECK(epd_app_task_begin(group, &task) == EPD_MEMORY_OK);
    epd_app_background_policy_t policy = {EPD_APP_BG_TASKS, group, EPD_APP_BG_STATE};
    CHECK(epd_app_background_set(&policy, NULL) == EPD_MEMORY_OK);
    CHECK(app_background_foreground(app, false) == EPD_MEMORY_OK);
    CHECK(epd_app_background_get(&bg) == EPD_MEMORY_OK && bg.tasks == 1 && bg.effective == EPD_APP_BG_TASKS);
    CHECK(app_background_report(app, bg.revision, EPD_APP_BG_RUNNING) == EPD_MEMORY_OK);
    CHECK(epd_app_task_end(task)); CHECK(epd_app_task_group_release(group) == EPD_MEMORY_OK);
    CHECK(epd_app_background_get(&bg) == EPD_MEMORY_OK && !bg.tasks && bg.effective == EPD_APP_BG_STATE);
    CHECK(app_background_foreground(app, true) == EPD_MEMORY_OK);
    checkpoint_spec.detach(checkpoint_spec.user); CHECK(!counter.checkpoint);
    memset(&checkpoint_spec, 0, sizeof(checkpoint_spec)); memset(&checkpoint_state, 0, sizeof(checkpoint_state));
    epd_memory_owner_leave(old); drain();
}
static void check_activity(epd_resource_activity_t a, epd_resource_kind_t kind)
{
    epd_resource_activity_snapshot_t s;
    CHECK(epd_resource_activity_get(a, &s) == EPD_RESOURCE_OK && s.state == EPD_RESOURCE_ADMITTED);
    CHECK(s.effective.kind == kind);
}
static size_t page_business(bool component)
{
    size_t unit = sizeof(reader_cached_page_t) + 4;
    reader_page_cache_t cache = {0};
    epd_resource_activity_t parent = 0, activity = 0;
    epd_resource_provider_t p = 0; epd_resource_ref_t refs[2] = {0}; void *kept = NULL;
    epd_memory_owner_t old = epd_memory_owner_enter(pages[0]);
    if (component) {
        cache.allocate = allocate; cache.release = dispose; cache.budget = 16384;
        reader_page_resource_attach(&cache);
        epd_reader_page_t *source = calloc(1, sizeof(*source)); CHECK(source);
        source->done = source->collect = 1; source->bytes = 4; memcpy(source->text, "page", 4);
        CHECK(reader_page_cache_put(&cache, 1, 7, source));
        CHECK(reader_page_cache_put(&cache, 20, 7, source)); free(source);
        kept = reader_page_cache_find(&cache, 1, 7); CHECK(kept);
        CHECK(!memcmp(reader_page_cache_text(kept), "page", 4));
        parent = cache.resource_parent; activity = cache.resource_activity;
    } else {
        parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0);
        epd_resource_context_request_t request = {0};
        request.kind = EPD_RESOURCE_NEARBY; request.backend = backend; request.interactive_parent = parent;
        CHECK(epd_resource_context_begin(&request, NULL, &activity) == EPD_RESOURCE_ACCEPTED);
        p = provider(pages[0]);
        for (unsigned i = 0; i < 2; ++i) {
            void *data = allocate(unit); CHECK(data); memset(data, 0, unit);
            memcpy((char *)data + sizeof(reader_cached_page_t), "page", 4);
            epd_resource_descriptor_t d = {0}; d.backend = backend; d.estimated_bytes = unit;
            d.key.bytes[0] = (uint8_t)(i + 1); d.key.length = 1; d.key.version = 7;
            d.discardable = true; d.rebuild_cost = 1; d.local_value = UINT32_MAX - (i ? 20 : 1);
            CHECK(epd_resource_adopt(activity, p, &d, data, unit, &refs[i]) == EPD_RESOURCE_ACCEPTED);
        }
        CHECK(epd_resource_pin(refs[0], &kept) == EPD_RESOURCE_OK);
        CHECK(!memcmp((char *)kept + sizeof(reader_cached_page_t), "page", 4));
    }
    epd_memory_owner_leave(old); drain();
    check_activity(parent, EPD_RESOURCE_INTERACTIVE); check_activity(activity, EPD_RESOURCE_NEARBY);
    epd_resource_stats_t stats; epd_resource_stats(&stats);
    CHECK(stats.resources == 2 && physical == unit * 2); account_usage(app, unit * 2, 0);
    CHECK(epd_resource_reclaim(backend, unit * 2, 2) == unit); drain();
    CHECK(physical == unit && freed == 1); account_usage(app, unit, 0);
    if (component) CHECK(cache.count == 1 && reader_page_cache_find(&cache, 1, 7) == kept);
    else { epd_resource_t resource; epd_resource_snapshot_t s;
        CHECK(epd_resource_ref_get(refs[0], &resource) == EPD_RESOURCE_OK);
        CHECK(epd_resource_get(resource, &s) == EPD_RESOURCE_OK && s.state == EPD_RESOURCE_READY); }
    old = epd_memory_owner_enter(pages[0]);
    if (component) reader_page_cache_close(&cache);
    else {
        CHECK(epd_resource_unpin(refs[0]) == EPD_RESOURCE_OK);
        CHECK(epd_resource_activity_dispose(activity) == EPD_RESOURCE_ACCEPTED);
        CHECK(epd_resource_activity_dispose(parent) == EPD_RESOURCE_ACCEPTED);
        CHECK(epd_resource_provider_unregister(p) == EPD_RESOURCE_PENDING);
    }
    epd_memory_owner_leave(old); drain(); CHECK(!physical && freed == 2);
    return unit;
}
static void capacity_control(void)
{
    epd_memory_credit_spec_t spec = {0}; epd_memory_credit_t occupied;
    spec.bytes[backend] = CAPACITY;
    CHECK(epd_memory_credit_request(service, &spec, &occupied) == EPD_MEMORY_OK);
    epd_resource_activity_t a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 0);
    drain(); epd_resource_activity_snapshot_t s;
    CHECK(epd_resource_activity_get(a, &s) == EPD_RESOURCE_OK && s.state == EPD_RESOURCE_REJECTED);
    epd_memory_owner_t old = epd_memory_owner_enter(pages[0]);
    CHECK(epd_resource_activity_dispose(a) == EPD_RESOURCE_ACCEPTED); epd_memory_owner_leave(old); drain();
    a = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 64, 1000);
    drain(); CHECK(epd_resource_activity_get(a, &s) == EPD_RESOURCE_OK && s.state == EPD_RESOURCE_WAITING);
    epd_memory_credit_end(occupied); epd_memory_owner_release(occupied); epd_memory_dispatch_changes(); drain();
    check_activity(a, EPD_RESOURCE_INTERACTIVE);
    old = epd_memory_owner_enter(pages[0]);
    CHECK(epd_resource_activity_dispose(a) == EPD_RESOURCE_ACCEPTED); epd_memory_owner_leave(old); drain();
}
int main(int argc, char **argv)
{
    CHECK(argc == 3 && (!strcmp(argv[2], "component") || !strcmp(argv[2], "direct")));
    setup_identity(argv[1]); counter_business(); size_t unit = page_business(!strcmp(argv[2], "component"));
    capacity_control();
    epd_memory_owner_t old = epd_memory_owner_enter(pages[0]);
    epd_app_background_policy_t policy = {EPD_APP_BG_RELEASE, 0, EPD_APP_BG_RELEASE};
    CHECK(epd_app_background_set(&policy, NULL) == EPD_MEMORY_OK); epd_memory_owner_leave(old);
    CHECK(app_background_foreground(app, false) == EPD_MEMORY_OK);
    epd_app_background_state_t terminal; CHECK(app_background_prepare_close(app, &terminal) == EPD_MEMORY_OK);
    CHECK(terminal == EPD_APP_BG_RELEASED);
    CHECK(epd_resource_runtime_close_owner(app) == EPD_RESOURCE_ACCEPTED); drain();
    CHECK(!epd_resource_runtime_owner_busy(app));
    epd_app_background_snapshot_t bg; CHECK(app_background_snapshot(app, &bg) == EPD_MEMORY_OK);
    CHECK(app_background_report(app, bg.revision, terminal) == EPD_MEMORY_OK);
    CHECK(app_background_unbind(app) == EPD_MEMORY_OK); CHECK(app_background_forget(argv[1]));
    close_all();
    printf("TRACE {\"counter\":6,\"dirty_saved\":6,\"state_retained\":true,\"tasks_return_state\":true,"
           "\"interactive\":3,\"nearby\":1,\"two_pages_bytes\":%zu,\"pinned_survivors\":1,"
           "\"reclaimed_pages\":1,\"capacity_reject\":true,\"capacity_wait_resume\":true,\"released\":true,\"remaining_bytes\":0}\n", unit * 2);
    return 0;
}
