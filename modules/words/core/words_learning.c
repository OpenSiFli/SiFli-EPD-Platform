#include "words_learning.h"

uint32_t words_word_hash(const char *word)
{
    uint32_t value = 2166136261u;
    while (*word) { value ^= (unsigned char)*word++; value *= 16777619u; }
    return value;
}

int words_due_select(const words_index_t *index, uint32_t count, int64_t now)
{
    int best = -1, priority = 3;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (index[i].phase == WORDS_NEW || (index[i].flags & WORDS_PAUSED) || index[i].due > now) continue;
        int p = index[i].phase == WORDS_REVIEW ? 1 : 0;
        if (p < priority || (p == priority && index[i].due < index[best].due))
        { best = (int)i; priority = p; }
    }
    return best;
}

bool words_record_rate(const words_record_t *old, words_rating_t rating, int64_t now,
                       int32_t day, words_record_t *result)
{
    if (!old || !result || day < 0 || (old->flags & WORDS_PAUSED)) return false;
    words_record_t value = *old;
    if (!words_fsrs_review(&old->card, rating, now, &value.card)) return false;
    if (old->card.phase == WORDS_NEW) value.first_day = day;
    value.day_ratings = old->rated_day == day ? old->day_ratings + 1 : 1;
    value.rated_day = day;
    *result = value;
    return true;
}

void words_count_record(words_counts_t *counts, const words_record_t *record, int32_t day)
{
    if (record->card.phase != WORDS_NEW && record->first_day == day) ++counts->added;
    if (record->day_ratings && record->rated_day == day)
    {
        if (record->first_day != day) ++counts->reviewed;
        counts->ratings += record->day_ratings;
    }
    if (record->flags & WORDS_COLLECTED) ++counts->collected;
}
