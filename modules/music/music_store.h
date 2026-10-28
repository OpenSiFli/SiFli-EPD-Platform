#ifndef MUSIC_STORE_H
#define MUSIC_STORE_H

#include "music_library.h"

#define MUSIC_STORE_VERSION 2u
typedef struct {
    music_queue_t queue;
    music_favorites_t favorites;
    char *directory;
    uint64_t snapshot_version;
    uint32_t snapshot_crc, snapshot_current_id;
    bool snapshot_dirty;
} music_store_state_t;

void music_store_init(music_store_state_t *state);
void music_store_free(music_store_state_t *state);
/* Mark changes to anything except position_ms, including selection/history,
 * shuffle order, file stamps, favorites and directory, before saving. */
void music_store_mark_changed(music_store_state_t *state);
/* A complete snapshot contains queue, IDs, position, shuffle, history,
 * favorites and directory. Position-only saves use a fixed-size progress
 * record bound to the committed snapshot's version, CRC and current ID.
 * No playback is started by load. Version 1 snapshots remain readable.
 * Save/load resolve the music installation volume and never change volumes.
 * Load replaces state only after complete validation. Save updates persistence
 * bookkeeping only after atomic commit; errors leave pending changes dirty. */
music_model_result_t music_store_save(music_store_state_t *state);
music_model_result_t music_store_load(music_store_state_t *state);
/* Recheck paths after load/before playback; changed current file resets position.
 * Missing files remain in the queue with unavailable=true. */
music_model_result_t music_store_revalidate(music_store_state_t *state);

#endif
