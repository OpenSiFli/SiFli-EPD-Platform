#ifndef MUSIC_ARTWORK_H
#define MUSIC_ARTWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads are exact, bounded and cancellable by the caller returning -1. */
typedef struct {
    void *context;
    int (*read_at)(void *, size_t, void *, size_t);
    size_t size;
} music_artwork_source_t;

typedef struct {
    size_t offset, bytes, decoded_bytes;
    bool unsynchronized;
} music_artwork_t;

/* Small streaming window; neither the audio file nor the encoded image is
 * allocated in full. Private to the music module. */
typedef struct {
    music_artwork_source_t source;
    size_t start, end, position, logical, remaining, cache_start, cache_count;
    bool unsynchronized, previous_ff;
    uint8_t cache[512];
} music_artwork_reader_t;

/* ID3v2 PIC/APIC, RIFF WAVE ID3 chunks, or FLAC PICTURE. Prefer front cover,
 * otherwise the first embedded JPEG/PNG. Linked, encrypted and compressed
 * pictures are not fetched/decoded. */
bool music_artwork_find(const music_artwork_source_t *, music_artwork_t *);
void music_artwork_reader_init(music_artwork_reader_t *,
                              const music_artwork_source_t *, const music_artwork_t *);
int music_artwork_read_at(void *, size_t, void *, size_t);

#endif
