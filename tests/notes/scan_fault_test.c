/* The filesystem/OS are injection boundaries. Store/model, DFS readdir, scan
 * callbacks, N_SCAN and the complete io_run worker are production C. */
#include "notes_store.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static int model_live, fail_rows;
static void *scan_calloc(size_t count, size_t size)
{
    if (fail_rows && count * size == 32 * sizeof(notes_row_t)) return NULL;
    void *p = calloc(count, size);
    if (p) ++model_live;
    return p;
}
static void scan_free(void *p) { if (p) { CHECK(model_live > 0); --model_live; } free(p); }
#define calloc scan_calloc
#define free scan_free
#include "../../modules/notes/notes_model.c"
#undef calloc
#undef free

#define open raw_open
#define read raw_read
#define opendir raw_opendir
#define readdir raw_readdir
#define closedir raw_closedir
#define storage_unlock raw_storage_unlock
#define storage_file_recover raw_storage_file_recover
#include "host_fs.c"
#undef open
#undef read
#undef opendir
#undef readdir
#undef closedir
#undef storage_unlock
#undef storage_file_recover

static const char *case_name;
static uint32_t ids[2];
static int body_failure, directory_failure = -1, open_failure, close_failure;
static int directory_calls, open_directories, clobber_errno;
static DIR *active_directory;
struct dfs_fd { int unused; };
static struct dfs_fd directory_fd;
static struct dfs_fd *fd_get(int fd) { return active_directory && fd == 91 ? &directory_fd : NULL; }
static void fd_put(struct dfs_fd *fd) { CHECK(fd == &directory_fd); }
static void rt_set_errno(int value) { errno = value; }
static int dfs_file_getdents(struct dfs_fd *fd, struct dirent *entries, size_t bytes)
{
    CHECK(fd == &directory_fd && bytes == 511);
    if (directory_failure >= 0 && directory_calls++ >= directory_failure) return -EIO;
    struct dirent *entry = raw_readdir(active_directory);
    if (!entry) return 0;
    *entries = *entry;
    entries->d_reclen = sizeof(*entries);
    return sizeof(*entries);
}
#define RTM_EXPORT(name)
#include "scan_readdir.inc"
int open(const char *path, int flags, ...)
{
    int fd = raw_open(path, flags, 0);
    if (fd < 0) errno = -ENOENT;
    return fd;
}
int read(int fd, void *out, size_t size)
{
    CHECK(fd >= 3 && fd < 64 && handles[fd].file);
    const char *path = handles[fd].file->path;
    char bad[64]; snprintf(bad, sizeof(bad), "/%08lx/note.bin", (unsigned long)ids[0]);
    if (body_failure && strstr(path,"/note.bin") && (body_failure == 2 || strstr(path,bad)))
    { errno = -EIO; return -1; }
    return raw_read(fd,out,size);
}
DIR *opendir(const char *path)
{
    if (open_failure) { errno = -EIO; return NULL; }
    DIR *dir = raw_opendir(path);
    if (!dir) { errno = -ENOENT; return NULL; }
    CHECK(!active_directory);
    active_directory = dir; dir->fd = 91; ++open_directories;
    directory_calls = 0;
    return dir;
}
int closedir(DIR *dir)
{
    CHECK(active_directory == dir);
    active_directory = NULL; --open_directories;
    raw_closedir(dir);
    if (close_failure) { errno = -EIO; return -1; }
    return 0;
}
void storage_unlock(void) { raw_storage_unlock(); if (clobber_errno) errno = 0; }
bool storage_file_recover(const char *path) { (void)path; return true; }

#define HISTORY_COUNT 12
#define RT_WAITING_FOREVER -1
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define RT_TICK_PER_SECOND 1000
#define RT_EOK 0
#define EPD_AI_OK 0
#define EPD_SPEECH_OK 0
typedef void *rt_mutex_t;
typedef int rt_base_t, rt_int32_t;
typedef uint32_t rt_uint32_t;
typedef void epd_service_t, epd_record_t, epd_activity_work_t;
typedef void *mp3ctrl_handle;
typedef int epd_speech_error_t, epd_ai_error_t, notes_ai_mode_t, epd_ai_cancel_t;
typedef struct { int unused; } app_work_t;
struct rt_event { int unused; };
struct rt_thread { int unused; };
#include "scan_types.inc"
static runtime_t instance;
static int runtime_locks, wakes, schedule_stage, status_checks, retries_requested;
static notes_row_t old_rows[2];
static size_t old_count;
static bool named(const char *s) { return strcmp(case_name,s) == 0; }
static int rt_mutex_take(void *mutex, int delay) { (void)mutex; (void)delay; ++runtime_locks; return 0; }
static void rt_mutex_release(void *mutex) { (void)mutex; CHECK(runtime_locks > 0); --runtime_locks; }
static bool epd_service_cancelled(void *owner) { (void)owner; return false; }
static void epd_activity_work_close(void *p) { CHECK(!p); }
static epd_activity_work_t *image_work_new(void) { CHECK(0); return NULL; }
static bool app_work_acquire(app_work_t *work, void *owner) { (void)work; return owner != NULL; }
static void app_work_release(app_work_t *work) { (void)work; }
static void epd_service_sleep_release(void *owner) { (void)owner; }
static void epd_media_intent_settled(void *owner) { (void)owner; }
static void epd_media_note_intent(void *owner) { (void)owner; }
static void epd_ai_cancel_init(epd_ai_cancel_t *cancel) { *cancel = 0; }
static void epd_service_wake(void *owner) { (void)owner; }
static int rt_event_send(struct rt_event *event, uint32_t bits) { (void)event; (void)bits; return 0; }
static notes_error_t save_current(void) { CHECK(!state->view.dirty); return NOTES_OK; }
static void record_update(void) { CHECK(0); }
static void record_close(void) { CHECK(0); }
static void player_close(void) { CHECK(0); }
typedef struct { bool finished; } epd_record_status_t;
static int epd_record_pump(void *p) { (void)p; CHECK(0); return 0; }
static void epd_record_status(void *p, epd_record_status_t *s) { (void)p; (void)s; CHECK(0); }
typedef int epd_media_command_t;
static bool epd_media_poll(void *owner, epd_media_command_t *cmd) { (void)owner; (void)cmd; return false; }
static int mp3ctrl_seek(void *p, uint32_t where) { (void)p; (void)where; CHECK(0); return 0; }
static int rt_tick_from_millisecond(int ms) { return ms; }
static int rt_event_recv(struct rt_event *, uint32_t, int, int, uint32_t *);
#include "scan_runtime.inc"

static void seed_note(unsigned i)
{
    note_t note = {0}; CHECK(notes_store_new(&state->store,&note) == NOTES_OK);
    ids[i] = note.id;
    snprintf(note.title,sizeof(note.title),"fake-note-%u",i);
    CHECK(note_insert(&note,0,NOTE_TEXT,"synthetic scan data"));
    CHECK(notes_store_save(&state->store,&note) == NOTES_OK);
    note_clear(&note);
}
static void remove_summary(unsigned i)
{
    char path[NOTES_PATH_SIZE]; CHECK(notes_note_path(path,sizeof(path),ids[i],"summary.bin"));
    CHECK(unlink(path) == 0);
}
static void arm_fault(void)
{
    if (named("body_io_partial") || named("boot_error_visible") || named("refresh_body_error") || named("retry_after_partial")) body_failure = 1;
    if (named("all_body_io_keep_old")) body_failure = 2;
    if (named("directory_io_first") || named("refresh_directory_error")) directory_failure = 0;
    if (named("directory_io_partial")) { directory_failure = 1; clobber_errno = 1; }
    if (named("directory_close_io")) close_failure = 1;
    if (named("directory_open_io")) open_failure = 1;
    if (named("callback_oom")) fail_rows = 1;
    if (named("stale_errno")) errno = -EIO;
    if (named("cancel_keep_old")) state->cancel = true;
}
static void clear_fault(void)
{ body_failure = 0; directory_failure = -1; open_failure = close_failure = fail_rows = clobber_errno = 0; state->cancel = false; }
static void enqueue_scan(void)
{
    CHECK(state->view.ready && !state->view.busy && !state->head);
    CHECK(notes_command(N_SCAN, 0, 0, NULL));
    ++retries_requested;
}
static int rt_event_recv(struct rt_event *event, uint32_t mask, int flags, int delay, uint32_t *out)
{
    (void)event; (void)mask; (void)flags; (void)delay; *out = 1;
    CHECK(++wakes <= 5 && runtime_locks == 0 && !state->view.busy);
    if (named("refresh_body_error") || named("refresh_directory_error") || named("retry_after_partial") || named("repeated_scan_no_duplicates"))
    {
        if (schedule_stage++ == 0)
        {
            CHECK(state->view.error == NOTES_OK && state->row_count == 2);
            old_count = state->row_count; memcpy(old_rows,state->rows,old_count*sizeof(*old_rows));
            arm_fault(); enqueue_scan(); return 0;
        }
        if (named("retry_after_partial") && schedule_stage == 2)
        {
            CHECK(state->view.error == NOTES_IO && state->row_count == 1 && state->rows[0].id == ids[1]);
            ++status_checks; clear_fault(); enqueue_scan(); return 0;
        }
    }
    state->stopping = true;
    return 0;
}
static void keep_old(void)
{
    state->rows = note_alloc(2*sizeof(*state->rows)); CHECK(state->rows);
    state->row_capacity = state->row_count = 2;
    state->rows[0].id = 0xaaa; strcpy(state->rows[0].title,"old-a");
    state->rows[1].id = 0xbbb; strcpy(state->rows[1].title,"old-b");
    old_count = 2; memcpy(old_rows,state->rows,2*sizeof(*old_rows));
}
static void cleanup(void)
{
    CHECK(open_directories == 0 && active_directory == NULL && lock_depth == 0 && runtime_locks == 0);
    for (int i=3;i<64;++i) CHECK(!handles[i].file);
    note_free(state->rows); note_free(state->books);
    note_clear(&state->document); note_clear(&state->preview); note_clear(&state->preview_original);
    note_buffer_clear(&state->saved_document);
    CHECK(model_live == 0);
    host_reset();
}
int main(int argc,char **argv)
{
    CHECK(argc == 2); case_name = argv[1]; state = &instance; state->owner = &instance;
    host_reset(); CHECK(notes_store_open(&state->store) == NOTES_OK);
    if (!named("empty_success_clears_old")) { seed_note(0); seed_note(1); }
    if (named("missing_summary")) { remove_summary(0); remove_summary(1); }
    if (named("corrupt_summary")) {
        char path[NOTES_PATH_SIZE]; notes_note_path(path,sizeof(path),ids[0],"summary.bin");
        CHECK(storage_file_replace(path,"bad",3));
    }
    if (named("body_invalid_partial")) {
        remove_summary(0); char path[NOTES_PATH_SIZE]; notes_note_path(path,sizeof(path),ids[0],"note.bin");
        CHECK(storage_file_replace(path,"bad",3));
    }
    bool refresh = named("refresh_body_error") || named("refresh_directory_error") || named("retry_after_partial") || named("repeated_scan_no_duplicates");
    bool keep = named("directory_io_first") || named("directory_open_io") || named("all_body_io_keep_old") || named("empty_success_clears_old") || named("cancel_keep_old");
    if (keep) keep_old();
    if (!refresh) arm_fault();
    io_run(NULL);
    CHECK(state->view.ready && !state->view.busy && !state->io_active);
    notes_error_t expected = NOTES_OK;
    if (named("body_invalid_partial")) expected = NOTES_INVALID;
    else if (named("callback_oom")) expected = NOTES_NOMEM;
    else if (named("cancel_keep_old")) expected = NOTES_CANCELLED;
    else if (body_failure || directory_failure >= 0 || open_failure || close_failure) expected = NOTES_IO;
    CHECK(state->view.error == expected);
    notes_status_t published; CHECK(notes_status(&published));
    CHECK(published.error == expected && published.ready && !published.busy);
    if ((keep && !named("empty_success_clears_old")) || named("refresh_directory_error"))
        CHECK(state->row_count == old_count && !memcmp(state->rows,old_rows,old_count*sizeof(*old_rows)));
    else if (named("body_io_partial") || named("body_invalid_partial") || named("boot_error_visible") || named("refresh_body_error"))
        CHECK(state->row_count == 1 && state->rows[0].id == ids[1]);
    else if (named("directory_io_partial")) CHECK(state->row_count == 1 && state->rows[0].id == ids[0]);
    else if (named("empty_success_clears_old") || named("callback_oom")) CHECK(state->row_count == 0);
    else CHECK(state->row_count == 2 && state->rows[0].id != state->rows[1].id);
    if (named("retry_after_partial")) CHECK(status_checks == 1 && retries_requested == 2 && state->view.error == NOTES_OK);
    cleanup(); printf("PASS %s\n",case_name); return 0;
}
