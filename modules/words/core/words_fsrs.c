/* FSRS-6, ported from Py-FSRS 9446cb06605c597a063aeee49f7d188d42e34dc2.
 * Copyright (c) 2022 Open Spaced Repetition. MIT; see ../LICENSE.fsrs. */
#include "words_fsrs.h"
#include <math.h>
#include <limits.h>

static const double weights[] = {
    0.212, 1.2931, 2.3065, 8.2956, 6.4133, 0.8334, 3.0194,
    0.001, 1.8722, 0.1666, 0.796, 1.4835, 0.0614, 0.2629,
    1.6483, 0.6014, 1.8729, 0.5425, 0.0912, 0.0658, 0.1542
};

static double maximum(double a, double b) { return a > b ? a : b; }
static double minimum(double a, double b) { return a < b ? a : b; }
static double difficulty_limit(double d) { return maximum(1.0, minimum(10.0, d)); }
static double initial_difficulty(unsigned rating)
{
    return weights[4] - exp(weights[5] * (rating - 1)) + 1.0;
}

static int64_t interval(double stability)
{
    /* Keep reference operation order and Python's ties-to-even rounding. */
    double factor = pow(0.9, -1.0 / weights[20]) - 1.0;
    double days = minimum(36500.0, maximum(1.0, (stability / factor) * factor));
    int64_t rounded = (int64_t)days;
    double fraction = days - (double)rounded;
    if (fraction > 0.5 || (fraction == 0.5 && (rounded & 1))) ++rounded;
    return rounded * 86400;
}

bool words_fsrs_review(const words_card_t *old, words_rating_t rating,
                       int64_t now, words_card_t *result)
{
    if (!old || !result || rating < WORDS_AGAIN || rating > WORDS_EASY ||
        old->phase > WORDS_RELEARNING || now < 0 || now > INT64_MAX - 36500LL * 86400 ||
        old->reviews == UINT32_MAX || old->lapses == UINT32_MAX) return false;
    if (old->phase != WORDS_NEW &&
        (old->last_review < 0 || now < old->last_review || !isfinite(old->stability) ||
         !isfinite(old->difficulty) || old->stability < 0.001 || old->difficulty < 1.0 ||
         old->difficulty > 10.0 ||
         (old->phase == WORDS_LEARNING && old->step > 1) ||
         (old->phase == WORDS_RELEARNING && old->step))) return false;

    words_card_t card = *old;
    if (old->phase == WORDS_NEW)
    {
        card.stability = weights[rating - 1];
        card.difficulty = difficulty_limit(initial_difficulty(rating));
        card.phase = WORDS_LEARNING;
        card.step = 0;
    }
    else
    {
        double s = old->stability, d = old->difficulty;
        int64_t days = (now - old->last_review) / 86400;
        if (!days)
        {
            double growth = exp(weights[17] * (rating - 3.0 + weights[18])) * pow(s, -weights[19]);
            if (rating != WORDS_AGAIN) growth = maximum(growth, 1.0);
            card.stability = s * growth;
        }
        else
        {
            double factor = pow(0.9, -1.0 / weights[20]) - 1.0;
            double r = pow(1.0 + factor * (double)days / s, -weights[20]);
            if (rating == WORDS_AGAIN)
                card.stability = minimum(weights[11] * pow(d, -weights[12]) *
                    (pow(s + 1.0, weights[13]) - 1.0) * exp((1.0 - r) * weights[14]),
                    s / exp(weights[17] * weights[18]));
            else
                card.stability = s * (1.0 + exp(weights[8]) * (11.0 - d) *
                    pow(s, -weights[9]) * (exp((1.0 - r) * weights[10]) - 1.0) *
                    (rating == WORDS_HARD ? weights[15] : 1.0) *
                    (rating == WORDS_EASY ? weights[16] : 1.0));
        }
        card.stability = maximum(card.stability, 0.001);
        double damped = d - (10.0 - d) * weights[6] * (rating - 3.0) / 9.0;
        card.difficulty = difficulty_limit(weights[7] * initial_difficulty(WORDS_EASY) +
                                           (1.0 - weights[7]) * damped);
    }
    if (!isfinite(card.stability) || !isfinite(card.difficulty)) return false;

    int64_t delay;
    if (card.phase == WORDS_REVIEW)
    {
        if (rating == WORDS_AGAIN)
        {
            card.phase = WORDS_RELEARNING;
            card.step = 0;
            ++card.lapses;
            delay = 600;
        }
        else delay = interval(card.stability);
    }
    else if (rating == WORDS_AGAIN)
    {
        card.step = 0;
        delay = card.phase == WORDS_LEARNING ? 60 : 600;
    }
    else if (rating == WORDS_HARD)
        delay = card.phase == WORDS_RELEARNING ? 900 : (card.step == 0 ? 330 : 600);
    else if (rating == WORDS_EASY || card.phase == WORDS_RELEARNING || card.step == 1)
    {
        card.phase = WORDS_REVIEW;
        card.step = 0;
        delay = interval(card.stability);
    }
    else
    {
        card.step = 1;
        delay = 600;
    }
    card.due = now + delay;
    card.last_review = now;
    ++card.reviews;
    *result = card;
    return true;
}
