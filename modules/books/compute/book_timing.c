/* SPDX-License-Identifier: Apache-2.0 */
#include "book_timing.h"
#include <string.h>

static book_timing_t *active;

void book_timing_resume(book_timing_t *timing) { active = timing; }

static void add(uint32_t *value, uint32_t amount)
{
    if (amount > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        active->report.saturated = 1;
    } else *value += amount;
}
static unsigned change(unsigned stage)
{
    unsigned previous = BOOK_DOCUMENT_STAGE_COUNT;
    if (active) {
        uint32_t now = active->clock();
        previous = active->stage;
        add(&active->report.ticks[previous], now - active->last);
        active->last = now;
        active->stage = stage;
    }
    return previous;
}
void book_timing_begin(book_timing_t *timing, uint32_t (*clock)(void), uint32_t hz)
{
    memset(timing, 0, sizeof(*timing));
    timing->report.tick_hz = hz;
    timing->clock = clock;
    timing->stage = BOOK_DOCUMENT_STAGE_PARSE;
    timing->report.calls[BOOK_DOCUMENT_STAGE_PARSE] = 1;
    timing->last = clock();
    active = timing;
}
unsigned book_timing_enter(unsigned stage)
{
    unsigned previous = change(stage);
    if (active) add(&active->report.calls[stage], 1);
    return previous;
}
void book_timing_leave(unsigned previous)
{
    change(previous);
}
void book_timing_end(void)
{
    change(BOOK_DOCUMENT_STAGE_COUNT);
    active = NULL;
}
