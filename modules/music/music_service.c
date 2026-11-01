#include "app_service.h"
#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "platform/epd_system.h"
#include "music_service.h"
#include "music_backend_sdk.h"
#include "music_cover.h"
#include "music_store.h"
#include "platform/epd_sound.h"
#include <audio_server.h>
#include <dlmodule.h>
#include <dfs_posix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct command_node {
    struct command_node *next;
    music_command_t command;
} command_node_t;

static struct rt_mutex guard;
static bool initialized, accepting;
static epd_service_t *worker;
static command_node_t *commands, *commands_tail;
static music_backend_t *backend;
static music_store_state_t store;
static music_library_t library;
static music_library_scan_t *scan;
static music_metadata_t metadata;
static music_snapshot_t published, working;
static uint32_t card_session;
static bool dirty, session_active, timer_end, automatic_chain;
static bool play_intent, has_pending_media;
static epd_media_command_t pending_media;
static epd_media_t previous_media;
static uint64_t previous_item;
static bool media_published, previous_quiet, media_dirty;
static rt_tick_t last_save, timer_start, timer_ticks;
static uint32_t flush_requested, flush_completed;
static bool flush_ok, flush_ready, settled;
static bool retire_ready;
static bool ui_active, cover_refresh;

extern const epd_background_t epd_app_background;

static void lock(void) { rt_mutex_take(&guard, RT_WAITING_FOREVER); }
static void unlock(void) { rt_mutex_release(&guard); }
static void text_copy(char *out, size_t capacity, const char *value)
{
    snprintf(out, capacity, "%s", value ? value : "");
}
static const char *basename_of(const char *path)
{
    const char *name = strrchr(path, '/');
    return name ? name + 1 : path;
}
static void result(music_result_t code, int detail, const char *path)
{
    working.result = code;
    working.detail = detail;
    text_copy(working.result_path, sizeof(working.result_path), path);
    ++working.result_revision;
}
static void model_result(music_model_result_t code, const char *path)
{
    if (code == MUSIC_MODEL_OK || code == MUSIC_MODEL_END) return;
    result(code == MUSIC_MODEL_MEMORY ? MUSIC_ERR_MEMORY :
           code == MUSIC_MODEL_LIMIT ? MUSIC_ERR_LIMIT :
           code == MUSIC_MODEL_FORMAT ? MUSIC_ERR_FORMAT : MUSIC_ERR_STORAGE, code, path);
}
static void publish(void)
{
    working.list_loading = scan != NULL;
    lock();
    bool changed = false;
    if (memcmp(&working, &published, sizeof(working))) {
        working.revision = published.revision + 1;
        published = working;
        changed = true;
    }
    if (changed && worker) epd_service_ui_request(worker, 0);
    unlock();
}

void music_service_init(void)
{
    int error = rt_mutex_init(&guard, "music", RT_IPC_FLAG_PRIO);
    initialized = error == RT_EOK;
    if (initialized) music_cover_init();
    else rt_kprintf("[music] start failed: stage=mutex_init rc=%d\n", error);
}

void music_service_cleanup(void)
{
    if (!initialized) return;
    music_cover_cleanup();
    while (commands) {
        command_node_t *next = commands->next;
        epd_app_free(commands);
        commands = next;
    }
    commands_tail = NULL;
    rt_mutex_detach(&guard);
    initialized = false;
}

bool music_start(void)
{
    if (!initialized) return false;
    lock();
    if (worker) {
        bool running = accepting && !epd_service_cancelled(worker);
        unlock();
        return running;
    }
    unlock();
    /* The catalog may have registered the service before its worker runs.
     * Reopening a page must not start a second worker or restore old playback. */
    if (app_service_running("music")) return true;
    void *module = dlmodule_find("music");
    if (!module) {
        rt_kprintf("[music] start failed: stage=module_lookup\n");
        return false;
    }
    epd_service_t *service = app_service_start("music", &epd_app_background, module);
    if (!service) return false;
    lock();
    worker = service;
    accepting = !epd_service_cancelled(service);
    unlock();
    return accepting;
}

void music_ui_active(bool active)
{
    if (!initialized) return;
    lock();
    if (ui_active != active) {
        ui_active = active;
        if (active) cover_refresh = true;
        if (worker) epd_service_wake(worker);
    }
    if (!active && worker) epd_service_ui_request(worker, 0);
    unlock();
}

static void trim_caches(void)
{
    lock();
    bool active = ui_active, refresh = active && cover_refresh;
    bool trim = !active && !commands && !has_pending_media;
    if (refresh && session_active && !epd_service_sleep_acquire(worker)) refresh = false;
    if (refresh) cover_refresh = false;
    unlock();
    if (refresh && session_active)
        music_cover_request(working.current_id, metadata.path, metadata.cover_path);
    if (!trim) return;
    if (!scan) music_library_free(&library);
    music_cover_trim();
}

bool music_submit(const music_command_t *command)
{
    if (!initialized || !command ||
        !memchr(command->path, 0, sizeof(command->path)) ||
        !memchr(command->filter, 0, sizeof(command->filter))) return false;
    command_node_t *node = epd_app_alloc(sizeof(*node), EPD_APP_PSRAM);
    if (!node) return false;
    node->command = *command;
    node->next = NULL;
    lock();
    if (!worker || !accepting || epd_service_cancelled(worker) ||
        !epd_service_sleep_acquire(worker)) {
        unlock();
        epd_app_free(node);
        return false;
    }
    settled = false;
    retire_ready = false;
    switch (command->kind) {
    case MUSIC_CMD_OPEN: case MUSIC_CMD_PLAY: case MUSIC_CMD_PAUSE: case MUSIC_CMD_STOP:
    case MUSIC_CMD_PREVIOUS: case MUSIC_CMD_NEXT: case MUSIC_CMD_SEEK: case MUSIC_CMD_MODE:
    case MUSIC_CMD_APPEND: case MUSIC_CMD_INSERT_NEXT: case MUSIC_CMD_REMOVE: case MUSIC_CMD_MOVE:
    case MUSIC_CMD_CLEAR: case MUSIC_CMD_IMPORT: case MUSIC_CMD_TIMER:
        epd_media_note_intent(worker);
        media_dirty = true;
        break;
    default: break;
    }
    /* A newer queued seek supersedes an older queued seek, never a command
     * already executing in the SDK. Other commands keep their FIFO order. */
    if (command->kind == MUSIC_CMD_SEEK && commands_tail &&
        commands_tail->command.kind == MUSIC_CMD_SEEK) {
        commands_tail->command = *command;
        epd_app_free(node);
    } else {
        if (commands_tail) commands_tail->next = node;
        else commands = node;
        commands_tail = node;
    }
    if (command->kind == MUSIC_CMD_STOP && backend)
        music_backend_request_cancel(backend);
    epd_service_wake(worker);
    unlock();
    return true;
}

void music_snapshot_read(music_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (!initialized) { memset(snapshot, 0, sizeof(*snapshot)); return; }
    lock();
    *snapshot = published;
    unlock();
}

static bool take_command(music_command_t *out)
{
    lock();
    command_node_t *node = commands;
    if (node) {
        *out = node->command;
        commands = node->next;
        if (!commands) commands_tail = NULL;
    }
    unlock();
    epd_app_free(node);
    return node != NULL;
}

static bool path_valid(const char *path)
{
    return !epd_service_cancelled(worker) && storage_path_available(path) &&
           (strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)) ||
            storage_session_valid(STORAGE_SD, card_session));
}

static void stop_scan(void)
{
    music_library_scan_close(scan);
    scan = NULL;
}

static void list_row(size_t *matches, uint32_t id, const char *path,
                     const char *name, bool directory)
{
    const char *title = !strcmp(path, working.current_path) ? working.title : NULL;
    if (!music_library_matches(name, title, working.filter)) return;
    size_t index = (*matches)++;
    if (index < working.first || working.count == MUSIC_PAGE_ROWS) return;
    music_list_entry_t *entry = &working.entries[working.count++];
    entry->id = id;
    entry->directory = directory;
    entry->favorite = music_favorites_contains(&store.favorites, path);
    text_copy(entry->path, sizeof(entry->path), path);
    text_copy(entry->name, sizeof(entry->name), name);
}

static void list_publish(void)
{
    size_t total = 0;
    working.count = 0;
    memset(working.entries, 0, sizeof(working.entries));
    if (working.list == MUSIC_LIST_QUEUE) {
        for (size_t i = 0; i < store.queue.count; ++i) {
            const music_queue_entry_t *item = &store.queue.entries[i];
            list_row(&total, item->id, item->path, basename_of(item->path), false);
        }
    } else if (working.list == MUSIC_LIST_FAVORITES) {
        for (size_t i = 0; i < store.favorites.count; ++i) {
            const char *path = store.favorites.paths[i];
            list_row(&total, 0, path, basename_of(path), false);
        }
    } else {
        for (size_t i = 0; i < library.count; ++i) {
            const music_library_entry_t *item = &library.entries[i];
            if (working.list == MUSIC_LIST_PLAYLISTS && !item->directory &&
                item->format != MUSIC_FORMAT_M3U8) continue;
            list_row(&total, 0, item->path, item->name, item->directory);
        }
    }
    working.total = total;
    ++working.list_revision;
}

static void structure_changed(void)
{
    music_store_mark_changed(&store);
    dirty = true;
    lock(); flush_ready = false; unlock();
}

static void start_list(const music_command_t *command)
{
    stop_scan();
    working.list = command->list;
    working.first = command->first;
    text_copy(working.filter, sizeof(working.filter), command->filter);
    if (command->path[0]) text_copy(working.directory, sizeof(working.directory), command->path);
    if (working.list == MUSIC_LIST_QUEUE || working.list == MUSIC_LIST_FAVORITES) {
        list_publish();
        return;
    }
    if (working.list == MUSIC_LIST_PLAYLISTS && !command->path[0]) {
        if (!storage_app_path(working.directory, sizeof(working.directory), "music",
                              STORAGE_APP_DATA, "playlists") || !storage_mkdirs(working.directory)) {
            result(MUSIC_ERR_STORAGE, 0, working.directory);
            return;
        }
    }
    music_library_free(&library);
    list_publish();
    if (!path_valid(working.directory)) { result(MUSIC_ERR_STORAGE, 0, working.directory); return; }
    music_model_result_t code = music_library_scan_begin(working.directory, &scan);
    model_result(code, working.directory);
    if (code == MUSIC_MODEL_OK && working.list == MUSIC_LIST_FILES &&
        (!store.directory || strcmp(store.directory, working.directory))) {
        char *directory = music_model_strdup(working.directory);
        if (directory) {
            music_model_free(store.directory);
            store.directory = directory;
            structure_changed();
        } else result(MUSIC_ERR_MEMORY, 0, working.directory);
    }
}

static void capture_position(void)
{
    if (!session_active || !backend) return;
    music_backend_snapshot_t value;
    music_backend_snapshot(backend, &value);
    if (store.queue.position_ms != value.position_ms) {
        dirty = true;
        lock(); flush_ready = false; unlock();
    }
    store.queue.position_ms = value.position_ms;
}

static bool save(void)
{
    capture_position();
    if (!dirty) return true;
    music_model_result_t code = music_store_save(&store);
    if (code != MUSIC_MODEL_OK) { result(MUSIC_ERR_SAVE, code, ""); return false; }
    dirty = false;
    last_save = rt_tick_get();
    return true;
}

static void close_track(bool preserve)
{
    if (preserve) capture_position();
    if (backend) music_backend_close(backend);
    session_active = false;
    working.playing = false;
    working.can_seek = false;
    working.state = MUSIC_IDLE;
    epd_media_publish(worker, NULL);
    media_published = false;
}

static void clear_current(void)
{
    music_cover_request(0, NULL, NULL);
    working.current_id = 0;
    working.title[0] = working.current_path[0] = working.artist[0] = working.album[0] = 0;
    working.format[0] = 0;
    working.sample_rate = working.channels = working.bits_per_sample = 0;
    working.position_seconds = working.duration_seconds = 0;
    working.can_seek = false;
    working.duration_estimated = false;
}

static void backend_result(void)
{
    music_backend_snapshot_t value;
    music_backend_snapshot(backend, &value);
    result(value.error == MUSIC_BACKEND_NO_MEMORY ? MUSIC_ERR_MEMORY :
           value.error == MUSIC_BACKEND_IO ? MUSIC_ERR_STORAGE :
           value.error == MUSIC_BACKEND_UNSUPPORTED ? MUSIC_ERR_FORMAT :
           value.error == MUSIC_BACKEND_OUTPUT ? MUSIC_ERR_OUTPUT : MUSIC_ERR_DECODE,
           value.error_detail, working.current_path);
    working.state = MUSIC_ERROR;
    working.playing = false;
}

static bool controls_waiting(void)
{
    if (!has_pending_media) has_pending_media = epd_media_poll(worker, &pending_media);
    lock();
    bool pending = false;
    for (command_node_t *node = commands; node; node = node->next) {
        music_command_kind_t kind = node->command.kind;
        if (kind == MUSIC_CMD_PAUSE || kind == MUSIC_CMD_STOP || kind == MUSIC_CMD_OPEN ||
            kind == MUSIC_CMD_NEXT || kind == MUSIC_CMD_PREVIOUS || kind == MUSIC_CMD_CLEAR) {
            pending = true;
            break;
        }
    }
    unlock();
    return pending || has_pending_media || epd_service_cancelled(worker);
}

static bool open_current(bool play, bool restore_position)
{
    const music_queue_entry_t *item = music_queue_current(&store.queue);
    if (!item) return false;
    close_track(false);
    music_cover_request(0, NULL, NULL);
    text_copy(working.current_path, sizeof(working.current_path), item->path);
    text_copy(working.title, sizeof(working.title), basename_of(item->path));
    working.artist[0] = working.album[0] = 0;
    working.format[0] = 0;
    working.sample_rate = working.channels = working.bits_per_sample = 0;
    working.current_id = item->id;
    working.state = MUSIC_OPENING;
    publish();
    if (!path_valid(item->path)) { result(MUSIC_ERR_STORAGE, 0, item->path); return false; }
    music_file_stamp_t stamp;
    music_model_result_t code = music_library_stat(item->path, false, &stamp);
    if (code != MUSIC_MODEL_OK) { model_result(code, item->path); return false; }
    if (!music_library_stamp_equal(&stamp, &item->stamp)) {
        store.queue.position_ms = 0;
        structure_changed();
    }
    if (item->unavailable) structure_changed();
    /* The queue is service-owned; no UI pointer refers to this entry. */
    ((music_queue_entry_t *)item)->stamp = stamp;
    ((music_queue_entry_t *)item)->unavailable = false;
    music_metadata_free(&metadata);
    if (music_library_metadata(item->path, &metadata) == MUSIC_MODEL_OK) {
        if (metadata.title && *metadata.title) text_copy(working.title, sizeof(working.title), metadata.title);
        text_copy(working.artist, sizeof(working.artist), metadata.artist);
        text_copy(working.album, sizeof(working.album), metadata.album);
        text_copy(working.format, sizeof(working.format), metadata.format == MUSIC_FORMAT_MP3 ? "MP3" :
                  metadata.format == MUSIC_FORMAT_WAV ? "WAV" : metadata.format == MUSIC_FORMAT_FLAC ? "FLAC" : "");
        working.sample_rate = metadata.sample_rate;
        working.channels = metadata.channels;
        working.bits_per_sample = metadata.bits_per_sample;
    }
    if (music_backend_open(backend, item->path) < 0) { backend_result(); return false; }
    session_active = true;
    music_backend_snapshot_t prepared;
    music_backend_snapshot(backend, &prepared);
    /* The native controller requires a decoded frame before seek. */
    if (restore_position && prepared.format == MUSIC_BACKEND_FORMAT_FLAC &&
        store.queue.position_ms &&
        music_backend_seek(backend, store.queue.position_ms) < 0) {
        backend_result();
        close_track(false);
        working.state = MUSIC_ERROR;
        return false;
    }
    if (!path_valid(item->path)) {
        result(MUSIC_ERR_STORAGE, 0, item->path);
        close_track(false);
        working.state = MUSIC_ERROR;
        return false;
    }
    if (play && !controls_waiting() && music_backend_play(backend) < 0) {
        backend_result();
        close_track(false);
        working.state = MUSIC_ERROR;
        return false;
    }
    music_cover_request(item->id, metadata.path, metadata.cover_path);
    dirty = true;
    return true;
}

static music_model_result_t failed_next(void)
{
    size_t history_count = store.queue.history_count;
    music_model_result_t code = music_queue_failed_next(&store.queue);
    if (code == MUSIC_MODEL_OK || store.queue.history_count != history_count) structure_changed();
    return code;
}

static void advance(bool previous, bool natural)
{
    bool play = natural || play_intent;
    play_intent = play;
    capture_position();
    close_track(false);
    music_model_result_t code = previous ? music_queue_previous(&store.queue) :
                                         music_queue_next(&store.queue, natural);
    if (code != MUSIC_MODEL_OK) {
        automatic_chain = false;
        music_queue_attempt_end(&store.queue);
        if (natural && code == MUSIC_MODEL_END) {
            play_intent = false;
            store.queue.position_ms = 0;
            working.position_seconds = 0;
            dirty = true;
        }
        model_result(code, ""); save(); return;
    }
    /* Repeat-one at natural EOF only resets position; other advances update
     * the selected ID, shuffle cursor/order or playback history. */
    if (previous || !natural || store.queue.mode != MUSIC_MODE_REPEAT_ONE) structure_changed();
    else dirty = true;
    automatic_chain = natural;
    music_queue_attempt_begin(&store.queue);
    while (!open_current(play, true) && !controls_waiting()) {
        if (!natural || failed_next() != MUSIC_MODEL_OK) break;
    }
    if (!session_active || !automatic_chain) music_queue_attempt_end(&store.queue);
    if (!session_active) { automatic_chain = false; working.state = MUSIC_ERROR; }
    list_publish();
    save();
}

static void open_path(const music_command_t *command)
{
    play_intent = true;
    automatic_chain = false;
    music_queue_attempt_end(&store.queue);
    if (command->id) {
        capture_position();
        if (music_queue_select(&store.queue, command->id) == MUSIC_MODEL_OK) {
            structure_changed();
            open_current(true, true);
            save();
        }
        else result(MUSIC_ERR_PATH, 0, "");
        return;
    }
    if (!command->path[0] || !path_valid(command->path)) {
        result(MUSIC_ERR_PATH, 0, command->path);
        return;
    }
    music_queue_t next;
    music_queue_init(&next, rt_tick_get());
    music_model_result_t code = MUSIC_MODEL_OK;
    if (music_library_extension(command->path) == MUSIC_FORMAT_M3U8) {
        code = music_library_import_m3u8(command->path, &next);
    } else if (working.list == MUSIC_LIST_FAVORITES) {
        for (size_t i = 0; code == MUSIC_MODEL_OK && i < store.favorites.count; ++i)
            code = music_queue_append(&next, store.favorites.paths[i], NULL);
    } else {
        char parent[STORAGE_PATH_MAX];
        text_copy(parent, sizeof(parent), command->path);
        char *slash = strrchr(parent, '/');
        if (!slash) code = MUSIC_MODEL_INVALID;
        else {
            *slash = 0;
            music_library_t temporary = {0};
            code = music_library_read(parent, &temporary);
            for (size_t i = 0; code == MUSIC_MODEL_OK && i < temporary.count; ++i) {
                music_library_entry_t *entry = &temporary.entries[i];
                if (!entry->directory && entry->format != MUSIC_FORMAT_M3U8)
                    code = music_queue_append(&next, entry->path, NULL);
            }
            music_library_free(&temporary);
        }
    }
    if (code == MUSIC_MODEL_OK && !next.count) code = MUSIC_MODEL_NOT_FOUND;
    if (code == MUSIC_MODEL_OK) {
        uint32_t selected = next.entries[0].id;
        for (size_t i = 0; i < next.count; ++i)
            if (!strcmp(next.entries[i].path, command->path)) selected = next.entries[i].id;
        code = music_queue_set_mode(&next, store.queue.mode);
        if (code == MUSIC_MODEL_OK) code = music_queue_select(&next, selected);
    }
    if (code == MUSIC_MODEL_OK) {
        close_track(true);
        music_queue_free(&store.queue);
        store.queue = next;
        memset(&next, 0, sizeof(next));
        structure_changed();
        if (!open_current(true, true)) working.state = MUSIC_ERROR;
        save();
    } else model_result(code, command->path);
    music_queue_free(&next);
    list_publish();
}

static void execute(const music_command_t *command)
{
    if (command->kind != MUSIC_CMD_LIST) {
        lock(); flush_ready = false; unlock();
    }
    music_model_result_t code = MUSIC_MODEL_OK;
    uint32_t result_before = working.result_revision;
    bool persist = false;
    switch (command->kind) {
    case MUSIC_CMD_LIST: start_list(command); break;
    case MUSIC_CMD_OPEN: open_path(command); break;
    case MUSIC_CMD_PLAY:
        play_intent = true;
        if (!store.queue.current_id && store.queue.count) {
            code = music_queue_select(&store.queue, store.queue.entries[0].id);
            if (code == MUSIC_MODEL_OK) { structure_changed(); persist = true; }
        }
        if (code != MUSIC_MODEL_OK) break;
        if (!store.queue.current_id) { result(MUSIC_ERR_PATH, 0, ""); break; }
        if (!session_active) { if (!open_current(true, true)) working.state = MUSIC_ERROR; }
        else if (music_backend_play(backend) < 0) backend_result();
        break;
    case MUSIC_CMD_PAUSE:
        play_intent = false;
        if (session_active && music_backend_pause(backend) < 0) backend_result();
        persist = true;
        break;
    case MUSIC_CMD_STOP:
        play_intent = false;
        automatic_chain = false;
        music_queue_attempt_end(&store.queue);
        close_track(true); persist = true; break;
    case MUSIC_CMD_PREVIOUS: advance(true, false); break;
    case MUSIC_CMD_NEXT: advance(false, false); break;
    case MUSIC_CMD_SEEK: {
        if (command->value < 0 || (command->id && command->id != store.queue.current_id)) break;
        bool opened_for_seek = !session_active;
        if (opened_for_seek && !open_current(false, false)) break;
        music_backend_snapshot_t current;
        music_backend_snapshot(backend, &current);
        if (!current.can_seek) break;
        working.state = MUSIC_SEEKING;
        publish();
        if (music_backend_seek(backend, (uint64_t)command->value * 1000) < 0) {
            backend_result();
            if (opened_for_seek) {
                close_track(false);
                working.state = MUSIC_ERROR;
            }
        }
        persist = true;
        break;
    }
    case MUSIC_CMD_MODE: {
        music_mode_t old_mode = store.queue.mode;
        code = music_queue_set_mode(&store.queue, (music_mode_t)command->value);
        persist = code == MUSIC_MODEL_OK;
        if (persist && old_mode != store.queue.mode) structure_changed();
        break;
    }
    case MUSIC_CMD_APPEND: case MUSIC_CMD_INSERT_NEXT:
        if (!path_valid(command->path)) code = MUSIC_MODEL_IO;
        else code = command->kind == MUSIC_CMD_APPEND ?
            music_queue_append(&store.queue, command->path, NULL) :
            music_queue_insert_next(&store.queue, command->path, NULL);
        persist = code == MUSIC_MODEL_OK;
        if (persist) structure_changed();
        break;
    case MUSIC_CMD_REMOVE: {
        bool current = command->id == store.queue.current_id, play = play_intent;
        code = music_queue_remove(&store.queue, command->id);
        if (code == MUSIC_MODEL_OK && current) {
            close_track(false);
            if (store.queue.current_id) open_current(play, true);
            else { play_intent = false; clear_current(); }
        }
        persist = code == MUSIC_MODEL_OK;
        if (persist) structure_changed();
        break;
    }
    case MUSIC_CMD_MOVE: {
        bool moved = command->value >= 0 && (size_t)command->value < store.queue.count &&
                     store.queue.entries[command->value].id != command->id;
        code = music_queue_move(&store.queue, command->id, (size_t)command->value);
        persist = code == MUSIC_MODEL_OK;
        if (persist && moved) structure_changed();
        break;
    }
    case MUSIC_CMD_CLEAR:
        play_intent = false;
        close_track(false);
        if (store.queue.count) structure_changed();
        music_queue_clear(&store.queue);
        clear_current();
        persist = true;
        break;
    case MUSIC_CMD_FAVORITE: {
        size_t old_count = store.favorites.count;
        code = music_favorites_set(&store.favorites, command->path, command->value != 0);
        persist = code == MUSIC_MODEL_OK;
        if (persist && old_count != store.favorites.count) structure_changed();
        break;
    }
    case MUSIC_CMD_TIMER:
        timer_end = command->value == -1;
        timer_ticks = command->value > 0 && command->value <= 60 ?
                      (rt_tick_t)command->value * 60u * RT_TICK_PER_SECOND : 0;
        timer_start = rt_tick_get();
        working.timer_minutes = timer_end ? (unsigned)-1 : (unsigned)(timer_ticks ? command->value : 0);
        break;
    case MUSIC_CMD_VOLUME:
        if (command->value >= 0 && command->value <= 100 &&
            !epd_sound_set_volume(EPD_SOUND_MEDIA, (unsigned)command->value))
            result(MUSIC_ERR_SAVE, 0, "");
        break;
    case MUSIC_CMD_IMPORT: {
        music_queue_t imported;
        music_queue_init(&imported, rt_tick_get());
        code = music_library_import_m3u8(command->path, &imported);
        if (code == MUSIC_MODEL_OK) {
            close_track(true);
            play_intent = automatic_chain = false;
            music_queue_free(&store.queue);
            store.queue = imported;
            memset(&imported, 0, sizeof(imported));
            clear_current();
            structure_changed();
            persist = true;
        }
        music_queue_free(&imported);
        break;
    }
    case MUSIC_CMD_SAVE_PLAYLIST: {
        char path[STORAGE_PATH_MAX], directory[STORAGE_PATH_MAX];
        if (!command->path[0] || strchr(command->path, '/') || strchr(command->path, '\\') ||
            strchr(command->path, ':') || !strcmp(command->path, ".") || !strcmp(command->path, "..") ||
            !storage_app_path(directory, sizeof(directory), "music", STORAGE_APP_DATA, "playlists") ||
            !storage_mkdirs(directory)) code = MUSIC_MODEL_INVALID;
        else {
            int size = snprintf(path, sizeof(path), "%s/%s%s", directory, command->path,
                                music_library_extension(command->path) == MUSIC_FORMAT_M3U8 ? "" : ".m3u8");
            code = size < 0 || (size_t)size >= sizeof(path) ? MUSIC_MODEL_RANGE :
                   music_library_export_m3u8(path, &store.queue);
            if (code == MUSIC_MODEL_OK) result(MUSIC_OK, 0, path);
        }
        break;
    }
    case MUSIC_CMD_FLUSH: persist = true; break;
    }
    model_result(code, command->path);
    if (persist) { dirty = true; save(); list_publish(); }
    if (command->kind != MUSIC_CMD_LIST && working.result_revision == result_before)
        result(MUSIC_OK, 0, "");
}

static void wake(void *context)
{
    epd_service_wake(context);
    epd_service_ui_request(context, 0);
}

static void update_track(void)
{
    working.mode = store.queue.mode;
    epd_sound_snapshot_t sound = {0};
    working.volume_available = epd_sound_read(&sound) && sound.available && sound.output_ready;
    working.volume = sound.volume[EPD_SOUND_MEDIA];
    if (!session_active) {
        lock();
        if (!commands && !has_pending_media) epd_media_intent_settled(worker);
        unlock();
        return;
    }
    music_backend_snapshot_t value;
    music_backend_snapshot(backend, &value);
    if (value.position_ms != store.queue.position_ms) {
        lock(); flush_ready = false; unlock();
    }
    working.position_seconds = (uint32_t)(value.position_ms / 1000);
    working.duration_seconds = (uint32_t)(value.duration_ms / 1000);
    working.duration_estimated = value.duration_estimated;
    if (metadata.duration_ms && !metadata.duration_estimated) {
        working.duration_seconds = (uint32_t)(metadata.duration_ms / 1000);
        working.duration_estimated = false;
    }
    working.playing = value.want_play;
    working.can_seek = value.can_seek;
    switch (value.state) {
    case MUSIC_BACKEND_PLAYING: working.state = MUSIC_PLAYING; break;
    case MUSIC_BACKEND_DRAINING: working.state = MUSIC_DRAINING; break;
    case MUSIC_BACKEND_SUSPENDED: working.state = MUSIC_SUSPENDED; break;
    case MUSIC_BACKEND_ERROR:
        backend_result();
        close_track(true);
        while (automatic_chain && play_intent && !controls_waiting() &&
               failed_next() == MUSIC_MODEL_OK) {
            if (open_current(play_intent, true)) { list_publish(); save(); return; }
        }
        automatic_chain = false;
        music_queue_attempt_end(&store.queue);
        working.state = MUSIC_ERROR;
        save();
        return;
    case MUSIC_BACKEND_ENDED:
        if (timer_end) { timer_end = false; working.timer_minutes = 0; close_track(true); save(); }
        else advance(false, true);
        return;
    default: working.state = MUSIC_PAUSED; working.playing = false; break;
    }
    epd_media_t media = {0};
    text_copy(media.title, sizeof(media.title), working.title);
    text_copy(media.artist, sizeof(media.artist), working.artist);
    media.audio_type = AUDIO_TYPE_LOCAL_MUSIC;
    media.playing = working.playing && value.state != MUSIC_BACKEND_SUSPENDED;
    media.capabilities = EPD_MEDIA_CAN_STOP |
                         (working.playing ? EPD_MEDIA_CAN_PAUSE : EPD_MEDIA_CAN_PLAY) |
                         (store.queue.count > 1 ? EPD_MEDIA_CAN_PREVIOUS | EPD_MEDIA_CAN_NEXT : 0);
    bool quiet = !value.want_play && (value.state == MUSIC_BACKEND_PAUSED ||
                                      value.state == MUSIC_BACKEND_PREPARED);
    lock();
    bool refresh = media_dirty;
    media_dirty = false;
    if (commands) quiet = false;
    if (refresh || !media_published || previous_quiet != quiet ||
        previous_item != value.session_id || memcmp(&previous_media, &media, sizeof(media))) {
        if (epd_media_publish_state(worker, &media, value.session_id, quiet)) {
            previous_media = media;
            previous_quiet = quiet;
            previous_item = value.session_id;
            media_published = true;
        }
    }
    if (!commands && !has_pending_media) epd_media_intent_settled(worker);
    unlock();
}

/* Only the worker publishes a saved, quiescent state. Taking new work or a UI
 * retirement commitment invalidates it under the same command-queue lock. */
static bool begin_work(void)
{
    lock();
    bool active = accepting;
    settled = retire_ready = false;
    unlock();
    return active;
}

static rt_int32_t settle(epd_service_t *service)
{
    lock();
    bool idle = !commands && !has_pending_media && !scan && !music_cover_busy() &&
                !dirty && !working.playing && working.state != MUSIC_DRAINING &&
                flush_requested == flush_completed;
    settled = idle;
    /* A restored queue is saved data, not an active playback session. */
    retire_ready = idle && !session_active && !play_intent &&
                   working.state == MUSIC_IDLE;
    if (retire_ready && !ui_active) epd_service_ui_request(service, 0);
    if (idle) epd_service_sleep_release(service);
    rt_int32_t delay = idle && !timer_ticks ? RT_WAITING_FOREVER :
                       scan || commands || has_pending_media ? 1 :
                       (rt_int32_t)rt_tick_from_millisecond(100);
    unlock();
    return delay;
}

static void run(epd_service_t *service)
{
    if (!initialized) return;
    while (!epd_service_cancelled(service) && !epd_service_sleep_acquire(service))
        epd_service_wait(service, RT_WAITING_FOREVER);
    if (epd_service_cancelled(service)) {
        lock();
        worker = NULL;
        accepting = false;
        while (commands) { command_node_t *next = commands->next; epd_app_free(commands); commands = next; }
        commands_tail = NULL;
        unlock();
        return;
    }
    music_cover_set_wake(wake, service);
    lock();
    worker = service;
    accepting = true;
    flush_requested = flush_completed = 0;
    flush_ok = flush_ready = settled = false;
    retire_ready = false;
    unlock();
    dirty = session_active = timer_end = automatic_chain = media_published = false;
    play_intent = has_pending_media = false;
    timer_ticks = 0;
    music_store_init(&store);
    music_model_result_t loaded = music_store_load(&store);
    dirty = loaded == MUSIC_MODEL_OK && store.snapshot_dirty;
    card_session = storage_card_session();
    music_backend_t *created = music_backend_create(wake, service);
    lock(); backend = created; unlock();
    memset(&working, 0, sizeof(working));
    if (loaded != MUSIC_MODEL_OK && loaded != MUSIC_MODEL_NOT_FOUND) model_result(loaded, "");
    if (!backend) {
        rt_kprintf("[music] start failed: stage=backend_create app_psram_free=%lu\n",
                   (unsigned long)epd_app_psram_available());
        result(MUSIC_ERR_MEMORY, 0, "");
    }
    working.ready = backend != NULL;
    working.mode = store.queue.mode;
    const music_queue_entry_t *item = music_queue_current(&store.queue);
    if (item) {
        working.current_id = item->id;
        working.position_seconds = (uint32_t)(store.queue.position_ms / 1000);
        text_copy(working.current_path, sizeof(working.current_path), item->path);
        text_copy(working.title, sizeof(working.title), basename_of(item->path));
    }
    text_copy(working.directory, sizeof(working.directory), store.directory && *store.directory ?
              store.directory : (storage_available(STORAGE_SD) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT));
    if (!store.directory || !*store.directory) {
        char directory[STORAGE_PATH_MAX];
        struct stat info;
        int size = snprintf(directory, sizeof(directory), "%s/Music", working.directory);
        if (size > 0 && (size_t)size < sizeof(directory) && stat(directory, &info) == 0 &&
            S_ISDIR(info.st_mode)) text_copy(working.directory, sizeof(working.directory), directory);
    }
    last_save = rt_tick_get();
    music_command_t command = {.kind = MUSIC_CMD_LIST, .list = MUSIC_LIST_FILES};
    start_list(&command);
    publish();
    while (backend && !epd_service_cancelled(service)) {
        if (!begin_work()) {
            epd_service_wait(service, RT_WAITING_FOREVER);
            continue;
        }
        bool deferred = false;
        for (unsigned batch = 0; batch < 4 && !epd_service_cancelled(service); ++batch) {
            epd_media_command_t media = pending_media;
            if (has_pending_media || epd_media_poll(service, &media)) {
                if (!epd_service_sleep_acquire(service)) {
                    pending_media = media;
                    has_pending_media = true;
                    deferred = true;
                    break;
                }
                lock(); settled = false; media_dirty = true; unlock();
                has_pending_media = false;
                memset(&command, 0, sizeof(command));
                command.kind = media == EPD_MEDIA_PLAY ? MUSIC_CMD_PLAY :
                               media == EPD_MEDIA_PAUSE ? MUSIC_CMD_PAUSE :
                               media == EPD_MEDIA_STOP ? MUSIC_CMD_STOP :
                               media == EPD_MEDIA_NEXT ? MUSIC_CMD_NEXT : MUSIC_CMD_PREVIOUS;
            } else if (!take_command(&command)) break;
            execute(&command);
        }
        if (epd_service_cancelled(service)) break;
        if (deferred) { epd_service_wait(service, RT_WAITING_FOREVER); continue; }
        if (session_active && !path_valid(working.current_path)) {
            result(MUSIC_ERR_STORAGE, 0, working.current_path);
            close_track(true);
        }
        if (session_active) music_backend_pump(backend);
        update_track();
        if (scan) {
            music_model_result_t step = music_library_scan_step(scan, &library, 8);
            if (step != MUSIC_MODEL_OK) {
                stop_scan();
                if (step == MUSIC_MODEL_LIMIT) music_library_free(&library);
                model_result(step, working.directory);
                list_publish();
            }
        }
        rt_tick_t now = rt_tick_get();
        if (timer_ticks && (rt_tick_t)(now - timer_start) >= timer_ticks) {
            timer_ticks = 0;
            working.timer_minutes = 0;
            close_track(true);
            save();
        }
        if ((session_active || dirty) && (rt_tick_t)(now - last_save) >= 30u * RT_TICK_PER_SECOND) {
            save();
            last_save = now;
        }
        lock();
        uint32_t flush = flush_requested;
        bool do_flush = flush != flush_completed;
        unlock();
        if (do_flush) {
            bool ok = save();
            lock(); flush_ok = ok; flush_completed = flush; flush_ready = ok; unlock();
            epd_service_ui_request(service, 0);
        }
        trim_caches();
        publish();
        epd_service_wait(service, settle(service));
    }
    lock(); accepting = false; unlock();
    stop_scan();
    close_track(true);
    music_cover_stop();
    music_cover_set_wake(NULL, NULL);
    save();
    lock();
    music_backend_t *old = backend;
    backend = NULL;
    unlock();
    music_backend_destroy(old);
    music_metadata_free(&metadata);
    music_library_free(&library);
    music_store_free(&store);
    lock();
    worker = NULL;
    while (commands) { command_node_t *next = commands->next; epd_app_free(commands); commands = next; }
    commands_tail = NULL;
    unlock();
    working.ready = false;
    publish();
    epd_service_sleep_release(service);
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    if (stopping) { music_view_stop(); return true; }
    if (rt_mutex_take(&guard, 0) != RT_EOK) {
        epd_service_ui_request(service, 10);
        return false;
    }
    bool retire = worker == service && accepting && retire_ready &&
                  !ui_active && !music_view_present() && !commands &&
                  !has_pending_media && flush_requested == flush_completed;
    if (retire) {
        accepting = false;
        retire_ready = false;
    }
    /* music_ui_active already signals the worker on foreground transitions.
     * Repeating that wake here also wakes the higher-priority UI itself and
     * prevents the worker from ever consuming cover_refresh. */
    unlock();
    if (retire) {
        app_service_stop("music");
        rt_kprintf("[music] stopped in background: releasing\n");
        return false;
    }
    music_view_update();
    return false;
}

static bool flush(void)
{
    if (!initialized) return true;
    lock();
    if (!worker || !accepting) { unlock(); return true; }
    if (flush_requested != flush_completed) { unlock(); return false; }
    if (flush_ready && flush_ok) {
        unlock();
        return true;
    }
    if (!epd_service_sleep_acquire(worker)) {
        /* A storage-maintenance barrier may acknowledge an already saved idle
         * state without accepting a new job during committed sleep. */
        bool saved = settled;
        unlock();
        return saved;
    }
    settled = false;
    retire_ready = false;
    ++flush_requested;
    epd_service_wake(worker);
    unlock();
    return false;
}

const epd_background_t epd_app_background = {
    .run = run, .stack_size = 12288, .priority = 22,
    .ui_process = ui_process, .flush = flush
};
