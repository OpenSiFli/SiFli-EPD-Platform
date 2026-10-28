#ifndef WORDS_FSRS_H
#define WORDS_FSRS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { WORDS_NEW, WORDS_LEARNING, WORDS_REVIEW, WORDS_RELEARNING } words_phase_t;
typedef enum { WORDS_AGAIN = 1, WORDS_HARD, WORDS_GOOD, WORDS_EASY } words_rating_t;

typedef struct
{
    double stability, difficulty;
    int64_t last_review, due;
    uint32_t reviews, lapses;
    uint8_t phase, step;
} words_card_t;

/* UTC seconds. Default FSRS-6 weights, retention 0.9, no interval fuzzing.
 * Learning steps: 60/600 seconds; relearning: 600 seconds.
 * Invalid input (including a backwards clock) leaves the output unchanged. */
bool words_fsrs_review(const words_card_t *old, words_rating_t rating,
                       int64_t now, words_card_t *result);

#endif
