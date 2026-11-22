static void history_bounds(void)
{
    reset(); seed(2);
    music_queue_t *q = &store.queue;
    OK(music_queue_set_mode(q, MUSIC_MODE_REPEAT_ALL));
    for (unsigned i = 0; i < MUSIC_HISTORY_LIMIT; ++i) OK(music_queue_next(q, true));
    CHECK(q->history_count == MUSIC_HISTORY_LIMIT && q->history_capacity == MUSIC_HISTORY_LIMIT);
    unsigned allocated = allocations;
    for (unsigned i = MUSIC_HISTORY_LIMIT; i < 100000; ++i) {
        OK(music_queue_next(q, true));
        CHECK(q->history_count == MUSIC_HISTORY_LIMIT && q->history_capacity * sizeof(uint32_t) == 1024);
    }
    CHECK(allocations == allocated && music_queue_valid(q));
    OK(music_queue_set_mode(q, MUSIC_MODE_SHUFFLE));
    for (unsigned i = 0; i < 100000; ++i) {
        OK(music_queue_next(q, true));
        CHECK(q->history_count <= MUSIC_HISTORY_LIMIT && q->history_capacity == MUSIC_HISTORY_LIMIT);
    }
    uint32_t retained[MUSIC_HISTORY_LIMIT];
    memcpy(retained, q->history, sizeof(retained));
    music_store_mark_changed(&store); OK(music_store_save(&store));
    CHECK(disk[0].size < 2048);
    music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
    CHECK(loaded.queue.history_count == MUSIC_HISTORY_LIMIT && !loaded.snapshot_dirty);
    CHECK(!memcmp(loaded.queue.history, retained, sizeof(retained)));
    for (size_t i = MUSIC_HISTORY_LIMIT - 1; i; --i) {
        OK(music_queue_previous(&loaded.queue));
        CHECK(loaded.queue.current_id == retained[i - 1]);
    }
    CHECK(music_queue_previous(&loaded.queue) == MUSIC_MODEL_END);
    allocated = allocations;
    for (size_t i = 1; i < MUSIC_HISTORY_LIMIT; ++i) {
        OK(music_queue_next(&loaded.queue, false));
        CHECK(loaded.queue.current_id == retained[i]);
    }
    CHECK(allocations == allocated);
    OK(music_queue_previous(&loaded.queue));
    OK(music_queue_previous(&loaded.queue));
    uint32_t branch = loaded.queue.current_id == 1 ? 2 : 1;
    OK(music_queue_select(&loaded.queue, branch));
    CHECK(loaded.queue.history_count == MUSIC_HISTORY_LIMIT - 1 && music_queue_valid(&loaded.queue));
    music_store_free(&loaded);
    /* Failed-open removal must address the shifted cursor at the cap. */
    OK(music_queue_failed_next(q));
    CHECK(q->history_count == MUSIC_HISTORY_LIMIT - 1 && music_queue_valid(q));
    CHECK(q->history[q->history_cursor] == q->current_id);
    CHECK(music_queue_failed_next(q) == MUSIC_MODEL_END && music_queue_valid(q));
    OK(music_queue_remove(q, q->current_id)); CHECK(music_queue_valid(q));
    puts("PASS history bounds: 100000 repeat + 100000 shuffle transitions, 1KiB cap, save/reload, 255 back/forward, branch, failed-open rollover/removal");
}

static void bytes_u32(unsigned char *out, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (i * 8)); }

static void legacy_history_bounds(void)
{
    const uint32_t cursors[] = {100000, 42, 50000};
    for (unsigned version = 1; version <= 2; ++version) {
        for (unsigned c = 0; c < sizeof(cursors) / sizeof(cursors[0]); ++c) {
            reset(); seed(2);
            const uint32_t count = 100001, cursor = cursors[c];
            size_t offset = 60 + (store.directory ? strlen(store.directory) : 0);
            for (size_t i = 0; i < store.queue.count; ++i) offset += 32 + strlen(store.queue.entries[i].path);
            CHECK(offset + store.queue.history_count * 4 == disk[0].size);
            size_t size = offset + (size_t)count * 4;
            unsigned char *bytes = malloc(size); CHECK(bytes);
            memcpy(bytes, disk[0].data, offset);
            bytes_u32(bytes + 16, 1 + (cursor & 1));
            bytes_u32(bytes + 20, MUSIC_MODE_SHUFFLE);
            bytes_u32(bytes + 44, count); bytes_u32(bytes + 48, cursor);
            for (uint32_t i = 0; i < count; ++i) bytes_u32(bytes + offset + (size_t)i * 4, 1 + (i & 1));
            if (version == 1) { memmove(bytes, bytes + 8, size - 8); size -= 8; }
            free(disk[0].data); disk[0].data = bytes; disk[0].size = size;
            disk[0].version = version; disk[0].crc = storage_crc32(bytes, size);
            if (version == 2) {
                unsigned char progress[24] = {0};
                memcpy(progress, bytes, 8);
                bytes_u32(progress + 8, disk[0].crc);
                bytes_u32(progress + 12, 1 + (cursor & 1));
                bytes_u32(progress + 16, 98765);
                CHECK(storage_record_save("/flash/data/music/progress.bin", 0x4d505247u, 1, progress, sizeof(progress)));
            }
            music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
            music_queue_t *q = &loaded.queue;
            size_t first = cursor >= MUSIC_HISTORY_LIMIT - 1 ? cursor - (MUSIC_HISTORY_LIMIT - 1) : 0;
            CHECK(loaded.snapshot_dirty && q->history_count == MUSIC_HISTORY_LIMIT && q->history_capacity == MUSIC_HISTORY_LIMIT);
            CHECK(q->history_cursor == cursor - first && q->current_id == 1 + (cursor & 1));
            CHECK(q->position_ms == (version == 2 ? 98765u : 0u));
            for (size_t i = 0; i < MUSIC_HISTORY_LIMIT; ++i) CHECK(q->history[i] == 1 + ((first + i) & 1));
            fail_save = true;
            CHECK(music_store_save(&loaded) == MUSIC_MODEL_IO && loaded.snapshot_dirty && disk[0].size == size);
            fail_save = false; OK(music_store_save(&loaded));
            CHECK(!loaded.snapshot_dirty && disk[0].size < 2048 && disk[0].version == MUSIC_STORE_VERSION);
            OK(music_store_load(&loaded)); CHECK(!loaded.snapshot_dirty && music_queue_valid(&loaded.queue));
            music_store_free(&loaded);
        }
    }
    /* Loaded capacity need not be a power of two. Its next growth still caps. */
    reset(); seed(2); OK(music_queue_set_mode(&store.queue, MUSIC_MODE_REPEAT_ALL));
    for (unsigned i = 1; i < 17; ++i) OK(music_queue_next(&store.queue, true));
    music_store_mark_changed(&store); OK(music_store_save(&store)); OK(music_store_load(&store));
    CHECK(store.queue.history_capacity == 17);
    for (unsigned i = 0; i < 1000; ++i) OK(music_queue_next(&store.queue, true));
    CHECK(store.queue.history_capacity == MUSIC_HISTORY_LIMIT && music_queue_valid(&store.queue));
    puts("PASS legacy history: v1/v2 100001 IDs trimmed around start/middle/end cursor, bound progress restored, failed save retains legacy data, rewrite/reload and non-power-of-two growth");
}
