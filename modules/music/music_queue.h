#ifndef MUSIC_QUEUE_H
#define MUSIC_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MUSIC_MODEL_OK = 0, MUSIC_MODEL_END = 1,
    MUSIC_MODEL_INVALID = -1, MUSIC_MODEL_MEMORY = -2,
    MUSIC_MODEL_IO = -3, MUSIC_MODEL_FORMAT = -4,
    MUSIC_MODEL_RANGE = -5, MUSIC_MODEL_NOT_FOUND = -6,
    MUSIC_MODEL_CHANGED = -7
} music_model_result_t;

typedef struct {
    void *(*allocate)(size_t);
    void *(*resize)(void *, size_t);
    void (*release)(void *);
} music_model_allocator_t;
/* Set only before creating any model objects; NULL restores platform defaults. */
void music_model_set_allocator(const music_model_allocator_t *allocator);
void *music_model_alloc(size_t size);
void *music_model_realloc(void *pointer, size_t size);
void music_model_free(void *pointer);
char *music_model_strdup(const char *text);

typedef enum {
    MUSIC_MODE_SEQUENCE, MUSIC_MODE_REPEAT_ALL,
    MUSIC_MODE_REPEAT_ONE, MUSIC_MODE_SHUFFLE
} music_mode_t;

typedef struct {
    uint64_t size;
    int64_t mtime;
    uint32_t signature;
    bool valid, signature_valid;
} music_file_stamp_t;

typedef struct {
    uint32_t id;
    char *path;
    music_file_stamp_t stamp;
    bool unavailable;
    uint32_t attempt_epoch;
} music_queue_entry_t;

/* Owned by one service thread. Returned entry/path pointers last until mutation. */
typedef struct {
    music_queue_entry_t *entries;
    size_t count, capacity;
    uint32_t current_id, next_id;
    uint64_t position_ms;
    music_mode_t mode;
    uint32_t random_state;
    uint32_t *shuffle;
    size_t shuffle_count, shuffle_cursor;
    uint32_t *history;
    size_t history_count, history_capacity, history_cursor;
    uint32_t attempt_epoch;
    bool attempt_active;
} music_queue_t;

void music_queue_init(music_queue_t *queue, uint32_t seed);
void music_queue_free(music_queue_t *queue);
void music_queue_clear(music_queue_t *queue);
const music_queue_entry_t *music_queue_find(const music_queue_t *queue, uint32_t id);
const music_queue_entry_t *music_queue_current(const music_queue_t *queue);
music_model_result_t music_queue_clone(music_queue_t *out, const music_queue_t *source);
music_model_result_t music_queue_insert(music_queue_t *queue, size_t index,
                                       const char *path, uint32_t *id);
music_model_result_t music_queue_append(music_queue_t *queue, const char *path, uint32_t *id);
music_model_result_t music_queue_insert_next(music_queue_t *queue, const char *path, uint32_t *id);
music_model_result_t music_queue_remove(music_queue_t *queue, uint32_t id);
music_model_result_t music_queue_move(music_queue_t *queue, uint32_t id, size_t index);
music_model_result_t music_queue_select(music_queue_t *queue, uint32_t id);
music_model_result_t music_queue_set_mode(music_queue_t *queue, music_mode_t mode);
/* natural_end=false implements manual next, including in repeat-one mode. */
music_model_result_t music_queue_next(music_queue_t *queue, bool natural_end);
music_model_result_t music_queue_previous(music_queue_t *queue);
/* Begin once for an automatic opening chain; current ID is already attempted.
 * Call failed_next after each failed open, end after success or user action.
 * A chain attempts each existing entry at most once, even in repeat modes. */
void music_queue_attempt_begin(music_queue_t *queue);
void music_queue_attempt_end(music_queue_t *queue);
music_model_result_t music_queue_failed_next(music_queue_t *queue);
bool music_queue_valid(const music_queue_t *queue);

#endif
