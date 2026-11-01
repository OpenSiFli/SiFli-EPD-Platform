#ifndef MUSIC_LIBRARY_H
#define MUSIC_LIBRARY_H

#include "music_queue.h"

/* Retained directory index, including array capacity and owned strings. */
#define MUSIC_LIBRARY_ENTRY_LIMIT 1024u
#define MUSIC_LIBRARY_BYTE_LIMIT (256u * 1024u)

typedef enum { MUSIC_FORMAT_UNKNOWN, MUSIC_FORMAT_MP3, MUSIC_FORMAT_WAV,
               MUSIC_FORMAT_FLAC, MUSIC_FORMAT_M3U8 } music_format_t;
typedef struct {
    char *path, *name;
    bool directory;
    music_format_t format;
} music_library_entry_t;
typedef struct {
    music_library_entry_t *entries;
    size_t count, capacity, string_bytes;
} music_library_t;
typedef struct {
    char *path, *title, *artist, *album, *cover_path;
    music_format_t format;
    uint64_t duration_ms;
    uint32_t sample_rate;
    unsigned channels, bits_per_sample;
    bool duration_estimated;
    music_file_stamp_t stamp;
} music_metadata_t;
typedef struct { char **paths; size_t count, capacity; } music_favorites_t;
typedef struct music_library_scan music_library_scan_t;

/* Initialize library/metadata/favorites outputs to zero before first use.
 * Their free functions release model-allocator memory and reset the structs. */

/* ASCII case-insensitive text matching preserves all UTF-8 bytes. */
int music_library_natural_compare(const char *left, const char *right);
bool music_library_matches(const char *filename, const char *title, const char *query);
music_format_t music_library_extension(const char *path);
/* Allocates an absolute normalized local path; base is an absolute directory.
 * Network schemes, malformed UTF-8, control characters and root escapes fail. */
music_model_result_t music_library_resolve(const char *base, const char *path, char **out);
void music_library_free(music_library_t *library);
music_model_result_t music_library_scan_begin(const char *directory, music_library_scan_t **out);
/* Reads at most budget directory entries. END means complete and sorted.
 * LIMIT means the complete directory exceeds the index budget. Callers must
 * discard the incomplete list and report the error instead of showing it. */
music_model_result_t music_library_scan_step(music_library_scan_t *scan,
                                            music_library_t *out, size_t budget);
void music_library_scan_close(music_library_scan_t *scan);
music_model_result_t music_library_read(const char *directory, music_library_t *out);
music_model_result_t music_library_stat(const char *path, bool signature, music_file_stamp_t *out);
bool music_library_stamp_equal(const music_file_stamp_t *left, const music_file_stamp_t *right);
music_model_result_t music_library_probe(const char *path, music_format_t *format);
void music_metadata_free(music_metadata_t *metadata);
/* MP3 duration prefers valid first-frame Xing/Info/VBRI frame counts; otherwise
 * SDK duration is marked estimated. Encoder delay/padding is not subtracted. */
music_model_result_t music_library_metadata(const char *path, music_metadata_t *out);
/* Refresh frees stale metadata on changed/unavailable source, even if reread fails. */
music_model_result_t music_library_metadata_refresh(music_metadata_t *metadata);
/* On any error, import preserves the destination. Duplicates remain queue entries. */
music_model_result_t music_library_import_m3u8(const char *path, music_queue_t *out);
music_model_result_t music_library_export_m3u8(const char *path, const music_queue_t *queue);
void music_favorites_free(music_favorites_t *favorites);
bool music_favorites_contains(const music_favorites_t *favorites, const char *path);
music_model_result_t music_favorites_set(music_favorites_t *favorites, const char *path, bool enabled);

#endif
