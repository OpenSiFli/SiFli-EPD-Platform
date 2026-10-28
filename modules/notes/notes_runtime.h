#ifndef NOTES_RUNTIME_H
#define NOTES_RUNTIME_H
#include "notes_store.h"
#include "app_service.h"
#include "notes_ai.h"
#include "epd_record.h"

typedef enum {
    N_SCAN, N_NEW, N_OPEN, N_IMPORT, N_EXPORT, N_ATTACH,
    N_DELETE, N_RESTORE, N_PURGE, N_REC_START, N_REC_PAUSE,
    N_REC_RESUME, N_REC_STOP, N_REC_MARK, N_PLAY, N_PLAY_STOP,
    N_AI, N_TRANSCRIBE, N_ADOPT, N_REVERT, N_BROWSE, N_IMAGE,
    N_QUERY, N_TASKS, N_TASK_CHECK, N_MOVE_BOOK, N_BOOK_NEW, N_BOOK_RENAME, N_DISCARD_AI,
    N_BOOK_DELETE, N_PREFERENCES
} notes_command_t;
typedef struct {
    uint32_t revision, document, saved, card, recording_block, recording_ms, playback_ms, playback_block, playback_note;
    uint32_t catalog_revision;
    unsigned progress, total;
    bool ready, busy, dirty, recording, paused, playing, online, preview, transcribing;
    notes_error_t error;
    epd_speech_error_t speech_error;
    epd_ai_error_t ai_error;
    char message[192], export_path[NOTES_PATH_SIZE];
} notes_status_t;

void notes_runtime_init(void);
void notes_runtime_cleanup(void);
bool notes_runtime_start(void);
epd_service_t *notes_owner(void);
bool notes_command(notes_command_t action, uint32_t id, uint32_t argument, const char *text);
void notes_cancel(void);
bool notes_status(notes_status_t *out);
bool notes_document(note_t *out);
bool notes_preview(note_t *out, notes_ai_mode_t *mode);
bool notes_preview_original(note_t *out);
size_t notes_rows(notes_row_t *out, size_t offset, size_t capacity,
                 const char *search, const char *notebook, unsigned filter, unsigned sort);
/* UI mutations use a short lock and keep the document owned by the service. */
typedef bool (*notes_edit_fn)(note_t *note, void *argument);
/* A current UI mirror is updated atomically with the edited block. */
bool notes_edit_text(uint32_t id, const char *text, note_t *mirror);
bool notes_edit_metadata(const char *title, const char *notebook, note_t *mirror);
bool notes_mutate(notes_edit_fn edit, void *argument, bool coalesce);
bool notes_undo(bool redo);
void notes_save_now(void);
typedef struct { char name[256]; bool directory; } notes_file_t;
size_t notes_files(notes_file_t *files, size_t offset, size_t capacity, char *path, size_t path_size);
bool notes_take_image(uint8_t **gray, unsigned *width, unsigned *height);
typedef struct { uint32_t note, block; bool checked; char title[160], text[256]; } notes_task_t;
size_t notes_tasks(notes_task_t *out, size_t offset, size_t count, bool completed);
size_t notes_tasks_query(notes_task_t *out, size_t offset, size_t count, bool completed, const char *search);
size_t notes_books(char (*out)[96], size_t offset, size_t count);
unsigned notes_preferences(void);
bool notes_preview_select(uint32_t block, bool selected);
void notes_ui_retire(void);
bool notes_ui_flush(void);
extern const epd_background_t epd_app_background;
#endif
