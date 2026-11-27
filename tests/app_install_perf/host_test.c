#include "host.h"
#include "lifecycle_host.h"
#include "lifecycle_ports.inc"
#include "load_probe.inc"
#undef rt_free
#define rt_free epd_app_free
#include "../../src/platform/epd_json.c"
#undef rt_free
#define rt_free free
#include "../../src/ui/platform/app_package.c"
#ifdef APP_ACTIVITY_HOST
#include "state_module.inc"
#else
#include "../../src/ui/platform/app_module.c"
#endif
#include "../../src/ui/platform/app_sap.c"
#include "terminate_entry.inc"
#include "installer.inc"
typedef struct { uint32_t card_session; } app_list_t;
#include "management_metadata.inc"
#include "../../src/ui/platform/app_library.c"
static bool app_service_app_stopping(const char *id) { (void)id; return false; }
static void app_service_set_background(const char *id, bool background) { (void)id; (void)background; }
static void remember_load_required(const char *root, const char *id, size_t peak,
                                   const app_package_revision_t *revision)
{ (void)root; (void)id; (void)peak; (void)revision; }
static bool builtin_id(const char *id) { (void)id; return false; }
/* Firmware entry handles are 32-bit. Exercise the original function body on
 * the 64-bit host without dereferencing its truncated UI output handles. */
#pragma warning(push)
#pragma warning(disable: 4311 4302)
#include "catalog_entry.inc"
#pragma warning(pop)

static bool power_before_publish, power_during_commit;
bool board_power_pending(void)
{
    return (power_before_publish && result.phase == APP_JOB_PHASE_PUBLISHING) ||
           (power_during_commit && host_find(host_app_volume == STORAGE_SD ?
                                            "/sdcard/.epd/apps/demo" : "/flash/apps/demo"));
}

static void run_ui_prepare(void)
{
    if (needs_prepare) app_installer_process();
}

static bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry)
{
    char root[256]; snprintf(root, sizeof(root), "%s/%s", directory, id);
    return app_package_metadata(root, id, entry);
}

static void digest_text(const void *data, size_t size, char out[65])
{
    unsigned char digest[32]; assert(!mbedtls_sha256_ret(data, size, digest, 0));
    for (unsigned i = 0; i < 32; ++i) sprintf(out + i * 2, "%02x", digest[i]);
}

static void fixture(void)
{
    epd_json_init();
    host_alloc_attempt = host_alloc_fail = 0;
    host_warning[0] = 0;
    assert(host_alloc_live == host_alloc_resident);
    host_fail_io = host_fail_path = NULL; host_fail_skip = 0;
    assert(!instances);
    assert(host_watch_pin_count() == 0);
    if (background_find_id("demo")) assert(app_background_forget("demo"));
    assert(epd_memory_init() == EPD_MEMORY_OK);
    assert(!host_cold.registered && !host_service_pins && !host_input_pin);
    host_input_cleanups = 0;
    memset(&host_cold, 0, sizeof(host_cold));
    host_cold_result = EPD_MEMORY_WAITING;
    host_cold_owner = host_cleanup_scope = EPD_MEMORY_OWNER_INVALID;
    host_ops_busy = host_resources_busy = host_quiesce_pending = false;
    host_cold_calls = host_cleanup_calls = host_lv_cancel = host_quiesce_calls = 0;
    host_module_init = NULL;
    host_quiesce_hook = NULL;
    host_sem_wait = run_ui_prepare;
    needs_prepare = power_before_publish = power_during_commit = false;
    host_page_open = false;
    host_exit_result = RT_EOK;
    host_watch_reset();
    for (int i = 0; i < file_count_host; ++i) free(files[i].data);
    memset(files, 0, sizeof(files)); memset(handles, 0, sizeof(handles));
    memset(verification_cache, 0, sizeof(verification_cache)); file_count_host = 0;
    available = true; changing = releasing = false; ++session; ++state_revision;
    installer_io_active = false; fail_publish = false; lock_depth = lock_yields = 0;
    host_revocations = host_removals = 0; host_reminder_error = false;
    fail_module_open = false;
    host_app_volume = STORAGE_FLASH; hash_chunks = 0;
    host_put("/flash", NULL, 0, true); host_put("/flash/apps", NULL, 0, true);
    host_put("/flash/apps/demo", NULL, 0, true); host_put("/flash/apps/demo/codecs", NULL, 0, true);
    unsigned char *elf = calloc(1, 1024 * 1024);
    memcpy(elf, "\177ELF\1\1\1", 7); elf[16] = 3; elf[18] = 40; elf[28] = 52; elf[42] = 32; elf[44] = 1;
    elf[52] = 1; elf[52 + 20 + 1] = 16;
    host_put("/flash/apps/demo/demo.so", elf, 1024 * 1024, false);
    host_put("/flash/apps/demo/codecs/codec.so", elf, 1024 * 1024, false);
    free(elf);
    char hash[65], index[512], manifest[1024];
    host_file_t *program = host_find("/flash/apps/demo/demo.so");
    digest_text(program->data, program->size, hash);
    snprintf(index, sizeof(index), "%s  codecs/codec.so\n%s  demo.so\n", hash, hash);
    host_put("/flash/apps/demo/files.sha256", index, strlen(index), false);
    digest_text(index, strlen(index), hash);
    snprintf(manifest, sizeof(manifest), "{\"id\":\"demo\",\"name\":\"Demo\",\"package_format\":1,\"abi\":%u,\"version\":2,\"data_version\":1,\"load_bytes\":4096,\"build_id\":\"%s\",\"files_sha256\":\"%s\",\"settings\":false,\"background\":false,\"libraries\":{\"codec\":4096}}", EPD_APP_ABI, hash, hash);
    host_put("/flash/apps/demo/app.json", manifest, strlen(manifest), false);
}

static void cached_validation(void)
{
    fixture(); size_t peak;
    assert(app_package_verify("/flash/apps/demo", "demo", false, &peak) == APP_PACKAGE_OK);
    assert(peak == 1024 * 1024 + 4096);
    host_file_t *main = host_find("/flash/apps/demo/demo.so"); uint64_t read_bytes = main->bytes_read;
    assert(read_bytes >= main->size);
    assert(app_package_verify("/flash/apps/demo", "demo", false, &peak) == APP_PACKAGE_OK);
    assert(main->bytes_read == read_bytes);
    assert(app_package_verify_library("/flash/apps/demo", "demo", "codec", &peak) == APP_PACKAGE_OK);
    assert(main->bytes_read == read_bytes);
    int writer = package_io_open(main->path, O_WRONLY); assert(writer >= 0);
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) != APP_PACKAGE_OK);
    package_io_close(writer); main->data[100] ^= 1; host_watch_changed(main->path, false);
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) != APP_PACKAGE_OK);
    puts("PASS cache hits read zero payload bytes; libraries never reread main; writes invalidate");
}

static void generations(void)
{
    const char *paths_to_change[] = {"/flash/apps/demo", "/flash/apps/demo/app.json", "/flash/apps/demo/files.sha256", "/flash/apps", "/flash/apps/demo/codecs"};
    for (unsigned i = 0; i < 5; ++i) {
        fixture(); assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
        host_file_t *main = host_find("/flash/apps/demo/demo.so"); uint64_t old = main->bytes_read;
        host_watch_changed(paths_to_change[i], false);
        assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
        assert(main->bytes_read > old);
    }
    fixture(); assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
    ++session; ++state_revision; host_find("/flash/apps/demo/demo.so")->data[100] ^= 1;
    host_watch_changed("/flash/apps/demo/demo.so", false);
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) != APP_PACKAGE_OK);
    fixture(); mutate_on_read = "/flash/apps/demo/demo.so";
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_IO_ERROR);
    fixture(); remove_on_read = true;
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) != APP_PACKAGE_OK);
    puts("PASS installation/metadata/index/parent/session invalidation and mid-read changes");
}

static void resident_validation(void)
{
    fixture(); void *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    memset(verification_cache, 0, sizeof(verification_cache));
    uint64_t before = host_find("/flash/apps/demo/demo.so")->bytes_read;
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read == before);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    int yields = lock_yields;
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    assert(lock_yields == yields);
    app_module_close(module);
    void *codec = app_module_open("/flash/apps/demo/codecs/codec.so", "demo"); assert(codec);
    assert(instances->modules->module == codec);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    app_module_close(module);
    app_module_close(module);
    host_watch_changed("/flash/apps/demo/app.json", false);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == NULL);
    assert(app_module_open("/flash/apps/demo/codecs/codec.so", "demo") == NULL);
    app_module_close(codec);
    app_module_close(module); app_modules_process(); assert(!instances);
    puts("PASS resident module survives cache eviction without disk reads; changed installation rejected");
}

static void directory_and_lru(void)
{
    fixture(); assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
    uint64_t bytes = host_find("/flash/apps/demo/demo.so")->bytes_read;
    assert(installer_rename("/flash/apps/demo", "/flash/.old-demo") == 0);
    assert(installer_rename("/flash/.old-demo", "/flash/apps/demo") == 0);
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read > bytes);
    bytes = host_find("/flash/apps/demo/demo.so")->bytes_read;
    uint32_t old = storage_path_revision("/flash/apps/demo/demo.so");
    for (unsigned i = 0; i < 300; ++i) { char path[96]; snprintf(path, sizeof(path), "/flash/eviction/%u", i); storage_path_revision(path); }
    assert(storage_path_revision("/flash/apps/demo/demo.so") != old);
    assert(app_package_verify("/flash/apps/demo", "demo", false, NULL) == APP_PACKAGE_OK);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read > bytes);
    int fd = package_io_open("/flash/apps/demo/demo.so", O_RDONLY); assert(fd >= 0);
    old = storage_path_revision("/flash/apps/demo/demo.so");
    for (unsigned i = 0; i < 300; ++i) { char path[96]; snprintf(path, sizeof(path), "/flash/pinned/%u", i); storage_path_revision(path); }
    assert(storage_path_revision("/flash/apps/demo/demo.so") == old);
    package_io_close(fd);
    puts("PASS production 128-slot watcher: directory rename invalidates, LRU eviction misses, open fd pins token");
}

static void pin_pressure(unsigned count, bool acquire)
{
    for (unsigned i = 0; i < count; ++i) {
        char path[96]; snprintf(path, sizeof(path), "/flash/pin-pressure/%u", i);
        if (acquire) assert(storage_path_pin(path));
        else storage_path_unpin(path);
    }
}

static void resident_pins(void)
{
    fixture(); void *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    assert(host_watch_pin_count() == 6);
    uint64_t before = host_find("/flash/apps/demo/demo.so")->bytes_read;
    for (unsigned i = 0; i < 1000; ++i) { char path[96]; snprintf(path, sizeof(path), "/flash/books/%u", i); storage_path_revision(path); }
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    assert(host_watch_pin_count() == 6 && host_find("/flash/apps/demo/demo.so")->bytes_read == before);
    app_module_close(module);
    void *codec = app_module_open("/flash/apps/demo/codecs/codec.so", "demo"); assert(codec);
    assert(host_watch_pin_count() == 7);
    for (unsigned i = 0; i < 1000; ++i) { char path[96]; snprintf(path, sizeof(path), "/flash/books/new-%u", i); storage_path_revision(path); }
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module); app_module_close(module);
    assert(app_module_open("/flash/apps/demo/codecs/codec.so", "demo") == codec); app_module_close(codec);
    app_module_close(codec); assert(host_watch_pin_count() == 6);
    host_watch_changed("/flash/apps/demo/demo.so", false);
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    app_module_close(module); app_modules_process(); assert(host_watch_pin_count() == 0);

    fixture(); module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    assert(installer_rename("/flash/apps/demo", "/flash/.old-demo") == 0);
    assert(installer_rename("/flash/.old-demo", "/flash/apps/demo") == 0);
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    app_module_close(module); app_modules_process(); assert(host_watch_pin_count() == 0);

    fixture(); module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    codec = app_module_open("/flash/apps/demo/codecs/codec.so", "demo"); assert(codec);
    app_module_close(module); app_modules_process();
    assert(instances && !instances->closing && host_watch_pin_count() == 7);
    for (unsigned i = 0; i < 1000; ++i) { char path[96]; snprintf(path, sizeof(path), "/flash/after-main-close/%u", i); storage_path_revision(path); }
    assert(app_module_open("/flash/apps/demo/codecs/codec.so", "demo") == codec); app_module_close(codec);
    module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    app_module_close(module); app_module_close(codec); app_modules_process(); assert(host_watch_pin_count() == 0);

    fixture(); pin_pressure(124, true);
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(host_watch_pin_count() == 124 && !instances); pin_pressure(124, false);
    fixture(); pin_pressure(123, true);
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(host_watch_pin_count() == 123 && !instances); pin_pressure(123, false);
    fixture(); module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    pin_pressure(122, true);
    assert(!app_module_open("/flash/apps/demo/codecs/codec.so", "demo"));
    assert(host_watch_pin_count() == 128);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module); app_module_close(module);
    pin_pressure(122, false); app_module_close(module); app_modules_process(); assert(host_watch_pin_count() == 0);
    fixture(); fail_module_open = true;
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(host_watch_pin_count() == 0 && !instances);
    fixture(); host_find("/flash/apps/demo/demo.so")->data[100] ^= 1;
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(host_watch_pin_count() == 0 && !instances);
    puts("PASS resident identity pins survive 1000-path scans; changes reject; shared=5/code=1 pins unwind on failures");
}

static DWORD WINAPI cache_thread(void *context)
{
    unsigned thread = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < 120; ++i) {
        char root[96]; snprintf(root, sizeof(root), "/flash/copies/%u", thread * 3 + i % 3);
        size_t peak = 0;
        app_package_status_t status = i & 1 ? app_package_verify_library(root, "demo", "codec", &peak) :
                                             app_package_verify(root, "demo", false, &peak);
        assert(status == APP_PACKAGE_OK && peak == 1024 * 1024 + 4096);
        assert(lock_depth == 0 && irq_depth == 0);
    }
    return 0;
}

static void concurrent_cache(void)
{
    fixture(); host_put("/flash/copies", NULL, 0, true);
    const char *names[] = {"", "/codecs", "/app.json", "/files.sha256", "/demo.so", "/codecs/codec.so"};
    for (unsigned i = 0; i < 12; ++i) for (unsigned j = 0; j < 6; ++j) {
        char source[96], target[96]; snprintf(source, sizeof(source), "/flash/apps/demo%s", names[j]);
        snprintf(target, sizeof(target), "/flash/copies/%u%s", i, names[j]);
        host_file_t *file = host_find(source); host_put(target, file->data, file->size, file->directory);
    }
    HANDLE threads[4];
    for (unsigned i = 0; i < 4; ++i) { threads[i] = CreateThread(NULL, 0, cache_thread, (void *)(uintptr_t)i, 0, NULL); assert(threads[i]); }
    assert(WaitForMultipleObjects(4, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 4; ++i) CloseHandle(threads[i]);
    puts("PASS four concurrent verifiers rotate eight cache entries (480 checks); no torn metadata or IRQ-held I/O");
}

static void extracted_validation(void)
{
    fixture(); const char *names[] = {"app.json", "files.sha256", "codecs/codec.so", "demo.so"};
    app_package_record_t records[5] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        strcpy(records[i].name, names[i]); char path[256]; snprintf(path, sizeof(path), "/flash/apps/demo/%s", names[i]);
        host_file_t *f = host_find(path); records[i].size = f->size;
        assert(!mbedtls_sha256_ret(f->data, f->size, records[i].digest, 0));
    }
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 4) == APP_PACKAGE_OK);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read == 84);
    assert(host_find("/flash/apps/demo/codecs/codec.so")->bytes_read == 84);
    records[3].digest[0] ^= 1;
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 4) != APP_PACKAGE_OK);
    records[3].digest[0] ^= 1;
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 3) != APP_PACKAGE_OK);
    records[4] = records[3];
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 5) != APP_PACKAGE_OK);
    strcpy(records[3].name, "../demo.so");
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 4) != APP_PACKAGE_OK);
    memset(records[3].name, 'x', sizeof(records[3].name));
    assert(app_package_verify_extracted("/flash/apps/demo", "demo", records, 4) != APP_PACKAGE_OK);
    puts("PASS streamed receipts: only 84 ELF bytes read, bad hash/missing/duplicate/traversal/long paths rejected");
}

static void chunked_copy(void)
{
    fixture(); installer_session = session; installer_io_active = true;
    void *buffer = malloc(COPY_SIZE); assert(buffer);
    assert(copy_file("/flash/apps/demo/demo.so", "/flash/copy.so", buffer));
    assert(lock_depth == 0 && lock_yields >= 64);
    assert(!memcmp(host_find("/flash/copy.so")->data, host_find("/flash/apps/demo/demo.so")->data, 1024 * 1024));
    mutate_on_read = "/flash/apps/demo/demo.so";
    assert(!copy_file("/flash/apps/demo/demo.so", "/flash/changed.so", buffer));
    assert(host_find("/flash/changed.so")->size == 0);
    remove_on_read = true;
    assert(!copy_file("/flash/apps/demo/demo.so", "/flash/interrupted.so", buffer));
    assert(host_find("/flash/interrupted.so")->size == 0);
    available = true;
    assert(!clear_stage("/flash/apps/demo"));
    assert(host_find("/flash/apps/demo/demo.so"));
    free(buffer); installer_io_active = false;
    puts("PASS migration yields storage lock per chunk; card change prevents writes and stale cleanup");
}

static host_file_t *archive_fixture(void)
{
    const char *names[] = {"app.json", "files.sha256", "codecs/codec.so", "demo.so"};
    size_t size = 32;
    for (unsigned i = 0; i < 4; ++i) { char path[256]; snprintf(path, sizeof(path), "/flash/apps/demo/%s", names[i]); size += 48 + strlen(names[i]) + host_find(path)->size; }
    unsigned char *data = calloc(1, size); memcpy(data, "SIFLSAP\0", 8); data[8] = 1; data[12] = 4;
    for (unsigned i = 0; i < 8; ++i) data[16 + i] = (unsigned char)((uint64_t)size >> (8 * i));
    size_t offset = 32;
    for (unsigned i = 0; i < 4; ++i) {
        char path[256]; snprintf(path, sizeof(path), "/flash/apps/demo/%s", names[i]); host_file_t *file = host_find(path);
        data[offset] = (unsigned char)strlen(names[i]);
        for (unsigned j = 0; j < 8; ++j) data[offset + 4 + j] = (unsigned char)((uint64_t)file->size >> (8 * j));
        assert(!mbedtls_sha256_ret(file->data, file->size, data + offset + 12, 0));
        offset += 48; memcpy(data + offset, names[i], strlen(names[i])); offset += strlen(names[i]);
        memcpy(data + offset, file->data, file->size); offset += file->size;
    }
    host_file_t *archive = host_put("/flash/demo.sap", data, size, false); free(data); return archive;
}

static void install_progress(uint64_t done, uint64_t total, void *user)
{
    (void)done; (void)total; (void)user; assert(lock_depth == 0);
}

static void install_archive(void)
{
    fixture(); archive_fixture();
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", install_progress, NULL) == APP_PACKAGE_OK);
    assert(host_find("/flash/.app-install/demo.so")->bytes_read == 84);
    assert(host_find("/flash/.app-install/codecs/codec.so")->bytes_read == 84);
    assert(lock_depth == 0 && lock_yields > 128);
    fixture(); host_file_t *archive = archive_fixture(); archive->data[archive->size - 1] ^= 1;
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", install_progress, NULL) == APP_PACKAGE_INTEGRITY_ERROR);
    puts("PASS real SAP extraction validates hashes and manifest with no second payload pass");
}

static void install_worker(bool fail, bool fail_cleanup)
{
    fixture(); archive_fixture();
    host_file_t *manifest = host_find("/flash/apps/demo/app.json");
    char *version = strstr((char *)manifest->data, "\"version\":2"); assert(version); version[10] = '1';
    fail_publish = fail; strcpy(job_id, "demo"); strcpy(job_source, "/flash/demo.sap");
    job_session = session; job_volume = job_source_volume = STORAGE_FLASH; remove_job = false; prepare_ok = true;
    result.state = APP_JOB_RUNNING; volume_ready[STORAGE_FLASH] = true;
    if (fail_cleanup)
    {
        host_fail_io = "unlink";
        host_fail_path = "/flash/.app-old/files.sha256";
    }
    worker(NULL);
    assert(result.state == (fail ? APP_JOB_FAILED : APP_JOB_DONE));
    assert(host_find("/flash/apps/demo/demo.so"));
    assert(!host_find("/flash/.app-install"));
    epd_app_entry_t entry; assert(app_package_metadata("/flash/apps/demo", "demo", &entry));
    assert(entry.version == (fail ? 1u : 2u)); assert(lock_depth == 0);
    if (fail_cleanup)
    {
        assert(result.message == APP_JOB_MESSAGE_COMMITTED_CLEANUP_FAILED);
        assert(discard_interrupted(&paths[STORAGE_FLASH]));
        assert(!host_find(paths[STORAGE_FLASH].backup));
        puts("PASS update reports committed with a cleanup warning and recovers the old directory");
        return;
    }
    puts(fail ? "PASS failed publication restores old installation" : "PASS installer publishes new installation and releases locks during copy/cleanup");
}

static void verify_staging_before_exit(void)
{
    assert(host_page_open && !host_exit_calls && needs_prepare);
    assert(result.phase == APP_JOB_PHASE_PREPARING);
    assert(host_find("/flash/.app-install/app.json"));
    assert(host_find("/flash/.app-install/codecs/codec.so"));
    run_ui_prepare();
}

static void install_preflight_keeps_page(void)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
        fixture();
        host_file_t *archive = archive_fixture();
        host_page_open = true;
        host_sem_releases = host_exit_calls = host_flush_calls = 0;
        volume_ready[STORAGE_FLASH] = true;
        result.state = APP_JOB_IDLE;
        if (scenario == 1 || scenario == 3)
        {
            char *version = strstr((char *)host_find("/flash/apps/demo/app.json")->data, "\"version\":2");
            assert(version);
            version[10] = '1';
        }
        if (scenario == 1) archive->data[archive->size - 1] ^= 1;
        if (scenario == 2)
        {
            char *version = strstr((char *)host_find("/flash/apps/demo/app.json")->data, "\"version\":2");
            assert(version);
            version[10] = '3';
        }
        if (scenario == 3)
        {
            host_flush_ready = true;
            host_sem_wait = verify_staging_before_exit;
        }
        uint32_t ticket;
        assert(app_installer_start_package("/flash/demo.sap", STORAGE_FLASH, &ticket));
        assert(result.ticket == ticket && result.phase == APP_JOB_PHASE_CHECKING);
        app_installer_process();
        assert(host_page_open && !needs_prepare && !host_exit_calls);
        worker(NULL);
        if (scenario == 3)
        {
            assert(result.state == APP_JOB_DONE && result.message == APP_JOB_MESSAGE_UPDATED);
            assert(!host_page_open && host_flush_calls == 1 && host_exit_calls == 1 && host_sem_releases == 1);
        }
        else
        {
            assert(result.state == APP_JOB_FAILED && result.message ==
                   (scenario == 1 ? APP_JOB_MESSAGE_VERIFY_FAILED : APP_JOB_MESSAGE_ALREADY_INSTALLED));
            assert(host_page_open && !host_flush_calls && !host_exit_calls && !host_sem_releases);
        }
        assert(!needs_prepare && !host_find("/flash/.app-install"));
    }
    puts("PASS duplicate, older and damaged packages never exit the current page; verified update flushes and exits");
}

static void cross_volume_installation(void)
{
    for (storage_volume_t volume = STORAGE_FLASH; volume < STORAGE_COUNT; ++volume)
    {
        const install_paths_t *target = &paths[volume];
        const install_paths_t *source = &paths[volume == STORAGE_FLASH ? STORAGE_SD : STORAGE_FLASH];
        char code[96], origin[96], destination[96], old_file[96], new_file[96];
        snprintf(code, sizeof(code), "%s/demo", target->directory);
        snprintf(origin, sizeof(origin), "%s/data/demo", source->root);
        snprintf(destination, sizeof(destination), "%s/data/demo", target->root);
        assert(join_path(old_file, sizeof(old_file), origin, "document"));
        assert(join_path(new_file, sizeof(new_file), destination, "document"));
        for (unsigned scenario = 0; scenario < 5; ++scenario)
        {
            fixture(); archive_fixture();
            assert(clear_stage("/flash/apps/demo"));
            assert(installer_mkdirs("/sdcard/.epd/apps") && installer_mkdirs(origin));
            host_put(old_file, "keep", 4, false);
            if (scenario == 4)
            {
                assert(installer_mkdirs(destination));
                host_put(new_file, "existing", 8, false);
            }
            host_app_volume = volume;
            volume_ready_set(volume, true);
            result.state = APP_JOB_IDLE;
            uint32_t ticket;
            assert(app_installer_start_package("/flash/demo.sap", volume, &ticket));
            power_before_publish = scenario == 0;
            fail_publish = scenario == 1;
            power_during_commit = scenario == 2;
            if (scenario == 3)
            {
                host_fail_io = "rename";
                host_fail_path = target->data_staging;
            }
            worker(NULL);
            power_before_publish = power_during_commit = fail_publish = false;
            if (scenario < 2)
            {
                assert(result.state == APP_JOB_FAILED && result.message ==
                       (scenario ? APP_JOB_MESSAGE_INSTALL_FAILED : APP_JOB_MESSAGE_BUSY));
                assert(!host_find(code) && !host_find(destination) && host_find(old_file));
                assert(app_installer_start_package("/flash/demo.sap", volume, &ticket));
                worker(NULL);
            }
            assert(result.state == APP_JOB_DONE && host_find(code));
            if (scenario == 3)
            {
                assert(result.message == APP_JOB_MESSAGE_COMMITTED_CLEANUP_FAILED);
                assert(!data_ready("demo", volume) && host_find(old_file));
                host_fail_path = host_fail_io = NULL;
                if (volume == STORAGE_FLASH)
                {
                    ++session;
                    host_watch_remount(STORAGE_SD);
                    host_put(old_file, "other", 5, false);
                }
                assert(discard_interrupted(target) && data_ready("demo", volume));
                host_file_t *preserved = host_find(old_file);
                assert(preserved);
                if (volume == STORAGE_FLASH)
                    assert(preserved->size == 5 && !memcmp(preserved->data, "other", 5));
            }
            else if (scenario == 4) assert(host_find(old_file));
            else assert(!host_find(origin));
            host_file_t *saved = host_find(new_file);
            const char *expected = scenario == 4 ? "existing" : "keep";
            assert(saved && saved->size == strlen(expected) && !memcmp(saved->data, expected, saved->size));
            assert(!host_find(target->data_staging) && !host_find(target->staging));
        }
    }
    puts("PASS both migration directions preserve originals through cancellation and code failure, recover committed data, and leave existing target data untouched");
}

static size_t record_offset(host_file_t *archive, unsigned index)
{
    size_t offset = 32;
    while (index--) offset += 48 + (size_t)little(archive->data + offset, 2) + (size_t)little(archive->data + offset + 4, 8);
    return offset;
}

static void malformed_archive(void)
{
    fixture(); host_file_t *archive = archive_fixture();
    size_t offset = record_offset(archive, 3);
    archive->data[archive->size - 1] ^= 1;
    size_t name_length = (size_t)little(archive->data + offset, 2);
    assert(!mbedtls_sha256_ret(archive->data + offset + 48 + name_length,
                              (size_t)little(archive->data + offset + 4, 8), archive->data + offset + 12, 0));
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_INTEGRITY_ERROR);
    fixture(); archive = archive_fixture(); offset = record_offset(archive, 2);
    archive->data[offset] = EPD_PACKAGE_PATH_MAX;
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_FORMAT_ERROR);
    fixture(); archive = archive_fixture(); offset = record_offset(archive, 2);
    memcpy(archive->data + offset + 48, "../", 3);
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_FORMAT_ERROR);
    fixture(); archive = archive_fixture(); offset = record_offset(archive, 3);
    archive->size = offset; archive->data[12] = 3;
    for (unsigned i = 0; i < 8; ++i) archive->data[16 + i] = (unsigned char)((uint64_t)offset >> (8 * i));
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_INTEGRITY_ERROR);
    fixture(); archive_fixture(); remove_on_read = true;
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_IO_ERROR);
    assert(!host_find("/flash/.app-install"));
    fixture(); archive_fixture(); result.state = APP_JOB_RUNNING; uint32_t ticket;
    assert(!app_installer_start_package("/flash/demo.sap", STORAGE_FLASH, &ticket));
    result.state = APP_JOB_IDLE;
    puts("PASS SAP/index disagreement, oversized/traversing paths, omitted entry, card loss and install exclusion");
}

static void replace_manifest(const char *needle, const char *replacement)
{
    host_file_t *file = host_find("/flash/apps/demo/app.json");
    const char *part = strstr((const char *)file->data, needle); assert(part);
    size_t prefix = part - (const char *)file->data;
    size_t size = file->size - strlen(needle) + strlen(replacement);
    char *text = malloc(size + 1); assert(text);
    memcpy(text, file->data, prefix); strcpy(text + prefix, replacement);
    strcpy(text + prefix + strlen(replacement), part + strlen(needle));
    host_put(file->path, text, size, false); free(text);
}

static void strict_manifest(void)
{
    const char *needles[] = {"\"id\":\"demo\"", "\"version\":2", "\"name\":\"Demo\"", "\"abi\":2"};
    const char *replacements[] = {"\"id\":\"demo\",\"id\":\"demo\"", "\"version\":02", "\"name\":\"Demo\",\"kind\":123", "\"abi\":1.5"};
    for (unsigned i = 0; i < 4; ++i) {
        fixture(); replace_manifest(needles[i], replacements[i]); archive_fixture();
        assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) != APP_PACKAGE_OK);
        epd_app_entry_t entry; assert(app_sap_metadata("/flash/demo.sap", &entry) != APP_PACKAGE_OK);
    }
    fixture(); host_file_t *manifest = host_find("/flash/apps/demo/app.json");
    /* Preserve the valid JSON followed by a forbidden embedded NUL byte. */
    ++manifest->size; archive_fixture();
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_FORMAT_ERROR);
    fixture(); archive_fixture(); mutate_on_read = "/flash/demo.sap";
    assert(app_sap_install("/flash/demo.sap", "/flash/.app-install", "demo", NULL, NULL) == APP_PACKAGE_IO_ERROR);
    fixture(); archive_fixture(); result.state = APP_JOB_IDLE; volume_ready[STORAGE_FLASH] = true;
    uint32_t ticket; assert(app_installer_start_package("/flash/demo.sap", STORAGE_FLASH, &ticket));
    assert(lock_depth == 0); result.state = APP_JOB_IDLE; needs_prepare = false;
    puts("PASS extraction repeats strict JSON validation, rejects source changes, and start metadata has no outer lock");
}

static void content_volume(void)
{
    const char *root = "/flash/apps/demo";
    const char *flash = "/flash/photo.JPG";
    const char *card = "/sdcard/photo.jpg";
    const char *extensions = "\"file_extensions\":[\"jpg\"]";
    fixture();
    replace_manifest("\"name\":\"Demo\"", "\"name\":\"Demo\",\"file_extensions\":[\"jpg\"]");
    assert(app_package_handles_path(root, flash) && app_package_handles_path(root, card));

    replace_manifest("\"id\":\"demo\"", "\"id\":\"gallery\"");
    assert(!app_package_handles_path(root, flash) && app_package_handles_path(root, card));
    replace_manifest(extensions, "\"file_extensions\":[\"jpg\"],\"file_volume\":\"sdcard\"");
    assert(!app_package_handles_path(root, flash) && app_package_handles_path(root, card));

    replace_manifest("\"file_volume\":\"sdcard\"", "\"file_volume\":\"flash\"");
    assert(app_package_handles_path(root, flash) && !app_package_handles_path(root, card));
    replace_manifest("\"file_volume\":\"flash\"", "\"file_volume\":\"usb\"");
    assert(!app_package_handles_path(root, flash) && !app_package_handles_path(root, card));
    replace_manifest("\"id\":\"gallery\"", "\"id\":\"demo\"");
    epd_app_entry_t entry;
    assert(app_package_metadata_status(root, "demo", &entry, NULL) != APP_PACKAGE_OK);
    assert(app_package_verify(root, "demo", true, NULL) != APP_PACKAGE_OK);
    puts("PASS file associations respect explicit volumes and legacy Gallery restrictions");
}

static void real_entry_locks(void)
{
    fixture(); volume_ready[STORAGE_FLASH] = true; result.state = APP_JOB_IDLE;
    app_entity_info info;
    assert(module_open("demo", &info));
    void *module = dlmodule_find("demo"); assert(module);
    assert(hash_chunks >= 32 && lock_depth == 0);
    uint64_t main_bytes = host_find("/flash/apps/demo/demo.so")->bytes_read;
    unsigned before = hash_chunks;
    void *codec = epd_app_library_open("demo", "codec"); assert(codec);
    assert(hash_chunks >= before + 32 && lock_depth == 0);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read == main_bytes);
    epd_app_library_close(codec);
    before = hash_chunks;
    assert(module_open("demo", &info)); assert(hash_chunks == before);
    app_module_close(module); app_module_close(module);
    app_modules_process();
    assert(!instances && host_watch_pin_count() == 0);
    puts("PASS actual catalog/library entry bodies: hash depth=0, payload I/O depth=1, warm main has no rehash");
}

static void volume_identity(void)
{
    fixture(); void *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    uint64_t before = host_find("/flash/apps/demo/demo.so")->bytes_read;
    ++session; ++state_revision; changing = true;
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    changing = false;
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module); app_module_close(module);
    releasing = true; assert(!app_module_open("/flash/apps/demo/demo.so", "demo")); releasing = false;
    ++session; ++state_revision;
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module); app_module_close(module);
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read == before);
    host_watch_changed("/flash/apps/demo/demo.so", false);
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo")); app_module_close(module); app_modules_process();

    fixture(); host_app_volume = STORAGE_SD;
    host_put("/sdcard", NULL, 0, true); host_put("/sdcard/.epd", NULL, 0, true); host_put(STORAGE_SD_APPS, NULL, 0, true);
    const char *names[] = {"", "/codecs", "/app.json", "/files.sha256", "/demo.so", "/codecs/codec.so"};
    for (unsigned i = 0; i < 6; ++i) {
        char source[128], target[128]; snprintf(source, sizeof(source), "/flash/apps/demo%s", names[i]);
        snprintf(target, sizeof(target), STORAGE_SD_APPS "/demo%s", names[i]);
        host_file_t *file = host_find(source); host_put(target, file->data, file->size, file->directory);
    }
    module = app_module_open(STORAGE_SD_APPS "/demo/demo.so", "demo"); assert(module);
    ++session; ++state_revision; host_watch_remount(STORAGE_SD);
    assert(!app_module_open(STORAGE_SD_APPS "/demo/demo.so", "demo")); app_module_close(module);
    app_modules_process();
    assert(!instances && host_watch_pin_count() == 0);
    puts("PASS flash resident survives unrelated TF session/state changes; actual flash writes and SD remount invalidate");
}

static DWORD WINAPI module_thread(void *context)
{
    unsigned thread = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < 80; ++i) {
        void *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
        void *codec = app_module_open("/flash/apps/demo/codecs/codec.so", "demo"); assert(codec);
        if (thread & 1) app_modules_set_locale("en_us");
        app_module_close(module); app_module_close(codec);
    }
    return 0;
}

static void concurrent_modules(void)
{
    fixture(); HANDLE threads[4];
    for (unsigned i = 0; i < 4; ++i) { threads[i] = CreateThread(NULL, 0, module_thread, (void *)(uintptr_t)i, 0, NULL); assert(threads[i]); }
    assert(WaitForMultipleObjects(4, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 4; ++i) CloseHandle(threads[i]);
    app_modules_process();
    assert(!instances && !dlmodule_find("demo") && !dlmodule_find("codec") && host_watch_pin_count() == 0);
    puts("PASS four concurrent lifecycle callers (320 main/library open-close pairs) leave no duplicate instances, modules or pins");
}

static rt_uint32_t existing_resolver(const char *name)
{ return !strcmp(name, "external_api") ? 0x1234 : 0; }

static void unresolved_imports(void)
{
    fixture(); host_missing_import = "missing_api";
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(!host_resolver && !instances && host_watch_pin_count() == 0);
    host_missing_import = NULL;
    assert(app_background_should_start("demo", true));
    void *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    assert(!host_resolver);
    host_missing_import = "missing_codec_api";
    assert(!app_module_open("/flash/apps/demo/codecs/codec.so", "demo"));
    assert(!host_resolver && host_watch_pin_count() == 6 && instances->modules->module == module);
    app_module_close(module); app_modules_process(); assert(!instances && host_watch_pin_count() == 0);

    fixture(); host_resolver = existing_resolver; host_missing_import = "external_api";
    module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    assert(host_resolver == existing_resolver); app_module_close(module); app_modules_process();
    host_missing_import = "missing_api";
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(host_resolver == existing_resolver && !instances && host_watch_pin_count() == 0);
    host_missing_import = NULL; host_resolver = NULL;
    puts("PASS missing imports fail without assert or resource leaks; existing resolver is preserved");
}

static void set_policy(epd_memory_owner_t owner, epd_app_background_mode_t mode,
                       epd_app_task_group_t group, epd_app_background_mode_t after)
{
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    assert(previous != EPD_MEMORY_OWNER_INVALID);
    epd_app_background_policy_t policy = {mode, group, after};
    assert(epd_app_background_set(&policy, NULL) == EPD_MEMORY_OK);
    epd_memory_owner_leave_trusted(previous);
}

static void assert_root(epd_memory_owner_t owner, bool closed)
{
    epd_memory_owner_snapshot_t state;
    assert(epd_memory_owner_get(owner, &state) && state.closed == closed);
}

static void assert_gone(epd_memory_owner_t owner)
{
    epd_memory_owner_snapshot_t state;
    assert(!instances && !dlmodule_find("demo") && !host_watch_pin_count());
    assert(!epd_memory_owner_get(owner, &state));
    assert(!app_background_should_start("demo", false));
}

static void pending_state_reopen(void)
{
    fixture(); struct rt_dlmodule *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    epd_memory_owner_t owner = instances->owner;
    set_policy(owner, EPD_APP_BG_STATE, 0, EPD_APP_BG_RELEASE);
    uint64_t bytes = host_find("/flash/apps/demo/demo.so")->bytes_read;
    for (unsigned i = 0; i < 20; ++i) {
        app_module_close(module); app_modules_process();
        assert(instances && !instances->closing && instances->modules->close_pending);
        assert_root(owner, false);
        assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
        assert(module->nref == 1 && instances->owner == owner && !instances->modules->close_pending);
    }
    assert(host_find("/flash/apps/demo/demo.so")->bytes_read == bytes && !host_cold_calls);
    set_policy(owner, EPD_APP_BG_RELEASE, 0, EPD_APP_BG_RELEASE);
    app_module_close(module); app_modules_process(); assert_gone(owner);
    puts("PASS STATE retained/reopened 20 times with stable root, one SDK ref and no payload rereads");
}

static void pending_tasks_reopen(void)
{
    fixture(); struct rt_dlmodule *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    epd_memory_owner_t owner = instances->owner, task;
    epd_app_task_group_t group;
    assert(app_background_group_create(owner, &group) == EPD_MEMORY_OK);
    assert(app_background_task_begin(group, &task) == EPD_MEMORY_OK);
    set_policy(owner, EPD_APP_BG_TASKS, group, EPD_APP_BG_RELEASE);
    app_module_close(module); app_modules_process(); assert_root(owner, false);
    assert(!instances->closing && !host_quiesce_calls);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module && module->nref == 1);
    app_module_close(module); app_modules_process();
    assert(app_background_task_end(task));
    assert(app_background_group_release(group) == EPD_MEMORY_OK);
    app_modules_process(); assert_gone(owner);
    puts("PASS accepted TASKS permit reopen; actual task end triggers final RELEASE without a new close call");
}

static void register_cold(void)
{
    host_cold_owner = epd_memory_owner_current();
    assert(epd_memory_owner_retain(host_cold_owner));
    host_cleanup_scope = epd_memory_scope_create(host_cold_owner, EPD_MEMORY_SCOPE_CLEANUP);
    assert(host_cleanup_scope != EPD_MEMORY_OWNER_INVALID);
    host_cold.registered = true;
}

static void detach_cold(void)
{
    assert(host_cold.registered && host_cold.closing);
    host_cold.registered = false;
    epd_memory_owner_close(host_cleanup_scope);
    epd_memory_owner_release_trusted(host_cleanup_scope);
    epd_memory_owner_release_trusted(host_cold_owner);
}

static void cleanup_busy(struct rt_dlmodule *module)
{
    (void)module; ++host_cleanup_calls;
    assert(epd_memory_owner_current() == host_cold_owner);
    host_resources_busy = true;
}

static void pressure_cold_lifecycle(void)
{
    fixture(); host_module_init = register_cold;
    struct rt_dlmodule *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    epd_memory_owner_t owner = instances->owner;
    set_policy(owner, EPD_APP_BG_STATE, 0, EPD_APP_BG_RELEASE);
    assert(app_background_dirty(owner, NULL) == EPD_MEMORY_OK);
    module->cleanup_func = cleanup_busy;
    app_module_close(module); app_modules_process(); assert(!host_cold_calls);
    assert(!app_modules_reclaim("demo"));
    assert(app_modules_reclaim("other")); app_modules_process();
    assert(host_cold_calls && !instances->closing && !app_background_clean(owner));
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    assert(!instances->eviction_requested && module->nref == 1);
    app_module_close(module); app_modules_process();
    host_cold_result = EPD_MEMORY_INVALID;
    assert(app_modules_reclaim("other")); app_modules_process(); assert(!instances->closing);
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    app_module_close(module); app_modules_process();
    host_cold_result = EPD_MEMORY_OK; host_cold.durable = true;
    assert(app_modules_reclaim("other")); app_modules_process();
    assert(instances->closing && instances->close_prepared && host_cold.closing);
    assert_root(owner, false); /* Provider can still capture/detach before core close. */
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(!app_background_should_start("demo", true));
    assert(!host_cleanup_calls && app_background_clean(owner));
    detach_cold(); host_ops_busy = true; app_modules_process();
    assert_root(owner, true); assert(!host_cleanup_calls && instances);
    host_ops_busy = false; app_modules_process();
    assert(host_cleanup_calls == 1 && instances && host_resources_busy);
    app_modules_process(); assert(host_cleanup_calls == 1 && instances);
    host_resources_busy = false; app_modules_process();
    assert(!instances && !dlmodule_find("demo") && !host_watch_pin_count());
    epd_memory_owner_snapshot_t state;
    assert(!epd_memory_owner_get(owner, &state));
    assert(app_background_should_start("demo", false)); /* Queued explicit enter. */
    module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    assert(instances->owner != owner && host_cold.registered);
    owner = instances->owner;
    module->cleanup_func = NULL;
    app_module_close(module); app_modules_process(); detach_cold();
    app_modules_process(); assert_gone(owner);
    puts("PASS pressure STATE save/wait/reopen/retry/seal/detach/drain/cleanup-once/unbind/reload ordering");
}

static void release_idle_services(void)
{
    fixture(); struct rt_dlmodule *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    epd_memory_owner_t owner = instances->owner;
    assert(app_module_open("/flash/apps/demo/demo.so", "demo") == module);
    host_service_pins = 1;
    app_module_close(module); app_modules_process();
    assert(host_quiesce_calls && !instances->closing && module->nref == 1);
    host_service_pins = 0; app_module_close(module); app_modules_process(); assert_gone(owner);
    puts("PASS default RELEASE quiesces an idle service pin after the last page leaves");
}

static void invalidate_in_init(void) { register_cold(); available = false; }
static void init_abort_lifecycle(void)
{
    fixture(); host_module_init = invalidate_in_init;
    assert(!app_module_open("/flash/apps/demo/demo.so", "demo"));
    assert(instances && instances->aborting && dlmodule_find("demo"));
    epd_memory_owner_t owner = instances->owner;
    app_modules_process(); assert_root(owner, false); assert(host_cold.closing);
    detach_cold(); app_modules_process(); assert_gone(owner);
    puts("PASS storage invalidated by module init retains executable until provider detach and root drain");
}

static void closed_input_drains_before_dlclose(void)
{
    fixture(); struct rt_dlmodule *module = app_module_open("/flash/apps/demo/demo.so", "demo"); assert(module);
    epd_memory_owner_t owner = instances->owner;
    host_input_pin = epd_memory_scope_create_trusted(owner, EPD_MEMORY_SCOPE_SERVICE);
    assert(host_input_pin != EPD_MEMORY_OWNER_INVALID);
    /* An input-owned child reference exists until the owner tree is closed. */
    app_module_close(module); app_modules_process();
    assert(host_input_cleanups == 1 && !host_input_pin);
    assert_gone(owner);
    puts("PASS final close cleans input references without waiting for a later input-loop iteration");
}

#include "error_cases.inc"

static void uninstall_save_barrier(void)
{
    fixture();
    volume_ready[STORAGE_FLASH] = true;
    host_page_open = true;
    host_flush_ready = false;
    host_exit_result = RT_EOK;
    host_sem_releases = host_exit_calls = host_flush_calls = 0;
    uint32_t ticket;
    assert(app_installer_start_uninstall("demo", STORAGE_FLASH, &ticket));
    app_installer_process();
    assert(needs_prepare && host_flush_calls == 1);
    assert(host_page_open && !host_exit_calls && !host_sem_releases && !host_quiesce_calls);

    host_tick += PREPARE_TIMEOUT_MS;
    app_installer_process();
    assert(!needs_prepare && !prepare_ok && host_sem_releases == 1);
    assert(host_page_open && !host_exit_calls && !host_quiesce_calls);
    worker(NULL);
    assert(result.state == APP_JOB_FAILED && result.message == APP_JOB_MESSAGE_BUSY);
    assert(host_find("/flash/apps/demo/app.json"));

    assert(app_installer_start_uninstall("demo", STORAGE_FLASH, &ticket));
    host_flush_ready = true;
    host_exit_result = -RT_EINVAL;
    app_installer_process();
    assert(needs_prepare && host_page_open && host_exit_calls == 1);
    assert(!host_quiesce_calls && host_sem_releases == 1);
    host_exit_result = RT_EOK;
    app_installer_process();
    assert(!needs_prepare && prepare_ok && !host_page_open);
    assert(host_exit_calls == 2 && host_quiesce_calls == 1 && host_sem_releases == 2);
    worker(NULL);
    assert(result.state == APP_JOB_DONE && result.message == APP_JOB_MESSAGE_REMOVED);

    fixture();
    volume_ready[STORAGE_FLASH] = true;
    host_page_open = false;
    host_flush_ready = false;
    host_flush_calls = 0;
    assert(app_installer_start_uninstall("demo", STORAGE_FLASH, &ticket));
    app_installer_process();
    assert(!needs_prepare && prepare_ok && !host_flush_calls && host_quiesce_calls == 1);
    worker(NULL);
    assert(result.state == APP_JOB_DONE && result.message == APP_JOB_MESSAGE_REMOVED);
    host_tick = 0;
    puts("PASS uninstall flushes active pages, preserves them on timeout, and retries a rejected exit");
}

static void uninstall_commit(void)
{
    fixture();
    job_volume = STORAGE_FLASH;
    job_session = session;
    strcpy(job_id, "demo");
    remove_job = prepare_ok = true;
    result.state = APP_JOB_RUNNING;
    const install_paths_t *p = &paths[job_volume];

    host_fail_io = "rename";
    host_fail_path = "/flash/apps/demo";
    worker(NULL);
    assert(result.state == APP_JOB_FAILED && result.message == APP_JOB_MESSAGE_REMOVE_FAILED);
    assert(host_find("/flash/apps/demo/app.json") && !host_find(p->removing));
    assert(!host_revocations && !host_removals);

    host_reminder_error = true;
    result.state = APP_JOB_RUNNING;
    worker(NULL);
    assert(result.state == APP_JOB_DONE && result.message == APP_JOB_MESSAGE_COMMITTED_CLEANUP_FAILED);
    assert(!host_find("/flash/apps/demo/app.json") && host_find(p->removing) && host_find(p->journal));
    assert(host_revocations == 1 && host_removals == 1);

    host_reminder_error = false;
    assert(discard_interrupted(p));
    assert(!host_find(p->removing) && !host_find(p->journal));
    assert(host_revocations == 2 && host_removals == 2);
    remove_job = false;
    puts("PASS uninstall keeps grants/reminders on failed rename and resumes committed cleanup");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    cached_validation(); generations(); resident_validation(); extracted_validation(); chunked_copy();
    install_archive(); install_preflight_keeps_page(); cross_volume_installation();
    install_worker(false, false); install_worker(true, false);
    install_worker(false, true); uninstall_save_barrier(); uninstall_commit();
    malformed_archive();
    strict_manifest(); content_volume();
    directory_and_lru(); resident_pins(); concurrent_cache();
    real_entry_locks(); volume_identity(); concurrent_modules(); unresolved_imports();
    pending_state_reopen(); pending_tasks_reopen(); pressure_cold_lifecycle();
    release_idle_services(); init_abort_lifecycle(); closed_input_drains_before_dlclose();
    package_error_cases();
    assert(lock_depth == 0);
    puts("All app installation performance regressions passed.");
    return 0;
}
