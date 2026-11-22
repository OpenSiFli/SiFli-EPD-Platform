static void block_scan_late_fit(void)
{
    size_t spans[301];
    for (unsigned i = 0; i < 300; ++i) spans[i] = 128;
    spans[300] = 512;
    heap_spans(spans, 301);
    epd_service_t *s = reclaimable("weather", &ai_definition);
    unsigned allocated = allocations;
    CHECK(app_memory_pressure_allocation_failed_block(1000, 512));
    CHECK(!heap_sem_attempts && allocations == allocated && !s->stopping);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    CHECK(heap_sem_attempts == 1 && heap_sem_acquires == 1 && heap_sem_releases == 1);
    CHECK(allocations == allocated);
    for (unsigned i = 0; i < 100; ++i) app_memory_pressure_process("books", false);
    CHECK(heap_sem_attempts == 1); /* Completed/ordinary polls do not walk the heap. */
    stop_all();
}

static void block_span_boundary(void)
{
    heap_one_span(512);
    CHECK(app_memory_pressure_allocation_failed_block(500, 513));
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(heap_sem_acquires == 1 && heap_sem_releases == 1);
    advance_ms(1000);
    CHECK(app_memory_pressure_allocation_failed_block(500, 512));
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
}

static void block_empty_heap(void)
{
    CHECK(app_memory_pressure_allocation_failed_block(500, 512));
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(heap_sem_acquires == 1 && heap_sem_releases == 1);
}

static void block_normal_no_scan(void)
{
    app_image_psram_memheap.free_list = NULL;
    app_image_psram_memheap.lock.busy = true;
    low_memory();
    CHECK(app_memory_pressure_request(500));
    recovered();
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !heap_sem_attempts);
    CHECK(app_memory_pressure_allocation_failed(500));
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED && !heap_sem_attempts);
    heap_spans(NULL, 0);
    app_image_psram_memheap.lock.busy = false;
}

static void block_heap_busy(void)
{
    resource_backend_register();
    resource_candidate = 100;
    epd_service_t *s = reclaimable("weather", &ai_definition);
    app_image_psram_memheap.free_list = NULL; /* A failed take must not dereference it. */
    app_image_psram_memheap.lock.busy = true;
    CHECK(app_memory_pressure_allocation_failed_block(500, 512));
    for (unsigned i = 0; i < 100; ++i) {
        app_memory_pressure_process("books", false);
        CHECK(pressure.pending && !s->stopping && !resource_selects && !log_text[0]);
    }
    CHECK(heap_sem_attempts == 100 && !heap_sem_acquires && !heap_sem_releases);
    heap_one_span(512);
    app_image_psram_memheap.lock.busy = false;
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    CHECK(heap_sem_acquires == 1 && heap_sem_releases == 1 && !resource_selects);
    stop_all();
}

static void terminal_cooldown(uint32_t generation, app_memory_pressure_result_t result)
{
    size_t logged = strlen(log_text);
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(app_memory_pressure_allocation_failed_block(2000 + i, 1024 + i) == generation);
        app_memory_pressure_process("books", false);
        CHECK(!pressure.pending && pressure.result == result && strlen(log_text) == logged);
        advance_ms(10);
    }
    CHECK(app_memory_pressure_allocation_failed_block(3000, 2048) != generation);
    CHECK(pressure.pending && pressure.min_available == 3000 && pressure_contiguous == 2048);
}

static void block_merge_deadline(void)
{
    app_image_psram_memheap.lock.busy = true;
    uint32_t generation = app_memory_pressure_allocation_failed_block(1000, 512);
    CHECK(generation && pressure.pending);
    uint32_t started = pressure_started;
    advance_ms(4990);
    CHECK(app_memory_pressure_allocation_failed_block(2000, 256) == generation);
    CHECK(app_memory_pressure_allocation_failed_block(500, 1024) == generation);
    CHECK(app_memory_pressure_allocation_failed_block(100, 128) == generation);
    CHECK(pressure.min_available == 2000 && pressure_contiguous == 1024);
    CHECK(pressure_started == started);
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && !heap_sem_releases);
    advance_ms(10);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_TIMED_OUT && !pressure.pending);
    CHECK(heap_sem_attempts == 2 && !heap_sem_acquires);
    terminal_cooldown(generation, APP_MEMORY_PRESSURE_TIMED_OUT);
    CHECK(heap_sem_attempts == 2);
    app_image_psram_memheap.lock.busy = false;
}

static void block_merge_clock_wrap(void)
{
    now_ticks = UINT32_MAX - pressure_ticks(1000);
    block_merge_deadline();
}

static uint32_t interleaved_generation;
static size_t interleaved_available, interleaved_contiguous;
static void merge_after_scan(void)
{
    interleaved_generation = app_memory_pressure_allocation_failed_block(
        interleaved_available, interleaved_contiguous);
}

static void block_merge_during_scan(void)
{
    heap_one_span(768);
    epd_service_t *s = reclaimable("weather", &ai_definition);
    uint32_t generation = app_memory_pressure_allocation_failed_block(1000, 512);
    interleaved_available = 500;
    interleaved_contiguous = 1024;
    heap_after_scan = merge_after_scan;
    app_memory_pressure_process("books", false);
    CHECK(interleaved_generation == generation && pressure.generation == generation);
    CHECK(pressure.pending && pressure.min_available == 1000 && pressure_contiguous == 1024);
    CHECK(!s->stopping && !log_text[0]);
    heap_one_span(1024);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && !s->stopping);
    stop_all();
}

static void block_total_during_scan(void)
{
    available_bytes = 1500;
    heap_one_span(1024);
    uint32_t generation = app_memory_pressure_allocation_failed_block(1000, 512);
    interleaved_available = 2000;
    interleaved_contiguous = 256;
    heap_after_scan = merge_after_scan;
    app_memory_pressure_process(NULL, false);
    CHECK(interleaved_generation == generation && pressure.pending);
    CHECK(pressure.min_available == 2000 && pressure_contiguous == 512);
    available_bytes = 2000;
    app_memory_pressure_process(NULL, false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
}

static void replace_after_scan(void)
{
    /* Finish the first request, then reproduce a new allocation failure with
     * the same block demand, after the original caller has scanned a fit. */
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
    heap_one_span(256);
    interleaved_generation = app_memory_pressure_allocation_failed_block(1000, 512);
    CHECK(pressure.pending);
}

static void block_generation_during_scan(void)
{
    heap_one_span(512);
    uint32_t first = app_memory_pressure_allocation_failed_block(1000, 512);
    heap_after_scan = replace_after_scan;
    app_memory_pressure_process("books", false);
    CHECK(interleaved_generation != first && pressure.generation == interleaved_generation);
    CHECK(pressure.pending && pressure.result == APP_MEMORY_PRESSURE_PENDING);
    heap_one_span(512);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
}

static void resource_complete(void)
{
    CHECK(resource_snapshot.pending_objects == 1);
    resource_snapshot.reclaimed_bytes += resource_snapshot.pending_bytes;
    resource_snapshot.pending_bytes = 0;
    resource_snapshot.pending_objects = 0;
}

static void block_resource_chain(void)
{
    resource_backend_register();
    heap_one_span(256);
    epd_service_t *s = reclaimable("weather", &ai_definition);
    resource_candidate = 16;
    CHECK(app_memory_pressure_allocation_failed_block(1000, 1024));
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && resource_selects == 1 && !s->stopping);
    CHECK(resource_requested == 1024); /* A fragmented heap must not reclaim one byte at a time. */
    for (unsigned i = 0; i < 100; ++i) app_memory_pressure_process("books", false);
    CHECK(resource_selects == 1 && pressure.pending && !s->stopping);
    resource_complete();
    resource_candidate = 32;
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && !force_release && resource_selects == 2 && !s->stopping);
    CHECK(resource_requested == 1024);
    CHECK(resource_snapshot.pending_bytes == 32); /* Total free was already sufficient. */
    resource_complete();
    heap_one_span(1024);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED && resource_selects == 2);
    CHECK(!s->stopping);
    stop_all();
}

static void block_resource_then_service(void)
{
    resource_backend_register();
    heap_one_span(256);
    epd_service_t *s = reclaimable("weather", &ai_definition);
    resource_candidate = 16;
    CHECK(app_memory_pressure_allocation_failed_block(1000, 1024));
    app_memory_pressure_process("books", false);
    resource_complete();
    app_memory_pressure_process("books", false);
    CHECK(pressure.pending && resource_selects == 2 && s->stopping && pins == 1);
    for (unsigned i = 0; i < 100; ++i) {
        app_memory_pressure_process("books", false);
        CHECK(pressure.pending && pins == 1 && !speech_stops && !system_stops);
    }
    retire_stopped();
    CHECK(!pins && !allocations && speech_stops == 1 && system_stops == 1);
    heap_one_span(1024);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
}

static void block_services_exhausted(void)
{
    heap_one_span(256);
    epd_service_t *first = reclaimable("weather", &ai_definition);
    epd_service_t *second = reclaimable("books", &ai_definition);
    CHECK(app_memory_pressure_allocation_failed_block(1000, 1024));
    app_memory_pressure_process("Main", false);
    CHECK(pressure.pending && pins == 2 && (first->stopping != second->stopping));
    retire_stopped();
    CHECK(services && !services->stopping && pins == 1);
    app_memory_pressure_process("Main", false);
    CHECK(pressure.pending && services->stopping); /* One release did not make a fitting block. */
    retire_stopped();
    app_memory_pressure_process("Main", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED && !pins && !allocations);
    CHECK(speech_stops == 2 && system_stops == 2);
}

static void block_success_new_failure(void)
{
    heap_one_span(512);
    uint32_t first = app_memory_pressure_allocation_failed_block(1000, 512);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_SATISFIED);
    heap_one_span(256);
    uint32_t second = app_memory_pressure_allocation_failed_block(1000, 512);
    CHECK(second != first && pressure.pending);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(heap_sem_acquires == 2);
}

static void block_exhausted_cooldown(void)
{
    heap_one_span(256);
    uint32_t generation = app_memory_pressure_allocation_failed_block(1000, 512);
    app_memory_pressure_process("books", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED);
    terminal_cooldown(generation, APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(heap_sem_acquires == 1);
}

static void block_foreground_tasks(void)
{
    heap_one_span(256);
    epd_service_t *front = reclaimable("weather", &ai_definition);
    epd_memory_owner_t owner = app_module_owner_acquire("music");
    CHECK(owner != EPD_MEMORY_OWNER_INVALID);
    CHECK(app_background_bind("music", owner) == EPD_MEMORY_OK);
    epd_service_t *task_service = reclaimable("music", &ai_definition);
    epd_app_task_group_t group;
    epd_memory_owner_t task;
    CHECK(app_background_group_create(owner, &group) == EPD_MEMORY_OK);
    CHECK(app_background_task_begin(group, &task) == EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    CHECK(previous != EPD_MEMORY_OWNER_INVALID);
    epd_app_background_policy_t policy = { EPD_APP_BG_TASKS, group, EPD_APP_BG_RELEASE };
    CHECK(epd_app_background_set(&policy, NULL) == EPD_MEMORY_OK);
    epd_memory_owner_leave_trusted(previous);
    CHECK(app_background_foreground(owner, false) == EPD_MEMORY_OK);
    CHECK(app_memory_pressure_allocation_failed_block(1000, 1024));
    app_memory_pressure_process(NULL, false);
    for (unsigned i = 0; i < 100; ++i) {
        app_memory_pressure_process("weather", false);
        CHECK(pressure.pending && !front->stopping && !task_service->stopping);
    }
    epd_app_background_snapshot_t snapshot;
    CHECK(app_background_snapshot(owner, &snapshot) == EPD_MEMORY_OK);
    CHECK(snapshot.tasks == 1 && snapshot.effective == EPD_APP_BG_TASKS);
    CHECK(app_background_task_end(task));
    CHECK(app_background_group_release(group) == EPD_MEMORY_OK);
    app_memory_pressure_process("weather", false);
    CHECK(task_service->stopping && !front->stopping && pressure.pending);
    retire_stopped();
    app_memory_pressure_process("weather", false);
    CHECK(pressure.result == APP_MEMORY_PRESSURE_EXHAUSTED && !front->stopping);
    stop_all();
    app_background_stopped(owner);
    CHECK(app_background_snapshot(owner, &snapshot) == EPD_MEMORY_OK);
    CHECK(app_background_report(owner, snapshot.revision, EPD_APP_BG_RELEASED) == EPD_MEMORY_OK);
    CHECK(app_background_unbind(owner) == EPD_MEMORY_OK);
    epd_memory_owner_release(owner);
}

static void block_over_capacity(void)
{
    heap_one_span(256);
    uint32_t generation = app_memory_pressure_allocation_failed_block(1000, 512);
    uint32_t started = pressure_started;
    uint32_t rejected = app_memory_pressure_allocation_failed_block(1000, epd_app_psram_capacity() + 1);
    CHECK(rejected != generation && app_memory_pressure_query().result == APP_MEMORY_PRESSURE_EXHAUSTED);
    CHECK(pressure.pending && pressure.generation == generation && pressure_started == started);
    CHECK(pressure_contiguous == 512 && !heap_sem_attempts);
    CHECK(app_memory_pressure_allocation_failed_block(1000, 512) == generation);
    heap_one_span(512);
    app_memory_pressure_process("books", false);
    CHECK(app_memory_pressure_query().result == APP_MEMORY_PRESSURE_SATISFIED);
}
