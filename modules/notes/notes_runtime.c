#include "notes_runtime.h"
#include "epd_memory.h"
#include "../common/memory_retry.h"
#include "storage.h"
#include "storage_file.h"
#include "system_time.h"
#include "platform/epd_app.h"
#include "platform/epd_system.h"
#include <mod_installer.h>
#include <audio_mp3ctrl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "raster.h"

#define HISTORY_COUNT 12
typedef struct job {
    struct job *next;
    notes_command_t action;
    uint32_t id, argument;
    char text[NOTES_PATH_SIZE];
} job_t;
typedef struct {
    rt_mutex_t mutex;
    struct rt_event io_event;
    struct rt_thread io_thread;
    void *io_stack;
    epd_service_t *owner;
    notes_store_t store;
    note_t document, preview, preview_original;
    note_buffer_t saved_document;
    note_size_t edit_size;
    uint32_t size_id, size_revision, edited_block;
    note_buffer_t undo[HISTORY_COUNT], redo[HISTORY_COUNT];
    unsigned undos, redos;
    notes_row_t *rows;
    size_t row_count, row_capacity;
    job_t *head, *tail, *network;
    bool io_started, stopping, flushing, save_failed, cancel;
    bool idle_stopping, retired;
    volatile bool io_done;
    uint32_t deadline, edited_at, preview_source, record_id;
    notes_ai_mode_t preview_mode;
    notes_status_t view;
    epd_ai_cancel_t ai_cancel;
    epd_record_t *record;
    mp3ctrl_handle player;
    volatile bool player_end;
    volatile uint32_t player_seconds;
    uint32_t player_id, player_note, player_seek, player_resume;
    volatile bool player_progress;
    notes_file_t *files;
    size_t file_count;
    char directory[NOTES_PATH_SIZE];
    uint8_t *gray;
    unsigned image_width, image_height;
    uint32_t *matches;
    size_t match_count;
    char query[192];
    notes_task_t *tasks;
    size_t task_count;
    char (*books)[96];
    size_t book_count;
    unsigned preferences;
} runtime_t;
static runtime_t *state;
static bool book_name_valid(const char *name);
static bool book_add(const char *name);
static int extension_compare(const char *a, const char *b)
{
    for(;;++a,++b){unsigned x=(unsigned char)*a,y=(unsigned char)*b;
        if(x>='A'&&x<='Z')x+='a'-'A';if(y>='A'&&y<='Z')y+='a'-'A';
        if(x!=y||!x)return (int)x-(int)y;
    }
}
static void lock(void) { rt_mutex_take(state->mutex, RT_WAITING_FOREVER); }
static void unlock(void) { rt_mutex_release(state->mutex); }
static uint32_t now(void) { return (uint32_t)((uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND); }
static void changed(void) { ++state->view.revision; }
static void catalog_changed(void) { ++state->view.catalog_revision; changed(); }
static void wake_io(void) { rt_event_send(&state->io_event, 1); }
static bool stopping(void *ignored)
{
    (void)ignored; lock();
    bool stop = state->stopping || state->cancel || (state->owner && epd_service_cancelled(state->owner));
    unlock(); return stop;
}
static bool record_cancel(void *ignored)
{
    (void)ignored; lock(); bool stop = state->stopping; unlock(); return stop;
}
static void status_error(notes_error_t error, const char *message)
{
    lock(); state->view.error = error;
    snprintf(state->view.message, sizeof(state->view.message), "%s", message ? message : ""); changed(); unlock();
}
static void history_clear(note_buffer_t *items, unsigned *count)
{ while (*count) note_buffer_clear(&items[--*count]); }
static bool history_push(note_buffer_t *items, unsigned *count, const note_t *note)
{
    note_buffer_t encoded = {0}; if (!note_encode(note, &encoded)) { note_buffer_clear(&encoded); return false; }
    size_t total = encoded.size;
    for (unsigned i = 0; i < *count; ++i) total += items[i].size;
    while (*count && (*count == HISTORY_COUNT || total > 512u * 1024u)) {
        total -= items[0].size;
        note_buffer_clear(items); memmove(items, items + 1, (*count - 1) * sizeof(*items)); --*count;
    }
    items[(*count)++] = encoded; return true;
}
static void dirty(void)
{
    state->document.updated = (uint32_t)system_time_get_utc();
    state->view.dirty = true; state->save_failed = false; state->deadline = now() + 1000;
    state->view.document = state->document.id; changed(); wake_io();
}
bool notes_mutate(notes_edit_fn edit, void *argument, bool coalesce)
{
    if (!state || rt_mutex_take(state->mutex, RT_WAITING_FOREVER) != RT_EOK) return false;
    bool ok = state->view.ready && !state->view.busy && !state->flushing && !state->stopping &&
        state->document.id && !state->document.deleted;
    if (ok) ok = epd_service_sleep_acquire(state->owner);
    if (ok) {
        note_t candidate={0};
        ok=note_clone(&candidate,&state->document)&&edit(&candidate,argument);
        if(ok){++candidate.revision;ok=note_measure(&candidate,NULL);}
        bool snapshot = !coalesce || now() - state->edited_at >= 1000 || !state->undos;
        if (ok&&snapshot) ok = history_push(state->undo, &state->undos, &state->document);
        if (ok) {
            note_clear(&state->document);state->document=candidate;memset(&candidate,0,sizeof(candidate));
            history_clear(state->redo, &state->redos); state->edited_at = now(); dirty();
        }
        note_clear(&candidate);state->edited_block=0;wake_io();
    }
    unlock(); return ok;
}
bool notes_edit_text(uint32_t id, const char *text, note_t *mirror)
{
    if (!state || !text || rt_mutex_take(state->mutex, RT_WAITING_FOREVER) != RT_EOK) return false;
    note_t *n = &state->document;
    bool ok = state->view.ready && !state->view.busy && !state->flushing && !state->stopping && n->id && !n->deleted;
    note_block_t *block = ok ? note_find(n, id) : NULL;
    note_block_t *display = NULL;
    note_text_edit_t edit = {0}, visible = {0};
    ok = block != NULL;
    if (ok && mirror) {
        if (mirror->id != n->id || mirror->revision != n->revision) ok = note_clone(mirror, n);
        display = ok ? note_find(mirror, id) : NULL;
        ok = display != NULL;
    }
    if (ok && !strcmp(block->text, text)) { unlock(); return true; }
    if (ok && (state->size_id != n->id || state->size_revision != n->revision)) {
        ok = note_measure(n, &state->edit_size);
        if (ok) { state->size_id = n->id; state->size_revision = n->revision; }
    }
    if (ok) ok = note_edit_prepare(block, text, state->edit_size, &edit);
    if (ok && mirror) ok = note_edit_prepare(display, text, state->edit_size, &visible);
    if (ok) ok = epd_service_sleep_acquire(state->owner);
    bool snapshot = state->edited_block != id || now() - state->edited_at >= 1000 || !state->undos;
    if (ok && snapshot) ok = history_push(state->undo, &state->undos, n);
    if (ok) {
        note_edit_apply(block, &edit); ++n->revision;
        state->edit_size = edit.size; state->size_id = n->id; state->size_revision = n->revision;
        history_clear(state->redo, &state->redos);
        state->edited_block = id; state->edited_at = now(); dirty();
        if (mirror) {
            note_edit_apply(display, &visible);
            mirror->revision = n->revision; mirror->updated = n->updated;
        }
    }
    note_edit_clear(&edit); note_edit_clear(&visible); wake_io(); unlock(); return ok;
}
bool notes_edit_metadata(const char *title, const char *notebook, note_t *mirror)
{
    if (!state || !title || !notebook) return false;
    size_t title_size = strlen(title), book_size = strlen(notebook);
    if (title_size >= sizeof(state->document.title) || book_size >= sizeof(state->document.notebook) ||
        !note_utf8(title, title_size) || !note_utf8(notebook, book_size)) return false;
    /* Inputs may be fields in the UI mirror, which can need resynchronization. */
    char new_title[160], new_book[96];
    memcpy(new_title, title, title_size + 1); memcpy(new_book, notebook, book_size + 1);
    lock(); note_t *n = &state->document;
    bool ok = state->view.ready && !state->view.busy && !state->flushing && !state->stopping && n->id && !n->deleted;
    if (ok && mirror && (mirror->id != n->id || mirror->revision != n->revision)) ok = note_clone(mirror, n);
    if (ok && !strcmp(n->title, new_title) && !strcmp(n->notebook, new_book)) { unlock(); return true; }
    if (ok && (state->size_id != n->id || state->size_revision != n->revision)) {
        ok = note_measure(n, &state->edit_size);
        if (ok) { state->size_id = n->id; state->size_revision = n->revision; }
    }
    size_t encoded = ok ? state->edit_size.encoded - strlen(n->title) - strlen(n->notebook) + title_size + book_size : 0;
    if (ok) ok = encoded <= 4u * NOTE_TEXT_LIMIT && epd_service_sleep_acquire(state->owner);
    bool snapshot = state->edited_block != UINT32_MAX || now() - state->edited_at >= 1000 || !state->undos;
    if (ok && snapshot) ok = history_push(state->undo, &state->undos, n);
    if (ok) {
        strcpy(n->title, new_title); strcpy(n->notebook, new_book); ++n->revision;
        state->edit_size.encoded = encoded; state->size_id = n->id; state->size_revision = n->revision;
        history_clear(state->redo, &state->redos); state->edited_block = UINT32_MAX; state->edited_at = now(); dirty();
        if (mirror) { strcpy(mirror->title, new_title); strcpy(mirror->notebook, new_book); mirror->revision = n->revision; mirror->updated = n->updated; }
    }
    wake_io(); unlock(); return ok;
}
bool notes_undo(bool redo)
{
    if (!state) return false; lock();
    note_buffer_t *from = redo ? state->redo : state->undo, *to = redo ? state->undo : state->redo;
    unsigned *count = redo ? &state->redos : &state->undos, *other = redo ? &state->undos : &state->redos;
    bool ok = *count && !state->view.busy && !state->view.recording && !state->view.online &&
        !state->flushing && notes_store_valid(&state->store) && epd_service_sleep_acquire(state->owner);
    note_t n = {0};
    if (ok) ok = note_decode(&n, from[*count - 1].data, from[*count - 1].size) && history_push(to, other, &state->document);
    if (ok) {
        n.revision = state->document.revision + 1; note_clear(&state->document); state->document = n; memset(&n, 0, sizeof(n));
        note_buffer_clear(&from[--*count]); state->edited_block = 0; dirty();
    }
    note_clear(&n); wake_io(); unlock(); return ok;
}
bool notes_document(note_t *out)
{
    if (!state || rt_mutex_take(state->mutex, RT_WAITING_FOREVER) != RT_EOK) return false;
    bool ok = state->document.id && note_clone(out, &state->document); unlock(); return ok;
}
static bool document_snapshot(note_t *out)
{
    lock(); bool ok = state->document.id && note_clone(out, &state->document); unlock(); return ok;
}
bool notes_preview(note_t *out, notes_ai_mode_t *mode)
{
    if (!state || rt_mutex_take(state->mutex, 0) != RT_EOK) return false;
    bool ok = state->view.preview && note_clone(out, &state->preview);
    if (ok && mode) *mode = state->preview_mode; unlock(); return ok;
}
bool notes_status(notes_status_t *out)
{
    if (!state || rt_mutex_take(state->mutex, 0) != RT_EOK) return false;
    *out = state->view; out->card = state->store.session; unlock(); return true;
}
epd_service_t *notes_owner(void) { return state ? state->owner : NULL; }
bool notes_preview_original(note_t *out)
{
    if (!state) return false;
    lock(); bool ok=state->view.preview && note_clone(out,&state->preview_original); unlock(); return ok;
}
size_t notes_files(notes_file_t *files, size_t offset, size_t capacity, char *path, size_t size)
{
    if (!state) return 0; lock();
    size_t count = state->file_count;
    if (offset < count) { size_t n = count - offset; if (n > capacity) n = capacity; memcpy(files, state->files + offset, n * sizeof(*files)); }
    snprintf(path, size, "%s", state->directory); unlock(); return count;
}
bool notes_take_image(uint8_t **gray, unsigned *width, unsigned *height)
{
    if (!state) return false; lock(); bool ok = state->gray != NULL;
    if (ok) { *gray = state->gray; *width = state->image_width; *height = state->image_height; state->gray = NULL; }
    unlock(); return ok;
}
bool notes_preview_select(uint32_t id, bool selected)
{
    if (!state) return false; lock(); note_block_t *b = note_find(&state->preview, id);
    if (b) { b->checked = selected; changed(); } unlock(); return b != NULL;
}
void notes_cancel(void)
{
    if (!state) return; lock(); state->cancel = true; epd_ai_cancel(&state->ai_cancel); unlock(); wake_io();
}
void notes_save_now(void)
{
    if (!state) return; lock();
    if (state->view.dirty && state->owner && epd_service_sleep_acquire(state->owner)) {
        state->deadline = 0; state->save_failed = false; wake_io();
    }
    unlock();
}
static bool note_matches_query(const note_t *n, const char *query)
{
    if (!*query || strstr(n->title, query) || strstr(n->notebook, query)) return true;
    for (size_t i = 0; i < n->count; ++i) {
        const note_block_t *b = &n->blocks[i];
        if (strstr(b->text, query) || (b->original && strstr(b->original, query))) return true;
    }
    return false;
}
static void update_row(const note_t *n)
{
    lock(); size_t i;
    for (i = 0; i < state->row_count && state->rows[i].id != n->id; ++i) {}
    if (i == state->row_count) {
        if (state->row_count == state->row_capacity) {
            size_t cap = state->row_capacity + 32;
            notes_row_t *next = note_alloc(cap * sizeof(*next));
            if (!next) { state->view.error = NOTES_NOMEM; changed(); unlock(); return; }
            memcpy(next, state->rows, state->row_count * sizeof(*next)); note_free(state->rows); state->rows = next; state->row_capacity = cap;
        }
        ++state->row_count;
    }
    notes_row_t *row = &state->rows[i]; memset(row, 0, sizeof(*row));
    book_add(n->notebook);
    row->id = n->id; row->revision = n->revision; row->created = n->created; row->updated = n->updated;
    row->deleted = n->deleted; row->pinned = n->pinned; strcpy(row->notebook, n->notebook);
    note_display_title(n, row->title, sizeof(row->title));
    for (size_t b = 0; b < n->count; ++b) {
        if (n->blocks[b].kind == NOTE_TASK && !n->blocks[b].checked) ++row->tasks;
        if (!row->summary[0] && n->blocks[b].text[0]) {
            size_t bytes = strlen(n->blocks[b].text); if (bytes >= sizeof(row->summary)) bytes = sizeof(row->summary) - 1;
            while (bytes && ((unsigned char)n->blocks[b].text[bytes] & 0xc0) == 0x80) --bytes;
            memcpy(row->summary, n->blocks[b].text, bytes);
        }
    }
    if (*state->query) {
        size_t match;
        for (match = 0; match < state->match_count && state->matches[match] != n->id; ++match) {}
        if (note_matches_query(n, state->query)) {
            if (match == state->match_count) {
                uint32_t *next = note_alloc((state->match_count + 1) * sizeof(*next));
                if (next) {
                    memcpy(next, state->matches, state->match_count * sizeof(*next));
                    note_free(state->matches); state->matches = next; state->matches[state->match_count++] = n->id;
                } else {
                    state->query[0] = 0; state->match_count = 0; state->view.error = NOTES_NOMEM;
                }
            }
        } else if (match < state->match_count) {
            memmove(state->matches + match, state->matches + match + 1, (--state->match_count - match) * sizeof(*state->matches));
        }
    }
    catalog_changed(); unlock();
}
static bool scan_row(void *unused, const notes_row_t *row)
{
    (void)unused; if (stopping(NULL)) return false; lock();
    if (state->row_count == state->row_capacity) {
        size_t cap = state->row_capacity + 32; notes_row_t *next = note_alloc(cap * sizeof(*next));
        if (!next) { unlock(); return false; }
        memcpy(next, state->rows, state->row_count * sizeof(*next)); note_free(state->rows); state->rows = next; state->row_capacity = cap;
    }
    state->rows[state->row_count++] = *row; catalog_changed(); unlock(); return true;
}
static bool matches(const notes_row_t *row, const char *query, const char *notebook, unsigned filter)
{
    bool found = !query || !*query;
    if (!found && !strcmp(query, state->query))
        for (size_t i = 0; i < state->match_count; ++i) if (state->matches[i] == row->id) { found = true; break; }
    return row->deleted == (filter == 2) && (filter != 1 || row->tasks) && (filter != 3 || row->pinned) &&
        (!notebook || !*notebook || (!strcmp(notebook,"\1") ? !*row->notebook : !strcmp(row->notebook, notebook))) &&
        found;
}
static unsigned row_sort;
static int compare_rows(const void *a, const void *b)
{
    const notes_row_t *x = a, *y = b;
    if (x->pinned != y->pinned) return x->pinned ? -1 : 1;
    unsigned mode=row_sort&3u;
    int order;
    if (mode == 2) order=strcmp(x->title, y->title);
    else { uint32_t xx = mode == 1 ? x->created : x->updated, yy = mode == 1 ? y->created : y->updated;
        order=xx==yy?(x->id==y->id?0:x->id>y->id?-1:1):xx>yy?-1:1; }
    return row_sort&4u?-order:order;
}
size_t notes_rows(notes_row_t *out, size_t offset, size_t capacity, const char *query, const char *book, unsigned filter, unsigned sort)
{
    if (!state || rt_mutex_take(state->mutex, 0) != RT_EOK) return 0;
    row_sort = sort; qsort(state->rows, state->row_count, sizeof(*state->rows), compare_rows);
    size_t count = 0;
    for (size_t i = 0; i < state->row_count; ++i) if (matches(&state->rows[i], query, book, filter)) {
        if (count >= offset && count - offset < capacity) out[count - offset] = state->rows[i]; ++count;
    }
    unlock(); return count;
}
static bool save_allocation_cancelled(void *unused)
{
    (void)unused;
    lock();
    bool cancel = state->stopping || state->record || state->player ||
                  (state->owner && epd_service_cancelled(state->owner));
    unlock();
    return cancel;
}
static notes_error_t save_current(void)
{
    note_t copy = {0}; lock(); bool pending = state->view.dirty;
    bool copied = !pending || note_clone(&copy, &state->document); unlock();
    if (!pending) return NOTES_OK;
    notes_error_t e = copied ? NOTES_OK : NOTES_NOMEM;
    if (!e && !notes_store_valid(&state->store))
        e = notes_store_resume(&state->store, copy.id, &state->saved_document);
    note_buffer_t saved = {0};
    if (!e && !note_encode(&copy, &saved)) {
        size_t bytes = saved.allocation_failed;
        epd_memory_retry_t retry = {0};
        note_buffer_clear(&saved);
        if (!bytes) e = NOTES_LIMIT;
        else if (!epd_memory_retry_wait(&retry, bytes, save_allocation_cancelled, NULL) ||
                 !note_encode(&copy, &saved)) e = NOTES_NOMEM;
    }
    if (!e) e = notes_store_save(&state->store, &copy);
    lock();
    if (!e && state->document.id == copy.id) {
        state->view.saved = copy.revision;
        note_buffer_clear(&state->saved_document); state->saved_document = saved; memset(&saved, 0, sizeof(saved));
        if (state->document.revision == copy.revision) state->view.dirty = false;
    }
    state->save_failed = e != NOTES_OK; state->view.error = e; changed(); unlock();
    if (!e) update_row(&copy); note_clear(&copy); note_buffer_clear(&saved); return e;
}
static void install_document(note_t *n)
{
    lock(); note_clear(&state->document); state->document = *n; memset(n, 0, sizeof(*n));
    state->size_revision = 0; state->edited_block = 0;
    state->view.document = state->document.id; state->view.saved = state->document.revision; state->view.dirty = false;
    note_buffer_clear(&state->saved_document); note_encode(&state->document, &state->saved_document);
    history_clear(state->undo, &state->undos); history_clear(state->redo, &state->redos);
    note_clear(&state->preview); note_clear(&state->preview_original); state->view.preview = false; changed(); unlock();
}
static void preview_load(void)
{
    char path[NOTES_PATH_SIZE]; note_buffer_t data={0};
    if(!notes_note_path(path,sizeof(path),state->document.id,"ai.bin"))return;
    storage_lock(); bool ok=notes_store_valid(&state->store)&&storage_file_recover(path); storage_unlock();
    note_t original={0},preview={0}; notes_ai_mode_t mode;
    if(ok&&!notes_store_read(&state->store,path,&data)&&notes_ai_unpack(data.data,data.size,&original,&preview,&mode)&&
       preview.id==state->document.id&&preview.revision==state->document.revision){
        lock(); note_clear(&state->preview); note_clear(&state->preview_original);
        state->preview=preview;state->preview_original=original;memset(&preview,0,sizeof(preview));memset(&original,0,sizeof(original));
        state->preview_source=state->preview.revision;state->preview_mode=mode;state->view.preview=true;changed();unlock();
    }
    note_clear(&original);note_clear(&preview);note_buffer_clear(&data);
}
static notes_error_t preview_discard(void)
{
    char path[NOTES_PATH_SIZE];
    const char *files[]={"ai.bin.old","ai.bin.tmp","ai.bin"};
    for(size_t i=0;i<3;++i){
        notes_note_path(path,sizeof(path),state->document.id,files[i]);storage_lock();
        struct stat info; bool valid=notes_store_valid(&state->store);
        bool ok=valid&&(stat(path,&info)!=0||unlink(path)==0);storage_unlock();
        if(!ok)return valid?NOTES_IO:NOTES_NO_CARD;
    }
    lock();note_clear(&state->preview); note_clear(&state->preview_original);state->view.preview=false;changed();unlock();
    return NOTES_OK;
}
bool notes_command(notes_command_t action, uint32_t id, uint32_t argument, const char *text)
{
    if (!state || (text && strlen(text) >= NOTES_PATH_SIZE)) return false;
    job_t *job = note_alloc(sizeof(*job)); if (!job) return false;
    job->action = action; job->id = id; job->argument = argument;
    if (text) strcpy(job->text, text);
    lock(); bool online = action == N_AI || action == N_TRANSCRIBE;
    bool recording_control = action == N_REC_PAUSE || action == N_REC_RESUME || action == N_REC_STOP || action == N_REC_MARK;
    bool ok = state->owner && state->view.ready && !state->stopping && !state->flushing && !state->head && !state->view.busy &&
        (!state->view.recording || recording_control) && (!state->view.online || recording_control || action == N_PLAY_STOP);
    if (online && state->network) ok = false;
    if (ok) ok = epd_service_sleep_acquire(state->owner);
    if (ok) {
        if (action == N_PLAY || action == N_PLAY_STOP) epd_media_note_intent(state->owner);
        if (!state->view.online) state->cancel = false; state->view.error = NOTES_OK; state->view.ai_error = EPD_AI_OK; state->view.speech_error = EPD_SPEECH_OK;
        state->view.message[0] = 0; state->view.export_path[0] = 0;
        if (online) { state->network = job; state->view.online = true; state->view.transcribing = action == N_TRANSCRIBE; epd_ai_cancel_init(&state->ai_cancel); epd_service_wake(state->owner); }
        else { state->head = state->tail = job; state->view.busy = true; wake_io(); }
        changed();
    }
    unlock(); if (!ok) note_free(job); return ok;
}
static int player_callback(audio_server_callback_cmt_t event, void *user, uint32_t reserved)
{
    (void)user;
    if (event == as_callback_cmd_play_to_end) state->player_end = true;
    else if (event == as_callback_cmd_user) { state->player_seconds = reserved; state->player_progress = true; }
    else if (event == as_callback_cmd_suspended) state->player_end = true;
    wake_io(); return 0;
}
static void player_close(void)
{
    if (state->player) { mp3ctrl_close(state->player); state->player = NULL; }
    epd_media_publish_state(state->owner, NULL, 0, true);
    lock(); state->view.playing = false; changed(); unlock();
}
static void record_notification(bool finished)
{
    epd_notification_t notification={0};
    lock();
    snprintf(notification.id,sizeof(notification.id),"record-%08lx-%08lx",(unsigned long)state->document.id,(unsigned long)state->record_id);
    char title[160];note_display_title(&state->document,title,sizeof(title));
    size_t len=strlen(title);if(len>=sizeof(notification.title))len=sizeof(notification.title)-1;
    while(len&&((unsigned char)title[len]&0xc0)==0x80)--len;memcpy(notification.title,title,len);
    snprintf(notification.text,sizeof(notification.text),"%s · %02lu:%02lu",
        finished?((state->view.speech_error||state->view.error||state->view.dirty)?"录音已停止，请查看保存状态":"录音已保存"):
        state->view.paused?"录音已暂停":"正在录音",
        (unsigned long)(state->view.recording_ms/60000),(unsigned long)(state->view.recording_ms/1000%60));
    notification.ongoing=!finished;unlock();epd_notification_publish(state->owner,&notification);
}
static void record_update(void)
{
    if (!state->record) return; epd_record_status_t view; epd_record_status(state->record, &view);
    lock();
    note_block_t *b = note_find(&state->document, state->record_id);
    if (b) b->milliseconds = view.milliseconds;
    if (state->view.recording_ms / 5000 != view.milliseconds / 5000 || state->view.paused != view.paused) changed();
    state->view.recording_ms = view.milliseconds; state->view.paused = view.paused; unlock();
}
static void record_close(void)
{
    if (!state->record) return;
    epd_record_pause(state->record); record_update();
    epd_speech_error_t e = epd_record_close(state->record); state->record = NULL;
    lock(); state->view.recording = false; state->view.paused = false; state->view.speech_error = e;
    ++state->document.revision; dirty(); state->deadline = 0; unlock(); save_current();
    record_notification(true);
}
static void utf8_caption(char *out, size_t capacity, const char *text)
{
    size_t size = strlen(text); if (size >= capacity) size = capacity - 1;
    while (size && ((unsigned char)text[size] & 0xc0) == 0x80) --size;
    memcpy(out, text, size); out[size] = 0;
}
static bool book_name_valid(const char *name)
{ return strlen(name) < 96 && !strchr(name, '\n') && !strchr(name, '\r'); }
static bool book_add(const char *name)
{
    if (!*name) return true;
    if (!book_name_valid(name)) return false;
    for (size_t i = 0; i < state->book_count; ++i) if (!strcmp(name, state->books[i])) return true;
    void *next = note_alloc((state->book_count + 1) * sizeof(*state->books));
    if (!next) return false;
    memcpy(next, state->books, state->book_count * sizeof(*state->books));
    note_free(state->books); state->books = next; strcpy(state->books[state->book_count++], name); catalog_changed(); return true;
}
static notes_error_t books_save(void)
{
    note_buffer_t data = {0}; lock(); bool ok = true;
    for (size_t i = 0; i < state->book_count && ok; ++i)
        ok = note_buffer_append(&data, state->books[i], strlen(state->books[i])) && note_buffer_append(&data, "\n", 1);
    unlock(); notes_error_t e = ok ? notes_store_write(&state->store, NOTES_ROOT "/notebooks.txt", data.data, data.size) : NOTES_NOMEM;
    note_buffer_clear(&data); return e;
}
static void books_load(void)
{
    state->preferences=8;
    note_buffer_t data = {0};
    notes_store_read(&state->store, NOTES_ROOT "/notebooks.txt", &data);
    lock(); note_free(state->books); state->books = NULL; state->book_count = 0;
    if (data.data && note_utf8((char *)data.data, data.size)) {
        char *p = (char *)data.data;
        while (*p) { char *end = strchr(p, '\n'); if (end) *end = 0; book_add(p); if (!end) break; p = end + 1; }
    }
    for (size_t i = 0; i < state->row_count; ++i) book_add(state->rows[i].notebook);
    catalog_changed(); unlock(); note_buffer_clear(&data);
    if(!notes_store_read(&state->store,NOTES_ROOT "/view.cfg",&data)&&data.size==1&&data.data[0]<16){
        lock();state->preferences=data.data[0];unlock();
    }
    note_buffer_clear(&data);
}
unsigned notes_preferences(void){if(!state)return 0;lock();unsigned p=state->preferences;unlock();return p;}
size_t notes_books(char (*out)[96], size_t offset, size_t count)
{
    if (!state) return 0; lock(); size_t total = state->book_count;
    if (offset < total) { if (count > total - offset) count = total - offset; memcpy(out, state->books + offset, count * sizeof(*out)); }
    unlock(); return total;
}
size_t notes_tasks(notes_task_t *out, size_t offset, size_t capacity, bool completed)
{ return notes_tasks_query(out,offset,capacity,completed,""); }
size_t notes_tasks_query(notes_task_t *out, size_t offset, size_t capacity, bool completed, const char *search)
{
    if (!state) return 0; lock(); size_t total = 0;
    for (size_t i = 0; i < state->task_count; ++i) if (state->tasks[i].checked == completed &&
        (!search||!*search||strstr(state->tasks[i].text,search)||strstr(state->tasks[i].title,search))) {
        if (total >= offset && total - offset < capacity) out[total - offset] = state->tasks[i]; ++total;
    }
    unlock(); return total;
}
static notes_error_t query_notes(const char *query, bool tasks_only)
{
    lock(); size_t count = state->row_count;
    uint32_t *ids = note_alloc((count ? count : 1) * sizeof(*ids));
    if (ids) for (size_t i = 0; i < count; ++i) ids[i] = state->rows[i].id;
    unlock(); if (!ids) return NOTES_NOMEM;
    uint32_t *found = note_alloc((count ? count : 1) * sizeof(*found)); size_t matches_count = 0;
    notes_task_t *tasks = NULL; size_t task_count = 0, capacity = 0; notes_error_t error = found ? NOTES_OK : NOTES_NOMEM;
    for (size_t i = 0; i < count && !error; ++i) {
        if (stopping(NULL)) { error = NOTES_CANCELLED; break; }
        note_t n = {0}; error = notes_store_load(&state->store, ids[i], &n);
        if (error) { note_clear(&n); break; }
        bool matched = note_matches_query(&n, query);
        for (size_t j = 0; j < n.count; ++j) {
            note_block_t *b = &n.blocks[j];
            if (tasks_only && !n.deleted && b->kind == NOTE_TASK) {
                if (task_count == capacity) {
                    size_t next = capacity + 32; void *copy = note_alloc(next * sizeof(*tasks));
                    if (!copy) { error = NOTES_NOMEM; break; }
                    memcpy(copy, tasks, task_count * sizeof(*tasks)); note_free(tasks); tasks = copy; capacity = next;
                }
                notes_task_t *task = &tasks[task_count++];
                task->note = n.id; task->block = b->id; task->checked = b->checked;
                note_display_title(&n, task->title, sizeof(task->title)); utf8_caption(task->text, sizeof(task->text), b->text);
            }
        }
        if (matched) found[matches_count++] = n.id;
        note_clear(&n); rt_thread_yield();
    }
    if (!error) {
        lock();
        if (tasks_only) { note_free(state->tasks); state->tasks = tasks; state->task_count = task_count; tasks = NULL; }
        else { note_free(state->matches); state->matches = found; state->match_count = matches_count; found = NULL; strcpy(state->query, query); }
        catalog_changed(); unlock();
    }
    note_free(tasks); note_free(ids); note_free(found); return error;
}
static notes_error_t io_command(job_t *job)
{
    notes_error_t e = save_current(); if (e && job->action != N_SCAN && job->action != N_REC_STOP) return e;
    note_t n = {0}; char path[NOTES_PATH_SIZE];
    switch (job->action) {
    case N_PREFERENCES:{
        unsigned char value=(unsigned char)(job->argument&15u);
        e=notes_store_write(&state->store,NOTES_ROOT "/view.cfg",&value,1);
        if(!e){lock();state->preferences=value;unlock();}break;
    }
    case N_DISCARD_AI:e=preview_discard();break;
    case N_QUERY:
        e = strlen(job->text) < sizeof(state->query) ? query_notes(job->text, false) : NOTES_LIMIT; break;
    case N_TASKS: e = query_notes("", true); break;
    case N_TASK_CHECK:
        e = notes_store_load(&state->store, job->id, &n);
        if (!e) {
            note_block_t *b = note_find(&n, job->argument);
            if (!b || b->kind != NOTE_TASK) e = NOTES_INVALID;
            else { b->checked = !b->checked; ++n.revision; e = notes_store_save(&state->store, &n); }
            if (!e) { update_row(&n); if (state->document.id == n.id) install_document(&n); e = query_notes("", true); }
        } break;
    case N_MOVE_BOOK:
        if (!book_name_valid(job->text)) { e = NOTES_INVALID; break; }
        e = notes_store_load(&state->store, job->id, &n);
        if (!e) { strcpy(n.notebook, job->text); ++n.revision; e = notes_store_save(&state->store, &n); }
        if (!e) { update_row(&n); if (state->document.id == n.id) install_document(&n); lock(); book_add(job->text); unlock(); e = books_save(); }
        break;
    case N_BOOK_NEW:
        lock(); bool added = book_add(job->text); unlock(); e = added && *job->text ? books_save() : NOTES_INVALID; break;
    case N_BOOK_RENAME: case N_BOOK_DELETE: {
        char previous[96]; lock();
        if (job->id >= state->book_count || (job->action==N_BOOK_RENAME&&(!*job->text || !book_add(job->text)))) { unlock(); e = NOTES_INVALID; break; }
        strcpy(previous, state->books[job->id]);
        size_t count=0; uint32_t *ids=note_alloc((state->row_count+1)*sizeof(*ids));
        if(ids)for(size_t i=0;i<state->row_count;++i)
            if(!strcmp(state->rows[i].notebook,previous))ids[count++]=state->rows[i].id;
        unlock(); if(!ids){e=NOTES_NOMEM;break;}
        for (size_t i = 0; i < count && !e; ++i) {
            if(stopping(NULL)){e=NOTES_CANCELLED;break;}
            e = notes_store_load(&state->store, ids[i], &n);
            if (!e) { strcpy(n.notebook, job->action==N_BOOK_DELETE?"":job->text);
                if(job->action==N_BOOK_DELETE&&job->argument)n.deleted=true;
                ++n.revision; e = notes_store_save(&state->store, &n); }
            if (!e) { update_row(&n); if (state->document.id == n.id) install_document(&n); }
            note_clear(&n);
        }
        note_free(ids);
        if (!e && strcmp(previous, job->text)) {
            lock(); memmove(state->books + job->id, state->books + job->id + 1, (--state->book_count - job->id) * sizeof(*state->books)); catalog_changed(); unlock();
        }
        if (!e) e = books_save(); break;
    }
    case N_BROWSE: {
        if (strncmp(job->text, "/sdcard", 7) || (job->text[7] && job->text[7] != '/') || strstr(job->text, "..")) { e = NOTES_INVALID; break; }
        storage_lock(); DIR *dir = notes_store_valid(&state->store) ? opendir(job->text) : NULL; storage_unlock();
        if (!dir) { e = NOTES_IO; break; }
        notes_file_t *files = NULL; size_t count = 0, cap = 0;
        for (;;) {
            notes_file_t file = {0};
            storage_lock(); struct dirent *entry = notes_store_valid(&state->store) ? readdir(dir) : NULL;
            if (entry) snprintf(file.name, sizeof(file.name), "%s", entry->d_name); storage_unlock();
            if (!entry) break;
            if (!strcmp(file.name, ".") || !strcmp(file.name, "..")) continue;
            if (snprintf(path, sizeof(path), "%s/%s", job->text, file.name) >= (int)sizeof(path)) continue;
            struct stat info; storage_lock(); bool ok = notes_store_valid(&state->store) && !stat(path, &info); storage_unlock();
            if (!ok) continue; file.directory = S_ISDIR(info.st_mode);
            const char *ext = strrchr(file.name, '.');
            if (!file.directory && (!ext || (job->argument ?
                (extension_compare(ext, ".png") && extension_compare(ext, ".jpg") && extension_compare(ext, ".jpeg")) :
                (extension_compare(ext, ".txt") && extension_compare(ext, ".md") && extension_compare(ext, ".zip"))))) continue;
            if (count == cap) {
                size_t next = cap + 32; notes_file_t *copy = note_alloc(next * sizeof(*copy));
                if (!copy) { e = NOTES_NOMEM; break; } memcpy(copy, files, count * sizeof(*copy)); note_free(files); files = copy; cap = next;
            }
            files[count++] = file; if (stopping(NULL)) { e = NOTES_CANCELLED; break; }
        }
        storage_lock(); closedir(dir); storage_unlock();
        if (!notes_store_valid(&state->store)) e = NOTES_NO_CARD;
        lock(); note_free(state->files); state->files = files; state->file_count = count; strcpy(state->directory, job->text); changed(); unlock(); break;
    }
    case N_IMAGE: {
        lock(); note_block_t *b = note_find(&state->document, job->id);
        bool ok = b && b->kind == NOTE_IMAGE && notes_note_path(path, sizeof(path), state->document.id, b->attachment); unlock();
        if (!ok) { e = NOTES_INVALID; break; }
        raster_decoder_t decoder = {.path=path, .cancel=stopping, .max_width=620, .max_height=880, .fd=-1};
        ok = notes_store_valid(&state->store) && raster_io_open(&decoder) && raster_decode(&decoder) && notes_store_valid(&state->store);
        if (ok) { lock(); note_free(state->gray); state->gray = decoder.gray; decoder.gray = NULL;
            state->image_width = decoder.width; state->image_height = decoder.height; changed(); unlock(); }
        if (decoder.fd >= 0) { storage_lock(); close(decoder.fd); storage_unlock(); decoder.fd = -1; }
        raster_decoder_clear(&decoder); e = ok ? NOTES_OK : NOTES_INVALID; break;
    }
    case N_SCAN:
        if (state->view.dirty) return e ? e : NOTES_BUSY;
        if (!notes_store_valid(&state->store)) {
            lock(); note_clear(&state->document); state->view.document = 0; state->row_count = 0;
            note_clear(&state->preview); note_clear(&state->preview_original); state->view.preview = false; catalog_changed(); unlock();
            note_buffer_clear(&state->saved_document);
        }
        e = notes_store_open(&state->store); if (e) break;
        lock(); state->row_count = 0; catalog_changed(); unlock(); e = notes_store_scan(&state->store, scan_row, NULL);
        if (!e) books_load(); break;
    case N_NEW:
        if (job->argument != 1 && !book_name_valid(job->text)) { e = NOTES_INVALID; break; }
        if(job->argument==1){
            uint32_t id=0;lock();for(size_t i=0;i<state->row_count;++i)
                if(!state->rows[i].deleted&&!strcmp(state->rows[i].title,"随手待办")){id=state->rows[i].id;break;}
            unlock();if(id)e=notes_store_load(&state->store,id,&n);
            if(id&&!e){if(!note_insert(&n,n.count,NOTE_TASK,""))e=NOTES_NOMEM;else e=notes_store_save(&state->store,&n);
                if(!e){update_row(&n);install_document(&n);}break;}
        }
        e = notes_store_new(&state->store, &n);
        if(!e&&job->argument==1)strcpy(n.title,"随手待办");
        if(!e&&job->argument!=1)strcpy(n.notebook,job->text);
        if (!e && !note_insert(&n, 0, job->argument==1 ? NOTE_TASK : NOTE_TEXT, "")) e = NOTES_NOMEM;
        if (!e) e = notes_store_save(&state->store, &n);
        if (!e) { update_row(&n); install_document(&n); } break;
    case N_OPEN:
        e = notes_store_load(&state->store, job->id, &n);
        if (!e) {
            uint32_t revision = n.revision;
            for (size_t i = 0; i < n.count; ++i) if (n.blocks[i].kind == NOTE_AUDIO && n.blocks[i].attachment[0] && strncmp(n.blocks[i].attachment,"attachments/import-",19)) {
                notes_note_path(path, sizeof(path), n.id, n.blocks[i].attachment);
                uint32_t ms; if (!epd_record_recover(path, state->store.session, &ms) && ms != n.blocks[i].milliseconds) {
                    n.blocks[i].milliseconds = ms; ++n.revision;
                }
            }
            if (revision != n.revision) e = notes_store_save(&state->store, &n);
            if (!e) { install_document(&n);preview_load(); }
        } break;
    case N_IMPORT:
        {const char *ext=strrchr(job->text,'.');
        if(ext&&(!extension_compare(ext,".mp3")||!extension_compare(ext,".wav"))){
            mp3_info_t info={0};storage_lock();bool valid=notes_store_valid(&state->store)&&mp3ctrl_getinfo(job->text,&info)==0;storage_unlock();
            if(!valid||!info.samplerate||!info.channels||info.total_time_in_seconds>UINT32_MAX/1000)e=NOTES_INVALID;
            else e=notes_store_import_audio(&state->store,job->text,info.total_time_in_seconds*1000,&n);
        }else if(ext&&(!extension_compare(ext,".txt")||!extension_compare(ext,".md")||!extension_compare(ext,".markdown")))e=notes_store_import(&state->store,job->text,&n);
        else e=NOTES_INVALID;}
        if (!e) { update_row(&n); install_document(&n); } break;
    case N_EXPORT:
        if(job->id)e=notes_store_load(&state->store,job->id,&n);
        else if (!document_snapshot(&n)) e=NOTES_NOMEM;
        if(e)break;
        e = notes_store_export(&state->store, &n, job->argument, path, sizeof(path));
        if (!e) { lock(); strcpy(state->view.export_path, path); unlock(); } break;
    case N_ATTACH:
        { const char *ext = strrchr(job->text, '.');
          if (!ext || (extension_compare(ext, ".png") && extension_compare(ext, ".jpg") && extension_compare(ext, ".jpeg"))) { e = NOTES_INVALID; break; } }
        if (!document_snapshot(&n)) { e = NOTES_NOMEM; break; }
        { note_block_t *b = note_insert(&n, n.count, NOTE_IMAGE, "图片");
          e = b ? notes_store_attachment(&state->store, &n, b->id, job->text) : NOTES_NOMEM; }
        if (!e) e = notes_store_save(&state->store, &n);
        if (!e) { update_row(&n); install_document(&n); } break;
    case N_DELETE: case N_RESTORE:
        e = notes_store_load(&state->store, job->id, &n);
        if (!e) { n.deleted = job->action == N_DELETE; ++n.revision; e = notes_store_save(&state->store, &n); }
        if (!e) { update_row(&n); if (state->view.document == n.id) install_document(&n); } break;
    case N_PURGE:
        e = notes_store_purge(&state->store, job->id);
        if (!e) {
            lock(); for (size_t i = 0; i < state->row_count; ++i) if (state->rows[i].id == job->id) {
                memmove(state->rows + i, state->rows + i + 1, (--state->row_count - i) * sizeof(*state->rows)); catalog_changed(); break;
            }
            if (state->document.id == job->id) { note_clear(&state->document); state->view.document = 0; } changed(); unlock();
        } break;
    case N_REC_START:
        player_close();
        if (!document_snapshot(&n)) { e = NOTES_NOMEM; break; }
        { note_block_t *b = note_insert(&n, n.count, NOTE_AUDIO, "录音");
          if (!b) { e = NOTES_NOMEM; break; }
          snprintf(b->attachment, sizeof(b->attachment), "attachments/%08lx.wav", (unsigned long)b->id);
          notes_note_path(path, sizeof(path), n.id, b->attachment); state->record_id = b->id; }
        e = notes_store_save(&state->store, &n); if (e) break;
        install_document(&n);
        { epd_speech_error_t err = epd_record_open(path, state->store.session, record_cancel, NULL, &state->record);
          lock(); state->view.speech_error = err; state->view.recording = err == EPD_SPEECH_OK;
          state->view.recording_block = state->record_id; state->view.recording_ms = 0;
          if (err) { note_remove(&state->document, state->record_id); dirty(); state->deadline = 0; }
          changed(); unlock(); if(!err)record_notification(false); }
        break;
    case N_REC_PAUSE: case N_REC_RESUME:
        if (!state->record) { e = NOTES_INVALID; break; }
        { epd_speech_error_t err = job->action == N_REC_PAUSE ? epd_record_pause(state->record) : epd_record_resume(state->record);
          record_update(); lock(); state->view.speech_error = err; ++state->document.revision; dirty(); unlock(); }
        record_notification(false);
        break;
    case N_REC_STOP: record_close(); break;
    case N_REC_MARK:
        record_update(); lock();
        {char title[96];note_block_t *audio=note_find(&state->document,state->record_id);
        snprintf(title,sizeof(title),"重点 %u",(unsigned)(audio?audio->mark_count+1:1));
        if (!note_mark(&state->document, state->record_id, state->view.recording_ms, job->text[0] ? job->text : title)) e = NOTES_NOMEM;
        else dirty(); unlock(); break;
        }
    case N_PLAY:
        player_close();
        { epd_media_t other; uint32_t revision;
          if (epd_media_read(&other, &revision) && other.playing && strcmp(other.app_id, "notes")) {
              if (!epd_media_send(other.app_id, EPD_MEDIA_PAUSE)) { e = NOTES_BUSY; break; }
              uint32_t deadline = now() + 3000;
              while (epd_media_read(&other, &revision) && other.playing && strcmp(other.app_id, "notes") &&
                     (int32_t)(now() - deadline) < 0 && !stopping(NULL)) rt_thread_mdelay(20);
              if (other.playing && strcmp(other.app_id, "notes")) { e = NOTES_BUSY; break; }
          } }
        lock();
        { note_block_t *b = note_find(&state->document, job->id);
          bool ok = b && b->kind == NOTE_AUDIO && notes_note_path(path, sizeof(path), state->document.id, b->attachment);
          unlock(); if (!ok) { e = NOTES_INVALID; break; } }
        state->player_seek = job->argument ? job->argument / 1000 :
            state->player_id == job->id && state->player_note == state->document.id ? state->player_resume : 0;
        state->player_id = job->id; state->player_note = state->document.id;
        state->player_end = false; state->player_seconds = 0; state->player_progress = false;
        state->player = mp3ctrl_open(AUDIO_TYPE_LOCAL_MUSIC, path, player_callback, NULL);
        if (!state->player || mp3ctrl_play(state->player)) {
            player_close(); e = NOTES_IO;
        } else {
            epd_media_t media = {.audio_type=AUDIO_TYPE_LOCAL_MUSIC, .capabilities=EPD_MEDIA_CAN_PAUSE, .playing=true};
            snprintf(media.title, sizeof(media.title), "%s", "笔记录音");
            epd_media_publish_state(state->owner, &media, ((uint64_t)state->player_note << 32) | job->id, false);
            lock(); state->view.playing = true;state->view.playback_block=job->id;state->view.playback_note=state->player_note;state->view.playback_ms=state->player_seek*1000; changed(); unlock();
        } break;
    case N_PLAY_STOP: state->player_resume = state->player_seconds; player_close(); break;
    case N_REVERT:
        e = notes_store_restore(&state->store, job->id, &n);
        if (!e) { n.revision = state->document.revision + 1; e = notes_store_save(&state->store, &n); }
        if (!e) { update_row(&n); install_document(&n); } break;
    case N_ADOPT:
        lock();
        if (!state->view.preview || state->document.id != state->preview.id || state->document.revision != state->preview_source) e = NOTES_CONFLICT;
        else if (!note_clone(&n, &state->document)) e = NOTES_NOMEM;
        unlock();
        if (!e) e = notes_store_checkpoint(&state->store, &n);
        if (!e) {
            lock();
            if (state->preview_mode <= AI_TYPO) {
                if (!notes_ai_apply(&n,&state->preview)) e=NOTES_INVALID;
            } else {
                for (size_t i = 0; i < state->preview.count; ++i) {
                    note_block_t *b = &state->preview.blocks[i];
                    if (state->preview_mode == AI_TASKS && !b->checked) continue;
                    if (!note_insert(&n, n.count, state->preview_mode == AI_TASKS ? NOTE_TASK : NOTE_TEXT, b->text)) { e = NOTES_NOMEM; break; }
                }
            }
            unlock(); if (!e) { ++n.revision; e = notes_store_save(&state->store, &n); }
            if (!e) { preview_discard();update_row(&n); install_document(&n); }
        } break;
    default: e = NOTES_INVALID; break;
    }
    note_clear(&n); return e;
}

static void ai_job(epd_service_t *owner, job_t *job)
{
    note_t source={0},result={0}; note_buffer_t encoded={0};
    epd_ai_error_t error=EPD_AI_OK;
    if(job->argument>AI_MINUTES || !document_snapshot(&source)){error=EPD_AI_INVALID;goto done;}
    error=notes_ai_run(owner,&source,(notes_ai_mode_t)job->argument,&state->ai_cancel,&result);
    if(!error && (stopping(NULL)||!notes_store_valid(&state->store)))error=EPD_AI_CANCELLED;
    if(!error && !notes_ai_pack(&source,&result,(notes_ai_mode_t)job->argument,&encoded))error=EPD_AI_NO_MEMORY;
    if(!error){
        char path[NOTES_PATH_SIZE];notes_note_path(path,sizeof(path),source.id,"ai.bin");
        if(stopping(NULL)||!notes_store_valid(&state->store))error=EPD_AI_CANCELLED;
        else if(notes_store_write(&state->store,path,encoded.data,encoded.size))error=EPD_AI_STORAGE;
    }
    lock();
    if(!error && (state->cancel||state->stopping||state->document.id!=source.id))error=EPD_AI_CANCELLED;
    if(!error){
        note_clear(&state->preview);note_clear(&state->preview_original);
        state->preview=result;state->preview_original=source;memset(&result,0,sizeof(result));memset(&source,0,sizeof(source));
        state->preview_source=state->preview.revision;state->preview_mode=(notes_ai_mode_t)job->argument;state->view.preview=true;
    }
    unlock();
    if(error==EPD_AI_CANCELLED && encoded.size && source.id){
        char path[NOTES_PATH_SIZE];const char *files[]={"ai.bin","ai.bin.tmp","ai.bin.old"};
        for(size_t i=0;i<3;++i){notes_note_path(path,sizeof(path),source.id,files[i]);
            storage_lock();if(notes_store_valid(&state->store))unlink(path);storage_unlock();}
    }
done:
    note_buffer_clear(&encoded);note_clear(&source);note_clear(&result);
    lock();state->view.ai_error=error;changed();unlock();
}
static void transcribe_job(job_t *job)
{
    note_t source = {0}; epd_speech_error_t e = EPD_SPEECH_INVALID;
    char *text = note_alloc(32u * 1024u); if (!text) e = EPD_SPEECH_NO_MEMORY;
    if (!text || !document_snapshot(&source)) goto done;
    note_block_t *audio = note_find(&source, job->id);
    if (!audio || audio->kind != NOTE_AUDIO || !audio->milliseconds) goto done;
    uint32_t end = audio->milliseconds, offset = 0;
    for (size_t i = 0; i < source.count; ++i) if (source.blocks[i].kind == NOTE_TRANSCRIPT && source.blocks[i].source == audio->id &&
        source.blocks[i].milliseconds > offset) offset = source.blocks[i].milliseconds;
    char path[NOTES_PATH_SIZE]; notes_note_path(path, sizeof(path), source.id, audio->attachment);
    lock(); state->view.total = (end + 119999) / 120000; state->view.progress = (offset + 119999) / 120000; changed(); unlock();
    e = EPD_SPEECH_OK;
    while (offset < end && !e && !stopping(NULL)) {
        uint32_t duration = end - offset; if (duration > 120000) duration = 120000;
        e = epd_record_transcribe(path, state->store.session, offset, duration, stopping, NULL, text, 32u * 1024u);
        if (e) break;
        lock();
        note_block_t *b = note_text_bytes(&state->document) + 2 * strlen(text) <= NOTE_TEXT_LIMIT ?
            note_insert(&state->document, state->document.count, NOTE_TRANSCRIPT, text) : NULL;
        if (!b) e = EPD_SPEECH_NO_MEMORY;
        else {
            b->source = audio->id; b->milliseconds = offset + duration; b->original = note_string(text, strlen(text));
            if (!b->original) { note_remove(&state->document, b->id); e = EPD_SPEECH_NO_MEMORY; }
            else { dirty(); state->deadline = 0; }
        }
        uint32_t commit_revision = state->document.revision;
        unlock();
        if (e) break;
        /* Wait for the I/O worker to commit this segment before requesting another. */
        for (;;) {
            lock(); bool pending = state->view.saved < commit_revision, failed = state->save_failed; unlock();
            if (failed) { e = EPD_SPEECH_STORAGE; break; }
            if (!pending) break;
            if (stopping(NULL)) { e = EPD_SPEECH_CANCELLED; break; }
            rt_thread_mdelay(20);
        }
        offset += duration;
        lock(); state->view.progress = (offset + 119999) / 120000; changed(); unlock();
    }
    if (!e && stopping(NULL)) e = EPD_SPEECH_CANCELLED;
done:
    note_free(text); note_clear(&source); lock(); state->view.speech_error = e; changed(); unlock();
}
static void idle_hold(void)
{
    if (!state->head && !state->view.busy && !state->view.online && !state->view.playing &&
        (!state->view.dirty || state->save_failed) && (!state->view.recording || state->view.paused))
        epd_service_sleep_release(state->owner);
}
static void io_run(void *unused)
{
    (void)unused;
    notes_error_t initial = state->view.dirty ?
        notes_store_resume(&state->store,state->document.id,&state->saved_document) : notes_store_open(&state->store);
    if(!initial&&state->view.dirty)initial=save_current();
    if (!initial) initial = notes_store_scan(&state->store, scan_row, NULL);
    if (!initial) books_load();
    lock(); state->view.ready = true; state->view.error = initial; changed(); unlock();
    for (;;) {
        lock(); bool stop = state->stopping, flush = state->flushing;
        job_t *job = state->head; if (job) state->head = state->tail = NULL;
        unlock();
        if (job) {
            notes_error_t error = stop ? NOTES_CANCELLED : io_command(job);
            lock(); if(job->action==N_PLAY||job->action==N_PLAY_STOP)epd_media_intent_settled(state->owner); unlock();
            note_free(job);
            lock(); state->view.error = error; state->view.busy = false; changed(); unlock();
        }
        if (state->record) {
            epd_speech_error_t error = epd_record_pump(state->record); record_update();
            epd_record_status_t status; epd_record_status(state->record, &status);
            if (error || status.finished || stop || flush) record_close();
        }
        epd_media_command_t media_command;
        while (epd_media_poll(state->owner, &media_command)) {
            if(state->player){
                state->player_resume = state->player_seconds; player_close();
            }
            lock(); if(!state->head)epd_media_intent_settled(state->owner); unlock();
        }
        if (state->player) {
            if (state->player && state->player_progress && state->player_seek) {
                uint32_t seek = state->player_seek; state->player_seek = 0;
                if (mp3ctrl_seek(state->player, seek)) { player_close(); status_error(NOTES_IO, "无法跳转录音"); }
            }
            if (state->player_end || stop || flush || !notes_store_valid(&state->store)) {
                if (state->player_end) state->player_resume = 0;
                player_close();
            }
            else { lock(); uint32_t ms = state->player_seconds * 1000;
                if (state->view.playback_ms != ms) { state->view.playback_ms = ms; changed(); } unlock(); }
        }
        lock(); bool save = state->view.dirty && !state->save_failed &&
            (stop || flush || (int32_t)(now() - state->deadline) >= 0); unlock();
        if (save) save_current();
        lock();
        if (state->flushing && (!state->view.dirty || state->save_failed) && !state->view.online && !state->view.busy && !state->view.recording && !state->view.playing)
            state->flushing = false;
        idle_hold();
        bool active = state->record && !state->view.paused;
        rt_int32_t delay = active || state->player ? rt_tick_from_millisecond(50) : state->view.dirty && !state->save_failed ? rt_tick_from_millisecond(100) : RT_WAITING_FOREVER;
        unlock(); if (stop) break;
        rt_uint32_t event; rt_event_recv(&state->io_event, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, delay, &event);
    }
}
static void io_cleanup(struct rt_thread *thread) { (void)thread; state->io_done = true; }
static void run(epd_service_t *owner)
{
    if (!state) return; lock(); state->owner = owner; state->stopping = false; state->cancel = false; state->flushing=false; unlock();
    while (!epd_service_sleep_acquire(owner)) {
        if (epd_service_cancelled(owner)) return;
        epd_service_wait(owner, RT_WAITING_FOREVER); wake_io();
    }
    state->io_stack = note_alloc(12288);
    if (!state->io_stack || rt_thread_init(&state->io_thread, "notes_io", io_run, NULL, state->io_stack, 12288, 24, 10) != RT_EOK) {
        status_error(NOTES_NOMEM, "无法启动笔记保存服务"); note_free(state->io_stack); state->io_stack = NULL; epd_service_sleep_release(owner); return;
    }
    state->io_thread.cleanup = io_cleanup; state->io_done = false;
    if (rt_thread_startup(&state->io_thread) != RT_EOK) {
        rt_thread_detach(&state->io_thread); note_free(state->io_stack); state->io_stack = NULL;
        status_error(NOTES_NOMEM, "无法启动笔记保存服务"); epd_service_sleep_release(owner); return;
    }
    state->io_started = true;
    while (!epd_service_cancelled(owner)) {
        lock(); job_t *job = state->network; state->network = NULL; unlock();
        if (job) {
            if (job->action == N_AI) ai_job(owner, job); else transcribe_job(job);
            note_free(job); lock(); state->view.online = state->view.transcribing = false; changed(); idle_hold(); unlock(); wake_io();
        }
        epd_service_wait(owner, RT_WAITING_FOREVER); wake_io();
    }
    lock(); state->stopping = true; state->cancel = true;
    if (state->network) { note_free(state->network); state->network = NULL; }
    state->view.online = state->view.transcribing = false; unlock(); wake_io();
    while (!state->io_done) rt_thread_mdelay(1);
    note_free(state->io_stack); state->io_stack = NULL; state->io_started = false;
    epd_service_sleep_release(owner);
}
static bool flush(void)
{
    if (!notes_ui_flush()) return false;
    if (!state || !state->owner) return true;
    if (rt_mutex_take(state->mutex, 0) != RT_EOK) return false;
    bool ready = !state->view.dirty && !state->view.busy && !state->view.online && !state->view.recording && !state->view.playing;
    if (!ready && !state->flushing && !state->save_failed) {
        state->flushing = true; state->cancel = true; state->deadline = 0;
        epd_ai_cancel(&state->ai_cancel); wake_io(); epd_service_wake(state->owner);
    }
    unlock(); return ready;
}
static bool ui_process(epd_service_t *owner, bool stopping_now)
{
    if (!stopping_now) {
        if (!state || notes_ui_present()) return false;
        if (rt_mutex_take(state->mutex, 0) != RT_EOK) {
            epd_service_ui_request(owner, 100); return false;
        }
        bool idle = state->view.ready && !state->head && !state->network &&
            !state->view.dirty && !state->save_failed && !state->flushing &&
            !state->view.busy && !state->view.online && !state->view.transcribing &&
            !state->view.recording && !state->view.playing && !state->view.preview && !state->record && !state->player;
        if (idle) { state->idle_stopping = true; state->stopping = true; state->view.ready = false; }
        unlock();
        if (idle) app_service_stop("notes");
        else epd_service_ui_request(owner, 100);
        return false;
    }
    if (!state->idle_stopping) notes_ui_retire();
    lock(); state->owner = NULL; state->view.ready = false; state->retired = true; changed(); unlock(); return true;
}
const epd_background_t epd_app_background = {
    .run=run, .stack_size=16384, .priority=25, .flush=flush, .ui_process=ui_process
};
void notes_runtime_init(void)
{
    if (state) return; state = note_alloc(sizeof(*state)); if (!state) return;
    state->mutex = rt_mutex_create("notes", RT_IPC_FLAG_PRIO);
    if (!state->mutex || rt_event_init(&state->io_event, "notesio", RT_IPC_FLAG_PRIO) != RT_EOK) {
        if (state->mutex) rt_mutex_delete(state->mutex); note_free(state); state = NULL; return;
    }
    epd_ai_cancel_init(&state->ai_cancel);
}
void notes_runtime_cleanup(void)
{
    if (!state) return;
    note_clear(&state->document); note_clear(&state->preview); note_clear(&state->preview_original);
    note_buffer_clear(&state->saved_document);
    history_clear(state->undo, &state->undos); history_clear(state->redo, &state->redos);
    note_free(state->rows); note_free(state->head); note_free(state->network); note_free(state->files); note_free(state->gray);
    note_free(state->tasks); note_free(state->matches); note_free(state->books);
    rt_event_detach(&state->io_event); rt_mutex_delete(state->mutex); note_free(state); state = NULL;
}
bool notes_runtime_start(void)
{
    if (!state) return false;
    if (app_service_running("notes"))
        return !state->idle_stopping && (!state->owner || !epd_service_cancelled(state->owner));
    if (state->retired) { notes_runtime_cleanup(); notes_runtime_init(); if (!state) return false; }
    void *module = dlmodule_find("notes");
    return module && app_service_start("notes", &epd_app_background, module) != NULL;
}
void notes_runtime_ui_closed(void)
{
    if (state && state->owner) epd_service_ui_request(state->owner, 0);
}
