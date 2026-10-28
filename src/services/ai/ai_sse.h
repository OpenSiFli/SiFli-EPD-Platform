/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_SSE_H
#define AI_SSE_H
#include "ai_internal.h"
typedef epd_ai_error_t (*ai_sse_event_fn)(void *, const char *, const char *, size_t);
typedef struct {
    char *data;
    size_t size, capacity, line_size;
    char field[8], event[128];
    size_t field_size, event_size;
    unsigned mode, utf_left, utf_value, utf_min, bom;
    bool colon, skip_space, cr, has_data, line_started, stopped;
    ai_sse_event_fn emit;
    void *user;
} ai_sse_t;
void ai_sse_init(ai_sse_t *s, ai_sse_event_fn emit, void *user);
epd_ai_error_t ai_sse_feed(ai_sse_t *s, const void *data, size_t size);
bool ai_sse_pending(const ai_sse_t *s);
void ai_sse_clear(ai_sse_t *s);
bool ai_utf8_valid(const char *text, size_t size);
#endif
