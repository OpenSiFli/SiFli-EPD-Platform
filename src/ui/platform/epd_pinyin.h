#ifndef EPD_PINYIN_H
#define EPD_PINYIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EPD_PINYIN_MAX 63
#define EPD_PINYIN_CANDIDATE_BYTES 40
typedef struct
{
    char text[EPD_PINYIN_MAX + 1];
    size_t cursor;
} epd_pinyin_t;
typedef struct
{
    char text[EPD_PINYIN_CANDIDATE_BYTES];
    size_t consumed;
    /* Dictionary syllable boundaries: bit N marks the end of a syllable after
     * N normalized letters, excluding apostrophes, including the final syllable.
     * Display separators only between letters and where no apostrophe exists.
     * Zero for single-character candidates. */
    uint32_t boundaries;
} epd_pinyin_candidate_t;

void epd_pinyin_reset(epd_pinyin_t *state);
bool epd_pinyin_insert(epd_pinyin_t *state, char key);
bool epd_pinyin_backspace(epd_pinyin_t *state);
void epd_pinyin_move(epd_pinyin_t *state, int delta);
void epd_pinyin_consume(epd_pinyin_t *state, size_t count);
/* Stable, paged UTF-8 candidates for the beginning of the composition. Invalid
 * or unfinished input is retained until edited or explicitly discarded. Each
 * candidate records the number of composition bytes consumed on selection. */
size_t epd_pinyin_candidates(const epd_pinyin_t *state, size_t offset,
                             epd_pinyin_candidate_t *items, size_t capacity);

#endif
