#ifndef EPD_FONT_STEP_PROFILE_H
#define EPD_FONT_STEP_PROFILE_H
#include "font_wire.h"
#include "epd_compute_pipe.h"
#include <string.h>

typedef struct {
    epd_font_step_wire_t totals;
    uint32_t started;
    int active;
} font_step_profile;

static inline void font_step_begin(font_step_profile *p, uint32_t now, uint32_t hz)
{
    memset(p, 0, sizeof(*p));
    p->totals.tick_hz = hz; p->started = now; p->active = 1;
}

/* Clock intervals are modulo uint32_t; a batch must finish within one wrap.
 * Run intervals still include interrupts/preemption while the fiber is live. */
static inline void font_step_before(font_step_profile *p, uint32_t op,
    uint32_t now, int final)
{
    p->totals.run_ticks += now - p->started;
    if (op == EPD_PIPE_READ) ++p->totals.reads;
    else if (op == EPD_PIPE_IDLE) ++p->totals.idles;
    else if (op == EPD_PIPE_OUTPUT) ++p->totals.outputs;
    p->started = now;
    if (final) p->active = 0;
}

static inline void font_step_after(font_step_profile *p, uint32_t op, uint32_t now)
{
    uint32_t elapsed = now - p->started;
    if (op == EPD_PIPE_READ) p->totals.read_wait_ticks += elapsed;
    else if (op == EPD_PIPE_IDLE) p->totals.idle_wait_ticks += elapsed;
    else if (op == EPD_PIPE_OUTPUT) p->totals.output_wait_ticks += elapsed;
    p->started = now;
}
#endif
