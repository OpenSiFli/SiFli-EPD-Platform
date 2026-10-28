#ifndef WORDS_LEARNING_H
#define WORDS_LEARNING_H
#include "words_fsrs.h"
#include "words_dictionary.h"

#define WORDS_COLLECTED 1u
#define WORDS_PAUSED 2u

/* Fixed-width records: identity is the original NFC headword, never a dictionary offset. */
typedef struct
{
    words_card_t card;
    char word[WORDS_KEY_SIZE];
    uint32_t content_offset, content_size, content_crc;
    int32_t first_day, rated_day;
    uint32_t day_ratings, flags;
} words_record_t;

typedef struct
{
    int64_t due;
    uint32_t hash;
    uint8_t phase, flags;
} words_index_t;

typedef struct { uint32_t added, reviewed, ratings, due, collected; } words_counts_t;
uint32_t words_word_hash(const char *word);
int words_due_select(const words_index_t *index, uint32_t count, int64_t now);
bool words_record_rate(const words_record_t *old, words_rating_t rating, int64_t now,
                       int32_t day, words_record_t *result);
void words_count_record(words_counts_t *counts, const words_record_t *record, int32_t day);
#endif
