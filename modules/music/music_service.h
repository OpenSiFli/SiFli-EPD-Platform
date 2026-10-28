#ifndef MUSIC_SERVICE_H
#define MUSIC_SERVICE_H

#include "storage.h"
#include "platform/epd_app.h"

/* A page is a bounded UI snapshot, not a limit on library or queue size. */
#define MUSIC_PAGE_ROWS 7
typedef enum { MUSIC_LIST_FILES, MUSIC_LIST_QUEUE, MUSIC_LIST_FAVORITES,
               MUSIC_LIST_PLAYLISTS } music_list_kind_t;
typedef enum { MUSIC_IDLE, MUSIC_OPENING, MUSIC_PLAYING, MUSIC_PAUSED,
               MUSIC_SEEKING, MUSIC_DRAINING, MUSIC_SUSPENDED, MUSIC_ERROR } music_state_t;
typedef enum { MUSIC_OK, MUSIC_ERR_MEMORY, MUSIC_ERR_STORAGE, MUSIC_ERR_FORMAT,
               MUSIC_ERR_OPEN, MUSIC_ERR_DECODE, MUSIC_ERR_OUTPUT, MUSIC_ERR_SAVE,
               MUSIC_ERR_PATH, MUSIC_ERR_SERVICE } music_result_t;
typedef enum {
    MUSIC_CMD_LIST, MUSIC_CMD_OPEN, MUSIC_CMD_PLAY, MUSIC_CMD_PAUSE, MUSIC_CMD_STOP,
    MUSIC_CMD_PREVIOUS, MUSIC_CMD_NEXT, MUSIC_CMD_SEEK, MUSIC_CMD_MODE,
    MUSIC_CMD_APPEND, MUSIC_CMD_INSERT_NEXT, MUSIC_CMD_REMOVE, MUSIC_CMD_MOVE,
    MUSIC_CMD_CLEAR, MUSIC_CMD_FAVORITE, MUSIC_CMD_TIMER, MUSIC_CMD_VOLUME,
    MUSIC_CMD_IMPORT, MUSIC_CMD_SAVE_PLAYLIST, MUSIC_CMD_FLUSH
} music_command_kind_t;
typedef struct {
    music_command_kind_t kind;
    uint32_t id;
    int32_t value;
    music_list_kind_t list;
    size_t first;
    char path[STORAGE_PATH_MAX];
    char filter[96];
} music_command_t;
typedef struct {
    uint32_t id;
    bool directory;
    bool favorite;
    char name[256];
    char path[STORAGE_PATH_MAX];
} music_list_entry_t;
typedef struct {
    uint32_t revision, list_revision, result_revision;
    music_state_t state;
    music_result_t result;
    int detail;
    bool ready, playing, duration_estimated, volume_available, can_seek;
    /* volume is the shared media preference, in percent, before master/mute. */
    unsigned mode, volume, timer_minutes;
    uint32_t current_id, position_seconds, duration_seconds;
    char title[160], artist[96], album[160];
    char format[8];
    uint32_t sample_rate;
    unsigned channels, bits_per_sample;
    char current_path[STORAGE_PATH_MAX];
    char result_path[STORAGE_PATH_MAX];
    music_list_kind_t list;
    bool list_loading;
    size_t first, total, count;
    char directory[STORAGE_PATH_MAX], filter[96];
    music_list_entry_t entries[MUSIC_PAGE_ROWS];
} music_snapshot_t;

void music_service_init(void);
void music_service_cleanup(void);
bool music_start(void);
bool music_submit(const music_command_t *command);
void music_snapshot_read(music_snapshot_t *snapshot);
/* UI thread only. Implemented by music_view.c. */
void music_view_update(void);
void music_view_stop(void);
int music_view_open(intent_t intent);

#endif
