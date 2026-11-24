#include "host.h"
#include "music_service.h"
#include "music_backend_sdk.h"
#include "../../src/ui/platform/epd_system.h"
#include "../../src/ui/platform/epd_sound.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == MUSIC_MODEL_OK)
typedef struct { unsigned char *data; size_t size; uint32_t magic, version, crc; } disk_record_t;
static disk_record_t disk[2];
static unsigned saves[2], validations, stats, allocations;
static bool fail_save, fail_alloc, fail_path, file_changed, file_missing, fail_open;
static void (*during_save)(void);
static void check_save_context(void);

static unsigned slot(const char *path) { return strstr(path, "progress.bin") ? 1u : 0u; }
uint32_t storage_crc32(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
bool storage_app_path(char *out, size_t size, const char *id, int area, const char *relative)
{
    (void)id; (void)area;
    snprintf(out, size, "/flash/data/music/%s", relative);
    return !fail_path;
}
bool storage_mkdirs(const char *path) { (void)path; return true; }
bool storage_file_recover(const char *path) { (void)path; return true; }
int music_host_stat(const char *path, struct music_host_stat *out)
{
    disk_record_t *r = &disk[slot(path)];
    if (!r->data) { errno = ENOENT; return -1; }
    out->st_size = (int64_t)r->size + 16;
    return 0;
}
bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size)
{
    check_save_context();
    unsigned index = slot(path);
    ++saves[index];
    /* The storage API commits a replacement or preserves its previous record. */
    if (fail_save) return false;
    unsigned char *next = malloc(size);
    CHECK(next);
    memcpy(next, data, size);
    free(disk[index].data);
    disk[index] = (disk_record_t){next, size, magic, version, storage_crc32(data, size)};
    if (during_save) {
        void (*hook)(void) = during_save;
        during_save = NULL;
        hook();
    }
    return true;
}
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size)
{
    disk_record_t *r = &disk[slot(path)];
    if (!r->data || r->magic != magic || r->version != version || r->size != size ||
        r->crc != storage_crc32(r->data, r->size)) return false;
    memcpy(data, r->data, size);
    return true;
}
bool host_queue_valid(const music_queue_t *q) { ++validations; return music_queue_valid(q); }
static void *allocate(size_t n) { ++allocations; return fail_alloc ? NULL : malloc(n); }
static void *resize(void *p, size_t n) { ++allocations; return fail_alloc ? NULL : realloc(p, n); }
music_model_result_t music_library_stat(const char *path, bool signature, music_file_stamp_t *out)
{
    (void)path; ++stats;
    if (file_missing) return MUSIC_MODEL_IO;
    *out = (music_file_stamp_t){100, file_changed ? 2 : 1, 123, true, signature};
    return MUSIC_MODEL_OK;
}
#include "library.inc"

/* Service dependencies: playback, directory scanning and UI publication. */
typedef uint32_t rt_tick_t;
#define RT_TICK_PER_SECOND 1000u
static music_store_state_t store;
static music_backend_t *backend;
static music_snapshot_t working;
static music_library_t library;
static music_library_scan_t *scan;
static music_metadata_t metadata;
static bool dirty, flush_ready, session_active, play_intent, automatic_chain, media_published, timer_end;
static unsigned backend_opens;
static struct {
    uint64_t session;
    uint32_t revision;
    bool pending;
} playback_error;
static bool scan_limit;
static music_model_result_t library_read_result = MUSIC_MODEL_IO;
static rt_tick_t last_save, timer_start, timer_ticks, ticks;
static void *worker;
static music_backend_snapshot_t audio;
static unsigned lock_depth;
static void (*after_unlock)(void);
static void lock(void) { CHECK(!lock_depth); ++lock_depth; }
static void unlock(void) {
    CHECK(lock_depth == 1); --lock_depth;
    if (after_unlock) { void (*hook)(void) = after_unlock; after_unlock = NULL; hook(); }
}
static rt_tick_t rt_tick_get(void) { return ticks; }
static void list_publish(void) {}
static void publish(void) {}
static void stop_scan(void) { scan = NULL; }
static bool path_valid(const char *path) { (void)path; return true; }
static void backend_result(void) { working.state = MUSIC_ERROR; }
bool epd_media_publish(epd_service_t *service, const epd_media_t *media)
{ (void)service; (void)media; return true; }
bool epd_sound_set_volume(epd_sound_channel_t kind, unsigned volume) { (void)kind; (void)volume; return true; }
bool epd_sound_read(epd_sound_snapshot_t *out)
{ memset(out, 0, sizeof(*out)); out->available = out->output_ready = true; return true; }
bool music_cover_request(uint32_t id, const char *path, const char *cover)
{ (void)id; (void)path; (void)cover; return true; }
void music_backend_snapshot(music_backend_t *b, music_backend_snapshot_t *out) { (void)b; *out = audio; }
int music_backend_open(music_backend_t *b, const char *path)
{
    (void)b; (void)path; ++backend_opens; audio.position_ms = 0; audio.can_seek = true;
    audio.state = MUSIC_BACKEND_PREPARED; audio.want_play = false; ++audio.session_id;
    return fail_open ? -1 : 0;
}
int music_backend_close(music_backend_t *b) { (void)b; audio.state = MUSIC_BACKEND_IDLE; audio.want_play = false; return 0; }
int music_backend_play(music_backend_t *b) { (void)b; audio.state = MUSIC_BACKEND_PLAYING; audio.want_play = true; return 0; }
int music_backend_pause(music_backend_t *b) { (void)b; audio.state = MUSIC_BACKEND_PAUSED; audio.want_play = false; return 0; }
int music_backend_seek(music_backend_t *b, uint64_t pos) { (void)b; audio.position_ms = pos; return 0; }
void music_metadata_free(music_metadata_t *m) { memset(m, 0, sizeof(*m)); }
music_model_result_t music_library_metadata(const char *path, music_metadata_t *m)
{ (void)path; (void)m; return MUSIC_MODEL_NOT_FOUND; }
music_model_result_t music_library_scan_begin(const char *path, music_library_scan_t **out)
{ (void)path; *out = scan_limit ? (music_library_scan_t *)3 : NULL; return MUSIC_MODEL_OK; }
music_format_t music_library_extension(const char *path)
{ return strstr(path, ".m3u8") ? MUSIC_FORMAT_M3U8 : MUSIC_FORMAT_FLAC; }
music_model_result_t music_library_read(const char *path, music_library_t *out)
{ (void)path; (void)out; return library_read_result; }
music_model_result_t music_library_import_m3u8(const char *path, music_queue_t *out)
{ (void)path; return music_queue_append(out, "/sd/import.flac", NULL); }
music_model_result_t music_library_export_m3u8(const char *path, const music_queue_t *q)
{ (void)path; (void)q; return MUSIC_MODEL_OK; }
#include "lifecycle_fixture.h"
static void check_save_context(void)
{ if(test_cleanup_holds) CHECK(test_cleanup_context==2); }
#include "service.inc"
#include "dispatch_save.inc"

static void reset(void)
{
    music_store_free(&store);
    for (unsigned i = 0; i < 2; ++i) { free(disk[i].data); memset(&disk[i], 0, sizeof(disk[i])); }
    memset(saves, 0, sizeof(saves));
    memset(&working, 0, sizeof(working)); memset(&audio, 0, sizeof(audio));
    validations = stats = allocations = 0;
    fail_save = fail_alloc = fail_path = file_changed = file_missing = fail_open = false;
    dirty = session_active = play_intent = automatic_chain = false;
    backend_opens = 0;
    memset(&playback_error, 0, sizeof(playback_error));
    backend = (music_backend_t *)&audio;
    audio.format = MUSIC_BACKEND_FORMAT_FLAC;
    music_store_init(&store);
}
static void seed(size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        char path[64]; snprintf(path, sizeof(path), "/sd/song%zu.flac", i);
        OK(music_queue_append(&store.queue, path, NULL));
        OK(music_library_stat(path, false, &store.queue.entries[i].stamp));
    }
    if (count) OK(music_queue_select(&store.queue, store.queue.entries[0].id));
    store.directory = music_model_strdup("/sd");
    OK(music_store_save(&store));
}
static void restored_position(uint64_t expected)
{
    music_store_state_t loaded; music_store_init(&loaded);
    OK(music_store_load(&loaded));
    CHECK(loaded.queue.position_ms == expected);
    music_store_free(&loaded);
}
static void command(music_command_kind_t kind, uint32_t id, int32_t value, const char *path)
{
    music_command_t c = {0}; c.kind = kind; c.id = id; c.value = value;
    snprintf(c.path, sizeof(c.path), "%s", path ? path : "");
    execute(&c);
}

static void progress_cost(void)
{
    reset(); seed(2000);
    unsigned v = validations, a = allocations, s = stats, full = saves[0];
    uint32_t crc = disk[0].crc;
    fail_alloc = true;
    session_active = true;
    for (unsigned i = 1; i <= 100; ++i) {
        ticks += 30 * RT_TICK_PER_SECOND;
        audio.position_ms = (uint64_t)i * 30000;
        CHECK(save()); CHECK(!dirty);
    }
    CHECK(validations == v && allocations == a && stats == s);
    CHECK(saves[0] == full && saves[1] == 100 && disk[1].size == 24 && disk[0].crc == crc);
    fail_alloc = false;
    restored_position(3000000);
    puts("PASS progress: 2000 tracks, 100 saves, 24 bytes, zero validation/stat/allocation");
}

static void binding_and_legacy(void)
{
    reset(); seed(3);
    store.queue.position_ms = 30000; OK(music_store_save(&store));
    OK(music_queue_move(&store.queue, store.queue.entries[2].id, 0));
    store.queue.position_ms = 7; music_store_mark_changed(&store); OK(music_store_save(&store));
    restored_position(7); /* Old progress has the same current ID but an old version. */
    reset(); seed(3);
    store.queue.position_ms = 40000; OK(music_store_save(&store));
    music_store_free(&store); music_store_init(&store);
    seed(2); /* Reused version and IDs require the snapshot CRC as well. */
    restored_position(0);
    store.queue.position_ms = 51000; OK(music_store_save(&store));
    disk[1].data[12] ^= 1; disk[1].crc = storage_crc32(disk[1].data, disk[1].size);
    restored_position(0); /* Wrong selected ID, valid envelope CRC. */
    OK(music_store_save(&store));
    disk[1].data[20] ^= 1; restored_position(0); /* Corrupt envelope. */
    OK(music_store_save(&store));
    --disk[1].size; restored_position(0); /* Truncated progress. */
    ++disk[1].size;
    /* Convert the generated snapshot to the exact v1 payload layout. */
    memmove(disk[0].data, disk[0].data + 8, disk[0].size - 8);
    disk[0].size -= 8; disk[0].version = 1; disk[0].crc = storage_crc32(disk[0].data, disk[0].size);
    music_store_state_t legacy; music_store_init(&legacy); OK(music_store_load(&legacy));
    CHECK(legacy.queue.position_ms == 0 && legacy.snapshot_dirty && !legacy.snapshot_version);
    legacy.queue.position_ms = 9; OK(music_store_save(&legacy));
    CHECK(disk[0].version == MUSIC_STORE_VERSION); restored_position(9);
    music_store_free(&legacy);
    puts("PASS binding: stale version, reused IDs, wrong ID, corruption, truncation, v1 migration");
}

static void failure_and_revalidation(void)
{
    reset(); seed(3);
    store.queue.position_ms = 100; OK(music_store_save(&store));
    uint64_t version = store.snapshot_version;
    fail_save = true; store.queue.position_ms = 200; dirty = true;
    CHECK(!save() && dirty && working.result == MUSIC_ERR_SAVE);
    CHECK(store.snapshot_version == version); restored_position(100);
    OK(music_queue_append(&store.queue, "/sd/new.flac", NULL)); structure_changed();
    unsigned progress = saves[1];
    CHECK(!save() && dirty && store.snapshot_dirty && saves[1] == progress);
    CHECK(store.snapshot_version == version); restored_position(100);
    fail_save = false; CHECK(save() && !dirty && !store.snapshot_dirty);
    CHECK(store.snapshot_version == version + 1); restored_position(200);
    store.queue.position_ms = 300; OK(music_store_save(&store));
    file_changed = true; restored_position(0); file_changed = false;
    file_missing = true;
    music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
    CHECK(loaded.queue.entries[0].unavailable && loaded.queue.position_ms == 300 && loaded.snapshot_dirty);
    music_store_free(&loaded); file_missing = false;
    fail_path = true; CHECK(music_store_save(&store) == MUSIC_MODEL_IO); fail_path = false;
    music_store_mark_changed(&store); fail_alloc = true;
    CHECK(music_store_save(&store) == MUSIC_MODEL_MEMORY && store.snapshot_dirty); fail_alloc = false;
    music_store_init(&loaded); OK(music_queue_append(&loaded.queue, "/sd/keep.flac", NULL));
    disk[0].data[0] ^= 1;
    CHECK(music_store_load(&loaded) == MUSIC_MODEL_FORMAT);
    CHECK(loaded.queue.count == 1 && !strcmp(loaded.queue.entries[0].path, "/sd/keep.flac"));
    music_store_free(&loaded);
    puts("PASS failure: atomic API rollback, dirty/version retention, allocation/path failure, changed/missing files");
}

static void shuffle_history(void)
{
    reset(); seed(12);
    OK(music_queue_set_mode(&store.queue, MUSIC_MODE_SHUFFLE));
    for (int i = 0; i < 5; ++i) OK(music_queue_next(&store.queue, false));
    OK(music_queue_previous(&store.queue));
    OK(music_favorites_set(&store.favorites, "/sd/song3.flac", true));
    structure_changed(); CHECK(save());
    store.queue.position_ms = 1234567890123ULL; OK(music_store_save(&store));
    music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
    CHECK(loaded.queue.position_ms == store.queue.position_ms && loaded.queue.mode == MUSIC_MODE_SHUFFLE);
    CHECK(loaded.queue.history_cursor == store.queue.history_cursor && loaded.queue.history_count == store.queue.history_count);
    CHECK(!memcmp(loaded.queue.history, store.queue.history, store.queue.history_count * sizeof(uint32_t)));
    CHECK(loaded.queue.shuffle_cursor == store.queue.shuffle_cursor && loaded.queue.shuffle_count == store.queue.shuffle_count);
    CHECK(!memcmp(loaded.queue.shuffle, store.queue.shuffle, store.queue.shuffle_count * sizeof(uint32_t)));
    CHECK(music_favorites_contains(&loaded.favorites, "/sd/song3.flac") && !strcmp(loaded.directory, "/sd"));
    for (int i = 0; i < 30; ++i) {
        OK(music_queue_next(&store.queue, false)); OK(music_queue_next(&loaded.queue, false));
        CHECK(store.queue.current_id == loaded.queue.current_id && store.queue.random_state == loaded.queue.random_state);
    }
    music_store_free(&loaded);
    puts("PASS restore: 64-bit position, shuffle rounds, history forward/back, favorites, directory");
}

static void service_commands(void)
{
    reset(); seed(4);
    unsigned n = saves[0];
    command(MUSIC_CMD_PLAY, 0, 0, NULL); audio.position_ms = 31000;
    command(MUSIC_CMD_PAUSE, 0, 0, NULL);
    command(MUSIC_CMD_SEEK, 0, 44, NULL);
    command(MUSIC_CMD_FLUSH, 0, 0, NULL);
    command(MUSIC_CMD_STOP, 0, 0, NULL);
    CHECK(saves[0] == n); restored_position(44000);
    command(MUSIC_CMD_MODE, 0, MUSIC_MODE_SHUFFLE, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_MODE, 0, MUSIC_MODE_SHUFFLE, NULL); CHECK(saves[0] == n);
    command(MUSIC_CMD_NEXT, 0, 0, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_PREVIOUS, 0, 0, NULL); CHECK(saves[0] == ++n);
    fail_open = true;
    command(MUSIC_CMD_OPEN, store.queue.entries[2].id, 0, NULL); CHECK(saves[0] == ++n);
    fail_open = false;
    command(MUSIC_CMD_APPEND, 0, 0, "/sd/appended.flac"); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_INSERT_NEXT, 0, 0, "/sd/inserted.flac"); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_MOVE, store.queue.entries[0].id, 1, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_MOVE, store.queue.entries[1].id, 1, NULL); CHECK(saves[0] == n);
    command(MUSIC_CMD_FAVORITE, 0, 1, "/sd/favorite.flac"); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_FAVORITE, 0, 1, "/sd/favorite.flac"); CHECK(saves[0] == n);
    command(MUSIC_CMD_FAVORITE, 0, 0, "/sd/favorite.flac"); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_REMOVE, store.queue.current_id, 0, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_IMPORT, 0, 0, "/sd/list.m3u8"); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_PLAY, 0, 0, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_MODE, 0, MUSIC_MODE_REPEAT_ONE, NULL); CHECK(saves[0] == ++n);
    advance(false, true); CHECK(saves[0] == n); restored_position(0);
    command(MUSIC_CMD_LIST, 0, 0, "/sd/other"); CHECK(store.snapshot_dirty && dirty);
    CHECK(save()); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_CLEAR, 0, 0, NULL); CHECK(saves[0] == ++n);
    command(MUSIC_CMD_CLEAR, 0, 0, NULL); CHECK(saves[0] == n);
    puts("PASS service: play/pause/seek/stop/flush, selection/open failure, structural commands, no-ops, repeat-one");
}

static void previous_at_queue_start(void)
{
    reset(); seed(2);
    command(MUSIC_CMD_MODE, 0, MUSIC_MODE_SEQUENCE, NULL);
    command(MUSIC_CMD_PLAY, 0, 0, NULL);
    uint32_t current = store.queue.current_id;
    uint64_t session_id = audio.session_id;
    audio.position_ms = 31000;
    command(MUSIC_CMD_PREVIOUS, 0, 0, NULL);
    CHECK(session_active && play_intent && audio.state == MUSIC_BACKEND_PLAYING);
    CHECK(store.queue.current_id == current && audio.session_id == session_id &&
          audio.position_ms == 31000);
    command(MUSIC_CMD_NEXT, 0, 0, NULL);
    CHECK(store.queue.current_id != current);
    command(MUSIC_CMD_PREVIOUS, 0, 0, NULL);
    CHECK(session_active && audio.want_play && store.queue.current_id == current);
    puts("PASS previous at sequence start preserves the current playback session");
}

static void failed_chain_and_replacement(void)
{
    reset(); seed(4); restored_position(0); /* A snapshot without progress is sufficient. */
    command(MUSIC_CMD_MODE, 0, MUSIC_MODE_REPEAT_ALL, NULL);
    command(MUSIC_CMD_PLAY, 0, 0, NULL);
    fail_open = true;
    unsigned n = saves[0];
    advance(false, true);
    CHECK(saves[0] == n + 1 && !dirty && !store.snapshot_dirty && store.queue.history_count == 1);
    CHECK(store.queue.history[0] == store.queue.entries[0].id);
    CHECK(music_queue_valid(&store.queue));
    music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
    CHECK(loaded.queue.history_count == 1 && loaded.queue.history[0] == store.queue.history[0] &&
          loaded.queue.current_id == store.queue.current_id);
    music_store_free(&loaded);
    fail_open = false;
    command(MUSIC_CMD_FAVORITE, 0, 1, "/sd/favorite.flac");
    working.list = MUSIC_LIST_FAVORITES;
    command(MUSIC_CMD_OPEN, 0, 0, "/sd/favorite.flac");
    CHECK(store.queue.count == 1 && !strcmp(store.queue.entries[0].path, "/sd/favorite.flac"));
    CHECK(!store.snapshot_dirty && !dirty);
    command(MUSIC_CMD_OPEN, 0, 0, "/sd/playlist.m3u8");
    CHECK(store.queue.count == 1 && !strcmp(store.queue.entries[0].path, "/sd/import.flac"));
    CHECK(music_favorites_contains(&store.favorites, "/sd/favorite.flac"));
    store.snapshot_version = UINT64_MAX; music_store_mark_changed(&store);
    CHECK(music_store_save(&store) == MUSIC_MODEL_RANGE && store.snapshot_dirty);
    puts("PASS failure chain/replacement: removed failed history, favorites/playlist queues, version overflow");
}

#include "lifecycle_cases.h"
#include "release_cases.h"
#include "bounds_cases.h"

int main(void)
{
    music_model_allocator_t allocator = {allocate, resize, free};
    music_model_set_allocator(&allocator);
    progress_cost(); binding_and_legacy(); failure_and_revalidation(); shuffle_history(); service_commands();
    previous_at_queue_start(); failed_chain_and_replacement();
    startup_ui_yields(); stopped_release(); protected_lifecycle(); pending_work_and_save(); retirement_races();
    worker_exit_integration();
    never_played_worker_exit(); unused_pending_work(); unsubmitted_play_race();
    reset(); music_store_free(&store);
    directory_limit_keeps_playback(); history_bounds(); legacy_history_bounds();
    release_integration();
    flush_progress_interleavings();
    puts("music progress/lifecycle: all 26 existing suites passed; 8 flush interleaving suites and successful-save result preservation passed");
    return 0;
}
