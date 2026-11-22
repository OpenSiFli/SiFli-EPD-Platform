static void error_drained(void)
{
    assert(!lock_depth && !irq_depth && !host_alloc_live);
    for (unsigned i = 1; i < 64; ++i) assert(!handles[i].file);
}

static app_package_status_t error_entry(unsigned route)
{
    epd_app_entry_t entry;
    switch (route) {
    case 0: return app_package_metadata_status("/flash/apps/demo", "demo", &entry, NULL);
    case 1: return app_package_verify("/flash/apps/demo", "demo", true, NULL);
    case 2: return app_sap_metadata("/flash/demo.sap", &entry);
    default: return app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL);
    }
}

static void allocation_classification(void)
{
    unsigned failures = 0;
    assert(APP_PACKAGE_OK == 0 && APP_PACKAGE_FORMAT_ERROR == 1 && APP_PACKAGE_INCOMPATIBLE == 2 &&
           APP_PACKAGE_IO_ERROR == 3 && APP_PACKAGE_INTEGRITY_ERROR == 4 && APP_PACKAGE_NO_MEMORY == 5);
    for (unsigned route = 0; route < 4; ++route) {
        fixture(); archive_fixture(); host_alloc_attempt = 0;
        assert(error_entry(route) == APP_PACKAGE_OK);
        unsigned allocations = host_alloc_attempt; assert(allocations > 10); error_drained();
        for (unsigned fail = 1; fail <= allocations; ++fail) {
            fixture(); archive_fixture(); host_alloc_attempt = 0; host_alloc_fail = fail;
            assert(error_entry(route) == APP_PACKAGE_NO_MEMORY);
            assert(host_alloc_attempt == fail); error_drained(); ++failures;
            host_alloc_fail = 0;
            if (route == 3) assert(clear_stage("/flash/.app-install"));
            assert(error_entry(route) == APP_PACKAGE_OK); error_drained();
        }
    }
    printf("PASS %u allocation failures: real platform JSON, metadata, verifier, SAP metadata/extraction; partial trees and buffers drain; retry succeeds\n", failures);
}

static void preflight_classification(void)
{
    for (unsigned resident = 0; resident < 2; ++resident) {
        for (unsigned fail = 1; fail <= 3; ++fail) {
            fixture(); volume_ready[STORAGE_FLASH] = true; result.state = APP_JOB_IDLE;
            void *module = resident ? app_module_open("/flash/apps/demo/demo.so", "demo") : NULL;
            host_alloc_attempt = 0; host_alloc_fail = fail;
            app_entity_info info = {0};
            assert(!module_open_requested("demo", &info, true));
            assert(strstr(host_warning, "preflight failed: out of memory")); error_drained();
            host_alloc_fail = 0;
            assert(module_open_requested("demo", &info, true));
            if (module) app_module_close(module);
            app_module_close(dlmodule_find("demo")); app_modules_process(); assert(!instances);
        }
    }
    puts("PASS real catalog preflight preserves OOM before both cold and resident verification; recovery opens normally");
}

static void io_format_classification(void)
{
    const char *paths[] = {"/flash/apps/demo/app.json", "/flash/apps/demo/files.sha256", "/flash/apps/demo/demo.so"};
    const char *operations[] = {"open", "read", "short"};
    for (unsigned p = 0; p < 3; ++p) for (unsigned op = 0; op < 3; ++op) {
        fixture(); host_fail_io = operations[op]; host_fail_path = paths[p];
        assert(app_package_verify("/flash/apps/demo", "demo", true, NULL) == APP_PACKAGE_IO_ERROR);
        error_drained();
    }
    for (unsigned route = 2; route < 4; ++route) for (unsigned op = 0; op < 3; ++op) {
        fixture(); archive_fixture(); host_fail_io = operations[op]; host_fail_path = "/flash/demo.sap";
        assert(error_entry(route) == APP_PACKAGE_IO_ERROR); error_drained();
    }
    fixture(); archive_fixture(); host_fail_io = "write"; host_fail_path = "/flash/.app-install/demo.so";
    assert(error_entry(3) == APP_PACKAGE_IO_ERROR); error_drained();
    const char *invalid[] = {"{", "{} junk", "{\"name\":7}", "{\"id\":\"demo\"}"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fixture(); host_put(paths[0], invalid[i], strlen(invalid[i]), false); archive_fixture();
        for (unsigned route = 0; route < 4; ++route) {
            assert(error_entry(route) == APP_PACKAGE_FORMAT_ERROR); error_drained();
        }
    }
    fixture(); host_find("/flash/apps/demo/demo.so")->data[100] ^= 1;
    assert(error_entry(1) == APP_PACKAGE_INTEGRITY_ERROR); error_drained();
    puts("PASS real file failures/short reads remain IO; invalid JSON/schema remain FORMAT; hash mismatch remains INTEGRITY");
}

static void prepare_error_worker(void)
{
    fixture(); archive_fixture();
    char *version = strstr((char *)host_find("/flash/apps/demo/app.json")->data, "\"version\":2");
    assert(version); version[10] = '1';
    strcpy(job_id, "demo"); strcpy(job_source, "/flash/demo.sap");
    job_session = session; job_volume = job_source_volume = STORAGE_FLASH;
    remove_job = false; prepare_ok = true; result.state = APP_JOB_RUNNING;
    volume_ready[STORAGE_FLASH] = true;
}

static void installer_error_classification(void)
{
    prepare_error_worker(); host_alloc_attempt = 0; worker(NULL);
    assert(result.state == APP_JOB_DONE); unsigned allocations = host_alloc_attempt;
    error_drained();
    for (unsigned fail = 1; fail <= allocations; ++fail) {
        prepare_error_worker(); host_alloc_attempt = 0; host_alloc_fail = fail; worker(NULL);
        assert(result.state == APP_JOB_FAILED && result.message == APP_JOB_MESSAGE_NO_MEMORY);
        assert(!host_find("/flash/.app-install")); error_drained();
        host_alloc_fail = 0; epd_app_entry_t entry;
        assert(app_package_metadata("/flash/apps/demo", "demo", &entry) && entry.version == 1);
    }
    fixture(); archive_fixture(); app_list_t list = {session}; epd_app_entry_t entry;
    host_alloc_fail = host_alloc_attempt + 2;
    assert(app_source_metadata(&list, "/flash/demo.sap", &entry) == APP_JOB_MESSAGE_NO_MEMORY);
    host_alloc_fail = 0;
    host_fail_io = "read"; host_fail_path = "/flash/demo.sap";
    assert(app_source_metadata(&list, "/flash/demo.sap", &entry) == APP_JOB_MESSAGE_READ_FAILED);
    error_drained();
    result.state = APP_JOB_IDLE; volume_ready[STORAGE_FLASH] = true;
    host_alloc_fail = host_alloc_attempt + 2;
    uint32_t ticket, previous_ticket = result.ticket;
    assert(app_installer_start_package("/flash/demo.sap", STORAGE_FLASH, &ticket));
    assert(ticket != previous_ticket && result.ticket == ticket && result.state == APP_JOB_FAILED &&
           result.message == APP_JOB_MESSAGE_NO_MEMORY && !app_installer_busy());
    host_alloc_fail = 0; error_drained();
    const app_job_message_t messages[] = {APP_JOB_MESSAGE_READ_FAILED, APP_JOB_MESSAGE_BAD_PACKAGE, APP_JOB_MESSAGE_VERIFY_FAILED};
    for (unsigned kind = 0; kind < 3; ++kind) {
        prepare_error_worker();
        if (kind == 0) { host_fail_io = "read"; host_fail_path = "/flash/demo.sap"; }
        else if (kind == 1) {
            host_put("/flash/apps/demo/app.json", "{", 1, false); archive_fixture();
        } else host_find("/flash/demo.sap")->data[host_find("/flash/demo.sap")->size - 1] ^= 1;
        worker(NULL);
        assert(result.state == APP_JOB_FAILED && result.message == messages[kind]);
        assert(!host_find("/flash/.app-install")); error_drained();
    }
    printf("PASS installer worker propagates all %u allocation failures through publication revalidation without replacing old data; real management metadata maps OOM/IO\n", allocations);
    puts("PASS installer admission publishes a completed failure ticket when its metadata reread runs out of memory");
    puts("PASS installer worker retains IO/FORMAT/INTEGRITY messages and removes failed staging");
}

static void package_error_cases(void)
{
    allocation_classification(); preflight_classification(); io_format_classification(); installer_error_classification();
}
