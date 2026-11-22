#include "notes_store.h"
#include "storage.h"
#include "storage_file.h"
#include "system_time.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define HISTORY_COUNT 12
#define RT_EOK 0
#define EPD_AI_OK 0
#define EPD_SPEECH_OK 0
typedef int epd_speech_error_t, epd_ai_error_t;
typedef void epd_activity_work_t;
static epd_activity_work_t *image_work_new(void) { return NULL; }
static void epd_activity_work_close(epd_activity_work_t *work) { CHECK(!work); }
#include "notebook_types.inc"

typedef void epd_service_t;
static bool epd_service_sleep_acquire(epd_service_t *);
static bool epd_service_reclaim_hold(epd_service_t *owner) { return owner != NULL; }
static void epd_service_reclaim_release(epd_service_t *owner) { (void)owner; }
static bool epd_service_mark_dirty(epd_service_t *owner) { return owner != NULL; }
#include "../unified_operation/cleanup_ports.h"
#include "app_work.inc"
#include "app_cleanup.inc"
/* Only platform scheduling is replaced; commands, filtering and storage are production code. */
static struct {
    void *mutex, *owner;
    app_work_t work;
    notes_store_t store;
    note_t document, preview, preview_original;
    note_buffer_t saved_document, undo[HISTORY_COUNT], redo[HISTORY_COUNT];
    uint32_t size_revision, edited_block;
    unsigned undos, redos;
    notes_row_t *rows;
    size_t row_count, row_capacity;
    job_t *head, *tail, *network;
    bool stopping, cancel, flushing, save_failed;
    notes_status_t view;
    int ai_cancel;
    uint32_t *matches;
    size_t match_count;
    char query[192];
    notes_task_t *tasks;
    size_t task_count;
    char (*books)[96];
    size_t book_count;
    unsigned preferences;
    void *record, *player;
    uint32_t record_id;
} instance, *state = &instance;
static void lock(void) {}
static void unlock(void) {}
static void wake_io(void) {}
static int rt_mutex_take(void *mutex, int delay) { (void)mutex; (void)delay; return RT_EOK; }
static bool stopping(void *unused) { (void)unused; return state->stopping || state->cancel; }
static bool epd_service_cancelled(void *owner) { (void)owner; return state->stopping; }
uint32_t rt_tick_get(void);
/* Filesystem cases use a terminal allocation failure; recovery scheduling is
 * exercised against the memory core by the dedicated resource tests. */
typedef struct { bool used; } epd_memory_retry_t;
static bool epd_memory_retry_wait(epd_memory_retry_t *retry, size_t bytes,
                                 bool (*cancel)(void *), void *context)
{ (void)bytes; (void)cancel; (void)context; retry->used = true; return false; }
static bool epd_service_sleep_acquire(void *owner) { return owner != NULL; }
static void epd_service_wake(void *owner) { (void)owner; }
static void epd_media_note_intent(void *owner) { (void)owner; }
static void epd_ai_cancel_init(int *cancel) { *cancel = 0; }
typedef struct { uint32_t milliseconds; bool paused; } epd_record_status_t;
static epd_record_status_t recorded;
static void epd_record_status(void *record, epd_record_status_t *out) { (void)record; *out = recorded; }
static bool book_add(const char *name);
#include "notebook_actual.inc"

static notes_error_t command(notes_command_t action, uint32_t id, unsigned argument, const char *text)
{
    CHECK(notes_command(action, id, argument, text));
    job_t *job = state->head;
    CHECK(job && job->action == action);
    state->head = state->tail = NULL;
    notes_error_t error = io_command(job);
    state->view.busy = false;
    note_free(job);
    return error;
}
static void scan(void) { CHECK(command(N_SCAN, 0, 0, NULL) == NOTES_OK); }
static uint32_t fixture(const char *book, const char *text, bool task, bool pinned, bool deleted)
{
    note_t note = {0};
    CHECK(notes_store_new(&state->store, &note) == NOTES_OK);
    strcpy(note.notebook, book);
    CHECK(note_insert(&note, 0, task ? NOTE_TASK : NOTE_TEXT, text));
    note.pinned = pinned; note.deleted = deleted;
    CHECK(notes_store_save(&state->store, &note) == NOTES_OK);
    uint32_t id = note.id;
    update_row(&note);
    note_clear(&note);
    return id;
}
static size_t rows(const char *book, const char *query, unsigned filter)
{
    return notes_rows(NULL, 0, 0, query, book, filter, 0);
}
static unsigned book_index(const char *name)
{
    char books[16][96];
    size_t count = notes_books(books, 0, 16);
    CHECK(count <= 16);
    for (size_t i = 0; i < count; ++i) if (!strcmp(books[i], name)) return (unsigned)i;
    return UINT32_MAX;
}
static void persisted(uint32_t id, const char *book)
{
    note_t note = {0};
    CHECK(notes_store_load(&state->store, id, &note) == NOTES_OK);
    CHECK(!strcmp(note.notebook, book));
    note_clear(&note);
}
static void filters(void)
{
    fixture("", "unfiled", false, false, false);
    fixture("工作", "work", true, true, false);
    fixture("工作", "other", false, false, false);
    fixture("工作", "trash", true, false, true);
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    for (int pass = 0; pass < 2; ++pass) {
        CHECK(rows(NULL, "", 0) == 3 && rows("", "", 0) == 3);
        CHECK(rows("\1", "", 0) == 1 && rows("空本", "", 0) == 0);
        CHECK(rows("工作", "", 0) == 2 && rows("不存在", "", 0) == 0);
        CHECK(rows("工作", "", 1) == 1 && rows("工作", "", 3) == 1);
        CHECK(rows("工作", "", 2) == 1 && rows("空本", "", 2) == 0);
        CHECK(book_index("空本") != UINT32_MAX && book_index("") == UINT32_MAX);
        char book[1][96] = {{0}};
        unsigned index = book_index("空本");
        CHECK(notes_books(book, index, 1) > index && !strcmp(book[0], "空本"));
        for (unsigned sort = 0; sort < 8; ++sort) {
            notes_row_t page[2] = {{0}};
            CHECK(notes_rows(page, 1, 1, "", "工作", 0, sort) == 2);
            CHECK(page[0].id && !page[1].id && !strcmp(page[0].notebook, "工作"));
            CHECK(notes_rows(page, 8, 1, "", "工作", 0, sort) == 2);
        }
        scan();
    }
}
static void creation(void)
{
    fixture("", "unfiled", false, false, false);
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    CHECK(command(N_NEW, 0, 0, "空本") == NOTES_OK);
    uint32_t id = state->document.id;
    CHECK(!strcmp(state->document.notebook, "空本"));
    CHECK(rows("空本", "", 0) == 1 && rows("\1", "", 0) == 1);
    persisted(id, "空本");
    CHECK(command(N_NEW, 0, 2, "空本") == NOTES_OK);
    persisted(state->document.id, "空本");
    CHECK(command(N_NEW, 0, 0, "") == NOTES_OK);
    persisted(state->document.id, "");
    CHECK(command(N_NEW, 0, 0, NULL) == NOTES_OK);
    persisted(state->document.id, "");
    CHECK(command(N_NEW, 0, 1, "空本") == NOTES_OK);
    uint32_t task = state->document.id;
    CHECK(!strcmp(state->document.title, "随手待办") && !state->document.notebook[0]);
    CHECK(state->document.count == 1 && state->document.blocks[0].kind == NOTE_TASK);
    CHECK(command(N_MOVE_BOOK, task, 0, "工作") == NOTES_OK);
    CHECK(command(N_NEW, 0, 1, "空本") == NOTES_OK);
    CHECK(state->document.id == task && state->document.count == 2);
    persisted(task, "工作");
    scan();
    CHECK(rows("空本", "", 0) == 2 && rows("\1", "", 0) == 3 && rows("工作", "", 0) == 1);
}
static void move(void)
{
    uint32_t id = fixture("", "move", false, false, false);
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    CHECK(command(N_MOVE_BOOK, id, 0, "空本") == NOTES_OK);
    CHECK(rows("\1", "", 0) == 0 && rows("空本", "", 0) == 1);
    persisted(id, "空本"); scan();
    CHECK(rows("空本", "", 0) == 1);
    CHECK(command(N_MOVE_BOOK, id, 0, "") == NOTES_OK);
    CHECK(rows("\1", "", 0) == 1 && rows("空本", "", 0) == 0);
    scan(); CHECK(book_index("空本") != UINT32_MAX); persisted(id, "");
}
static void rename_books(void)
{
    uint32_t first = fixture("工作", "rename", false, false, false);
    uint32_t trash = fixture("工作", "trash", false, false, true);
    fixture("阅读", "other", false, false, false);
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    CHECK(command(N_BOOK_RENAME, book_index("空本"), 0, "空本二") == NOTES_OK);
    CHECK(book_index("空本") == UINT32_MAX && rows("空本二", "", 0) == 0);
    CHECK(command(N_BOOK_RENAME, book_index("工作"), 0, "项目") == NOTES_OK);
    CHECK(book_index("工作") == UINT32_MAX && rows("工作", "", 0) == 0);
    CHECK(rows("项目", "", 0) == 1 && rows("项目", "", 2) == 1);
    persisted(first, "项目"); persisted(trash, "项目");
    CHECK(command(N_BOOK_RENAME, book_index("项目"), 0, "阅读") == NOTES_OK);
    CHECK(notes_books(NULL, 0, 0) == 2 && rows("阅读", "", 0) == 2);
    CHECK(command(N_BOOK_RENAME, book_index("阅读"), 0, "阅读") == NOTES_OK);
    scan(); CHECK(notes_books(NULL, 0, 0) == 2 && book_index("空本二") != UINT32_MAX);
    CHECK(command(N_BOOK_DELETE, book_index("阅读"), 0, NULL) == NOTES_OK);
    CHECK(rows("\1", "", 0) == 2 && rows("\1", "", 2) == 1);
    persisted(first, ""); scan(); CHECK(book_index("阅读") == UINT32_MAX);
}
static void search_move(void)
{
    uint32_t id = fixture("工作", "body", false, false, false);
    fixture("阅读", "other", false, false, false);
    CHECK(command(N_QUERY, 0, 0, "阅读") == NOTES_OK);
    CHECK(rows("", "阅读", 0) == 1);
    CHECK(command(N_MOVE_BOOK, id, 0, "阅读") == NOTES_OK);
    CHECK(rows("阅读", "阅读", 0) == 2);
    CHECK(command(N_MOVE_BOOK, id, 0, "工作") == NOTES_OK);
    CHECK(rows("", "阅读", 0) == 1);
    CHECK(command(N_QUERY, 0, 0, "工作") == NOTES_OK);
    CHECK(command(N_MOVE_BOOK, id, 0, "") == NOTES_OK);
    CHECK(rows("", "工作", 0) == 0);
}
static void search_rename(void)
{
    fixture("工作", "body", false, false, false);
    CHECK(command(N_QUERY, 0, 0, "项目") == NOTES_OK);
    CHECK(rows("", "项目", 0) == 0);
    CHECK(command(N_BOOK_RENAME, book_index("工作"), 0, "项目") == NOTES_OK);
    CHECK(rows("项目", "项目", 0) == 1);
    CHECK(command(N_BOOK_RENAME, book_index("项目"), 0, "阅读") == NOTES_OK);
    CHECK(rows("", "项目", 0) == 0);
    CHECK(command(N_QUERY, 0, 0, "阅读") == NOTES_OK);
    CHECK(command(N_NEW, 0, 0, "阅读") == NOTES_OK);
    CHECK(rows("阅读", "阅读", 0) == 2);
    strcpy(state->document.notebook, "其他"); ++state->document.revision; state->view.dirty = true;
    CHECK(save_current() == NOTES_OK);
    CHECK(rows("阅读", "阅读", 0) == 1 && rows("其他", "阅读", 0) == 0);
    scan(); CHECK(rows("阅读", "阅读", 0) == 1);
}
static void validation(void)
{
    uint32_t id = fixture("工作", "body", false, false, false);
    char long_name[97]; memset(long_name, 'a', sizeof(long_name) - 1); long_name[96] = 0;
    const char *invalid[] = {"bad\rbook", "bad\nbook", long_name};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(command(N_MOVE_BOOK, id, 0, invalid[i]) == NOTES_INVALID);
        persisted(id, "工作");
        CHECK(command(N_NEW, 0, 0, invalid[i]) == NOTES_INVALID);
        CHECK(command(N_BOOK_NEW, 0, 0, invalid[i]) == NOTES_INVALID);
        CHECK(command(N_BOOK_RENAME, book_index("工作"), 0, invalid[i]) == NOTES_INVALID);
    }
    CHECK(rows("", "", 0) == 1 && notes_books(NULL, 0, 0) == 1);
    scan(); CHECK(rows("", "", 0) == 1);
}
static void search_content(void)
{
    uint32_t body = fixture("工作", "needle body", false, false, false);
    uint32_t title = fixture("工作", "body", false, false, false);
    uint32_t original = fixture("工作", "edited transcript", false, false, false);
    note_t note = {0};
    CHECK(notes_store_load(&state->store, title, &note) == NOTES_OK);
    strcpy(note.title, "needle title");
    CHECK(notes_store_save(&state->store, &note) == NOTES_OK); update_row(&note);
    CHECK(notes_store_load(&state->store, original, &note) == NOTES_OK);
    note.blocks[0].kind = NOTE_TRANSCRIPT;
    note.blocks[0].original = note_string("needle original", strlen("needle original"));
    CHECK(note.blocks[0].original);
    CHECK(notes_store_save(&state->store, &note) == NOTES_OK); update_row(&note);
    note_clear(&note);
    CHECK(command(N_QUERY, 0, 0, "needle") == NOTES_OK);
    CHECK(rows("工作", "needle", 0) == 3);
    CHECK(command(N_BOOK_RENAME, book_index("工作"), 0, "阅读") == NOTES_OK);
    CHECK(rows("阅读", "needle", 0) == 3);
    CHECK(command(N_MOVE_BOOK, body, 0, "") == NOTES_OK);
    CHECK(rows("\1", "needle", 0) == 1 && rows("阅读", "needle", 0) == 2);
    CHECK(command(N_DELETE, original, 0, NULL) == NOTES_OK);
    CHECK(rows("阅读", "needle", 0) == 1 && rows("阅读", "needle", 2) == 1);
    CHECK(command(N_RESTORE, original, 0, NULL) == NOTES_OK);
    scan(); CHECK(rows("", "needle", 0) == 3);
}
static notes_status_t status(void)
{
    notes_status_t result;
    CHECK(notes_status(&result));
    return result;
}
static void catalog_advanced(notes_status_t previous)
{
    notes_status_t current = status();
    CHECK(current.catalog_revision != previous.catalog_revision);
    CHECK(current.revision != previous.revision);
}
static void catalog(void)
{
    notes_status_t previous = status();
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    catalog_advanced(previous);
    CHECK(status().document == previous.document && status().saved == previous.saved);
    CHECK(!previous.busy && !status().busy);
    previous = status();
    CHECK(command(N_BOOK_NEW, 0, 0, "空本") == NOTES_OK);
    CHECK(status().catalog_revision == previous.catalog_revision);
    CHECK(command(N_BOOK_NEW, 0, 0, "bad\rname") == NOTES_INVALID);
    CHECK(status().catalog_revision == previous.catalog_revision);
    CHECK(command(N_BOOK_RENAME, book_index("空本"), 0, "工作") == NOTES_OK);
    catalog_advanced(previous);
    previous = status();
    CHECK(command(N_BOOK_DELETE, book_index("工作"), 0, NULL) == NOTES_OK);
    catalog_advanced(previous);
    previous = status();
    uint32_t id = fixture("工作", "task", true, false, false);
    catalog_advanced(previous);
    previous = status();
    CHECK(command(N_MOVE_BOOK, id, 0, "阅读") == NOTES_OK);
    catalog_advanced(previous);
    CHECK(status().document == previous.document && status().saved == previous.saved);
    previous = status();
    CHECK(command(N_BOOK_RENAME, book_index("阅读"), 0, "项目") == NOTES_OK);
    catalog_advanced(previous);
    previous = status();
    CHECK(command(N_QUERY, 0, 0, "项目") == NOTES_OK);
    catalog_advanced(previous); CHECK(state->match_count == 1);
    previous = status();
    CHECK(command(N_QUERY, 0, 0, "no matches") == NOTES_OK);
    catalog_advanced(previous); CHECK(state->match_count == 0);
    previous = status();
    CHECK(command(N_TASKS, 0, 0, NULL) == NOTES_OK);
    catalog_advanced(previous); CHECK(state->task_count == 1);
    previous = status();
    CHECK(command(N_TASK_CHECK, id, state->tasks[0].block, NULL) == NOTES_OK);
    catalog_advanced(previous); CHECK(state->tasks[0].checked);
    previous = status();
    CHECK(command(N_DELETE, id, 0, NULL) == NOTES_OK);
    catalog_advanced(previous);
    previous = status();
    CHECK(command(N_TASKS, 0, 0, NULL) == NOTES_OK);
    catalog_advanced(previous); CHECK(state->task_count == 0);
    previous = status();
    CHECK(command(N_RESTORE, id, 0, NULL) == NOTES_OK);
    catalog_advanced(previous);
    CHECK(command(N_DELETE, id, 0, NULL) == NOTES_OK);
    previous = status();
    CHECK(command(N_PURGE, id, 0, NULL) == NOTES_OK);
    catalog_advanced(previous); CHECK(state->row_count == 0);
    previous = status();
    CHECK(command(N_NEW, 0, 0, "工作") == NOTES_OK);
    catalog_advanced(previous);
    previous = status();
    state->document.pinned = true; ++state->document.revision; state->view.dirty = true;
    CHECK(save_current() == NOTES_OK);
    catalog_advanced(previous);
}
static void catalog_scan(void)
{
    fixture("工作", "body", false, false, false);
    notes_status_t previous = status();
    scan(); catalog_advanced(previous); CHECK(state->row_count == 1);
    notes_row_t row = state->rows[0];
    previous = status();
    scan_context_t context = {0};
    CHECK(scan_row(&context, &row)); catalog_advanced(previous);
    /* A valid but emptied card must publish the cleared list, even with no scan callbacks. */
    host_reset(); CHECK(notes_store_open(&state->store) == NOTES_OK);
    previous = status();
    scan(); catalog_advanced(previous);
    CHECK(state->row_count == 0 && state->book_count == 0);
    CHECK(book_add("temporary"));
    previous = status();
    books_load(); catalog_advanced(previous); CHECK(state->book_count == 0);
    fixture("工作", "body", false, false, false);
    host_reset(); host_new_card();
    CHECK(storage_file_replace(NOTES_ROOT, "x", 1));
    previous = status();
    CHECK(command(N_SCAN, 0, 0, NULL) != NOTES_OK);
    catalog_advanced(previous); CHECK(state->row_count == 0);
}
static void catalog_timers(void)
{
    CHECK(command(N_NEW, 0, 0, "工作") == NOTES_OK);
    state->record = state; state->record_id = state->document.blocks[0].id;
    notes_status_t previous = status();
    recorded.milliseconds = 6000;
    record_update();
    CHECK(status().recording_ms == 6000 && status().revision != previous.revision);
    CHECK(status().catalog_revision == previous.catalog_revision);
    recorded.paused = true; record_update();
    CHECK(status().paused && status().catalog_revision == previous.catalog_revision);
    previous = status();
    playback_tick(7000);
    CHECK(status().playback_ms == 7000 && status().revision != previous.revision);
    CHECK(status().catalog_revision == previous.catalog_revision);
    changed(); CHECK(status().catalog_revision == previous.catalog_revision);
    rows("工作", "", 0); notes_books(NULL, 0, 0);
    CHECK(status().catalog_revision == previous.catalog_revision);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    host_reset();
    state->owner = state; state->view.ready = true;
    CHECK(notes_store_open(&state->store) == NOTES_OK);
    if (!strcmp(argv[1], "filters")) filters();
    else if (!strcmp(argv[1], "creation")) creation();
    else if (!strcmp(argv[1], "move")) move();
    else if (!strcmp(argv[1], "rename")) rename_books();
    else if (!strcmp(argv[1], "search_move")) search_move();
    else if (!strcmp(argv[1], "search_rename")) search_rename();
    else if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "search_content")) search_content();
    else if (!strcmp(argv[1], "catalog")) catalog();
    else if (!strcmp(argv[1], "catalog_scan")) catalog_scan();
    else if (!strcmp(argv[1], "catalog_timers")) catalog_timers();
    else CHECK(false);
    printf("PASS: %s\n", argv[1]);
    note_clear(&state->document); note_clear(&state->preview); note_clear(&state->preview_original);
    note_buffer_clear(&state->saved_document);
    history_clear(state->undo, &state->undos); history_clear(state->redo, &state->redos);
    note_free(state->rows); note_free(state->books); note_free(state->matches); note_free(state->tasks);
    host_reset();
    return 0;
}
