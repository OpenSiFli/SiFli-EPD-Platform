#ifndef MUSIC_COVER_H
#define MUSIC_COVER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *pixels; /* Owned, tightly packed RGB565; stride is width * 2. */
    unsigned width, height;
} music_cover_image_t;

/* Module lifecycle calls are serialized, before/after all other callers.
 * init returns 0 on success. */
int music_cover_init(void);
void music_cover_cleanup(void);
/* Service binds the callback before requests, and clears it after stop. */
void music_cover_set_wake(void (*wake)(void *), void *context);
bool music_cover_busy(void);
/* Drop only idle cache ownership; UI images are independent copies. */
void music_cover_trim(void);

/* Paths are copied. The worker prefers an embedded picture in audio_path,
 * then the optional same-directory fallback image. Both empty clears/cancels.
 * A failed request also clears the previous image. No image I/O runs here. */
bool music_cover_request(uint32_t track_id, const char *audio_path, const char *fallback);

/* Called by the service owner; rejects requests while stopping and waits for
 * the worker to exit. A later request can start a new worker. */
void music_cover_stop(void);

uint32_t music_cover_revision(void);
/* out must be empty. Success returns an independent PSRAM image, fitted within
 * 180x180 with aspect ratio preserved. It survives stop/cleanup/new requests. */
bool music_cover_copy(uint32_t track_id, music_cover_image_t *out);
void music_cover_image_free(music_cover_image_t *image);

#endif
