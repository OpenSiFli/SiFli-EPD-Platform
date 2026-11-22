/* SPDX-License-Identifier: Apache-2.0 */
#define main toc_replay_main
#include "toc_test.c"
#undef main

static expected_toc_t original[512];
static uint32_t original_offset[512];
static unsigned original_count;

static void load_original(const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f);
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        unsigned chapter, level, offset; int used = 0;
        assert(sscanf(line, "%u\t%u\t%u\t%n", &chapter, &level, &offset, &used) == 3);
        char *target = line + used, *title = strchr(target, '\t'); assert(title); *title++ = 0;
        title[strcspn(title, "\r\n")] = 0;
        assert(original_count < 512 && strlen(target) < 1024 && strlen(title) < 1024);
        expected_toc_t *e = &original[original_count];
        strcpy(e->target, target); strcpy(e->title, title); e->chapter = chapter; e->level = level;
        original_offset[original_count++] = offset;
    }
    fclose(f);
    assert(original_count == 108 && expected_count == original_count);
    for (unsigned i = 0; i < original_count; ++i) {
        assert(expected[i].chapter == original[i].chapter && expected[i].level == original[i].level);
        assert(!strcmp(expected[i].target, original[i].target) && !strcmp(expected[i].title, original[i].title));
    }
}

static unsigned original_title_at(document_t *d, uint32_t record, unsigned wanted)
{
    char title[1024]; unsigned index = UINT32_MAX;
    bool found = document_current_title(d, record, title, sizeof(title), &index);
    if (found && index == wanted && !strcmp(title, original[wanted].title)) return 0;
    printf("FAIL TITLE record=%u expected_index=%u actual_index=%u expected=%s actual=%s found=%u\n",
           record, wanted, index, original[wanted].title, title, found);
    return 1;
}

static unsigned original_titles(document_t *d, const char *phase)
{
    unsigned reads = io_reads, opens = io_opens, hashes = hash_calls, resolved = 0, failures = 0;
    if (document_toc_count(d) != original_count) {
        printf("FAIL COUNT %s expected=%u actual=%u\n", phase, original_count, document_toc_count(d));
        ++failures;
        for (unsigned i = original_count; i < document_toc_count(d); ++i) {
            char title[1024], target[1024]; unsigned level;
            assert(document_toc_entry(d, i, title, sizeof(title), target, sizeof(target), &level));
            printf("EXTRA %s index=%u level=%u target=%s title=%s\n", phase, i, level, target, title);
        }
    }
    for (unsigned i = 0; i < original_count; ++i) {
        char title[1024], target[1024]; unsigned level;
        assert(document_toc_entry(d, i, title, sizeof(title), target, sizeof(target), &level));
        assert(!strcmp(title, original[i].title) && !strcmp(target, original[i].target) && level == original[i].level);
        if (original[i].chapter != d->chapter || original_offset[i] < d->source_begin) continue;
        uint32_t record; assert(document_anchor_find(d, target, &record));
        document_location_t *text = d->locations;
        while (text && (text->record < record || !text->bytes)) text = text->next;
        assert(text && text->record < d->status.end);
        document_anchor_t source;
        assert(document_source_position(d, text->record, 0, &source));
        assert(source.unit == 2 && source.chapter == original[i].chapter && source.offset == original_offset[i]);
        failures += original_title_at(d, record, i);
        failures += original_title_at(d, text->record, i);
        printf("ORIGINAL %s index=%u chapter=%u offset=%u anchor_record=%u text_record=%u title=%s\n",
               phase, i, source.chapter, source.offset, record, text->record, title);
        ++resolved;
    }
    uint32_t record, byte;
    assert(document_target_cursor(d, &record, &byte) == 1);
    char title[1024]; unsigned index = UINT32_MAX;
    bool found = document_current_title(d, record, title, sizeof(title), &index);
    printf("PAGE_START %s record=%u byte=%u found=%u index=%u title=%s\n", phase, record, byte, found, index, title);
    document_location_t *first = d->locations;
    while (first && (first->record < record || !first->bytes)) first = first->next;
    if (first) {
        document_anchor_t source;
        assert(document_source_position(d, first->record, 0, &source));
        unsigned wanted = UINT32_MAX;
        for (unsigned i = 0; i < original_count; ++i)
            if (original[i].chapter == source.chapter &&
                (wanted == UINT32_MAX || original_offset[i] <= source.offset)) wanted = i;
        if (wanted != UINT32_MAX) failures += original_title_at(d, first->record, wanted);
        found = document_current_title(d, first->record, title, sizeof(title), &index);
        printf("FIRST_TEXT %s record=%u source=%u:%u:%u found=%u index=%u title=%s\n",
               phase, first->record, source.unit, source.chapter, source.offset, found, index, title);
    }
    assert(reads == io_reads && opens == io_opens && hashes == hash_calls);
    printf("%s %s original metadata=108 resolved_titles=%u memory_only=1 failures=%u\n",
           failures ? "FAIL" : "PASS", phase, resolved, failures);
    return failures;
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    assert(argc == 4);
    snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    snprintf(persistent_path, sizeof(persistent_path), "%s/persisted.bin", test_root);
    snprintf(index_file, sizeof(index_file), "%s/index.bin", test_root);
    load_wire(argv[2]); load_original(argv[3]);
    document_t *d = open_fixture(); unsigned failures = original_titles(d, "cold"); close_fixture(d);
    unsigned pipes = pipe_opens;
    d = open_fixture(); failures += original_titles(d, "warm"); assert(pipe_opens == pipes); close_fixture(d);
    free(wire_data);
    document_service_shutdown(); document_worker_shutdown(); document_service_process();
    assert(!allocated_bytes && !documents && !fd_live && !hash_calls);
    DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return failures ? 1 : 0;
}
