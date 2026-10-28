#ifndef EPD_APP_FONT_H
#define EPD_APP_FONT_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UI thread. Call begin once when opening a reading session, before creating
 * fonts. It restores an available preferred font after storage recovery; do
 * not call it when resuming or reflowing an existing session. */
bool epd_app_reading_font_begin(void);
/* Current actual catalog ID (0 is built-in MiSans) and content revision.
 * Both are read-only snapshots. Compare both before reusing reading layout. */
uint32_t epd_app_reading_font_id(void);
uint32_t epd_app_reading_font_revision(void);

/* UI thread. family is a stable reading-font catalog ID, not a list index.
 * identity and fallback must be non-NULL output pointers. Returns
 * an owned font, or NULL on failure. A non-NULL *fallback is separately owned.
 * After removing object/draw references, destroy both with epd_app_font_destroy(). */
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback);
void epd_app_font_destroy(lv_font_t *font);
/* UI-thread idle preparation; false means the speculative cache budget is full. */
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent);
typedef struct
{
    uint64_t glyph_hits, glyph_misses;
    uint64_t prepare_failures;
    uint64_t source_bitmap_calls, source_bitmap_ticks;
    uint32_t ticks_per_second;
} epd_app_font_cache_stats_t;

/* Thread-safe, read-only cumulative counters, never reset on font/cache changes.
 * Subtract snapshots around page binding and completed drawing. Hits/misses
 * count native and derived pixel lookups outside preparation, not descriptors
 * or store-time duplicate checks. Preparation success does not imply residency.
 * source_bitmap_* counts completed native source bitmap callbacks outside
 * preparation, including failures. It excludes lock waits/cache copies; the
 * source may have its own cache, so these are not rasterization counts. Ticks
 * use ticks_per_second and may be zero for a sub-tick callback. NULL is ignored.
 * No draw wait, allocation, cache mutation or font initialization is performed. */
void epd_app_font_cache_stats(epd_app_font_cache_stats_t *out);
/* Optional derived A8 pixels in the reader's shared font LRU. The key is an
 * immutable resolved source or wrapper instance, its glyph ID and transformation
 * identity.
 * Call drop before changing/freeing/reusing that instance. UI preparation must
 * wait for drawing before generating pixels; draw callbacks must not wait.
 * Acquire/store return one owned reference in *entry, or NULL in both outputs.
 * Store copies pixels synchronously and never takes ownership of bitmap. Keep
 * source glyph references until generation/copy finishes. On failure retain
 * the immediate bitmap path. Release each acquired entry after drawing; track
 * cache references separately from immediate bitmap allocations. */
const lv_draw_buf_t *epd_app_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, lv_cache_entry_t **entry);
const lv_draw_buf_t *epd_app_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                              uint32_t style, const lv_draw_buf_t *bitmap,
                                              lv_cache_entry_t **entry);
void epd_app_font_derived_release(lv_cache_entry_t *entry);
/* UI thread only. Outstanding references remain valid until released. */
void epd_app_font_derived_drop(const lv_font_t *font);
/* Optional immutable font bytes, valid for the firmware lifetime. File-backed
 * fonts, including the block-compressed built-in font, return NULL and size 0. */
const void *epd_app_builtin_font_data(size_t *size);

#endif
