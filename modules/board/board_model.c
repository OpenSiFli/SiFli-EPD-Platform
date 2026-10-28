#include "board_model.h"
#include "cJSON.h"
#include <limits.h>
#include <string.h>

/* Enumerating members also rejects duplicates that object lookup alone hides. */
static bool fields(const cJSON *object, const char *const *names, unsigned count, const cJSON **values)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *item = object->child; item; item = item->next)
    {
        unsigned i;
        for (i = 0; i < count; ++i)
            if (item->string && !strcmp(item->string, names[i])) break;
        if (i == count || values[i]) return false;
        values[i] = item;
    }
    return true;
}

static const char *text(const cJSON *item)
{
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static board_result_t number(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item)) return BOARD_SCHEMA;
    double value = item->valuedouble;
    if (!(value >= 0 && value <= UINT32_MAX)) return BOARD_BOUNDS;
    uint32_t n = (uint32_t)value;
    if ((double)n != value) return BOARD_SCHEMA;
    *out = n;
    return BOARD_OK;
}

static board_result_t layout_parse(const cJSON *object, board_model_t *m)
{
    static const char *const names[] = {"columns", "margin", "gap", "padding"};
    const cJSON *values[4] = {0};
    uint32_t *out[] = {&m->columns, &m->margin, &m->gap, &m->padding};
    if (!fields(object, names, 4, values)) return BOARD_SCHEMA;
    for (unsigned i = 0; i < 4; ++i)
        if (values[i])
        {
            board_result_t result = number(values[i], out[i]);
            if (result != BOARD_OK) return result;
        }
    return m->columns ? BOARD_OK : BOARD_SCHEMA;
}

static board_result_t card_parse(const cJSON *object, board_card_t *card)
{
    static const char *const names[] = {"title", "text", "summary", "empty", "height", "span"};
    const cJSON *values[6] = {0};
    if (!fields(object, names, 6, values)) return BOARD_SCHEMA;
    card->title = text(values[0]);
    card->text = text(values[1]);
    card->summary = text(values[2]);
    card->empty = values[3] ? text(values[3]) : "暂无摘要";
    card->default_empty = values[3] == NULL;
    card->span = 1;
    if (!card->title || !card->empty || (!!values[1] == !!values[2]) ||
        (values[1] && !card->text) || (values[2] && (!card->summary || !*card->summary)))
        return BOARD_SCHEMA;
    if (card->summary)
        for (const char *s = card->summary; *s; ++s)
            if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return BOARD_SCHEMA;
    board_result_t result = number(values[4], &card->height);
    if (result != BOARD_OK) return result;
    if (values[5]) result = number(values[5], &card->span);
    return result != BOARD_OK ? result : (card->height && card->span ? BOARD_OK : BOARD_SCHEMA);
}

void board_model_free(board_model_t *m)
{
    if (!m) return;
    board_allocator_t a = m->allocator;
    board_card_t *c = m->cards;
    while (c) { board_card_t *next = c->next; a.free(c, a.context); c = next; }
    cJSON_Delete(m->document);
    a.free(m, a.context);
}

board_result_t board_model_parse(const char *json, size_t length, const board_allocator_t *a, board_model_t **out)
{
    static const char *const names[] = {"version", "title", "layout", "cards"};
    const cJSON *values[4] = {0};
    if (!out) return BOARD_SCHEMA;
    *out = NULL;
    if (!json || !a || !a->alloc || !a->free) return BOARD_SCHEMA;
    if (length >= (size_t)PTRDIFF_MAX) return BOARD_BOUNDS;
    if (memchr(json, 0, length)) return BOARD_JSON;
    board_model_t *m = a->alloc(sizeof(*m), a->context);
    if (!m) return BOARD_MEMORY;
    memset(m, 0, sizeof(*m));
    m->allocator = *a;
    /* ParseWithOpts is part of the platform's exported cJSON API. */
    char *terminated = a->alloc(length + 1, a->context);
    if (!terminated) { board_model_free(m); return BOARD_MEMORY; }
    memcpy(terminated, json, length);
    terminated[length] = 0;
    m->document = cJSON_ParseWithOpts(terminated, NULL, 1);
    a->free(terminated, a->context);
    board_result_t result = BOARD_JSON;
    if (!m->document) goto fail;
    result = BOARD_SCHEMA;
    if (!fields(m->document, names, 4, values)) goto fail;
    uint32_t version;
    if (number(values[0], &version) != BOARD_OK || version != 1 ||
        !(m->title = text(values[1])) || !cJSON_IsArray(values[3])) goto fail;
    m->columns = 1; m->margin = 24; m->gap = 16; m->padding = 20;
    if (values[2])
    {
        result = layout_parse(values[2], m);
        if (result != BOARD_OK) goto fail;
    }
    board_card_t **tail = &m->cards;
    for (const cJSON *item = values[3]->child; item; item = item->next)
    {
        if (m->count == SIZE_MAX) { result = BOARD_BOUNDS; goto fail; }
        board_card_t *c = a->alloc(sizeof(*c), a->context);
        if (!c) { result = BOARD_MEMORY; goto fail; }
        memset(c, 0, sizeof(*c));
        *tail = c;
        tail = &c->next;
        ++m->count;
        result = card_parse(item, c);
        if (result != BOARD_OK) goto fail;
    }
    *out = m;
    return BOARD_OK;
fail:
    board_model_free(m);
    return result;
}

static bool command(board_emit_t emit, void *context, board_kind_t kind, size_t index,
                    uint64_t x, uint64_t y, uint64_t w, uint64_t h, unsigned font,
                    const char *text, const char *summary, const char *empty, bool default_empty)
{
    if (!emit) return true;
    board_command_t c = {kind, index, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h,
                         font, text, summary, empty, default_empty};
    return emit(&c, context);
}

static board_result_t layout_run(const board_model_t *m, int32_t width, int32_t height,
                                 board_emit_t emit, void *context)
{
    if (!m || width <= 0 || height <= 0 || (uint32_t)width > BOARD_COORD_MAX ||
        (uint32_t)height > BOARD_COORD_MAX || !m->columns) return BOARD_BOUNDS;
    uint64_t w = (uint32_t)width, h = (uint32_t)height, margin = m->margin, gap = m->gap;
    uint64_t padding = m->padding, columns = m->columns;
    if (2 * margin >= w || 2 * margin + 64 > h) return BOARD_BOUNDS;
    uint64_t inner = w - 2 * margin;
    if (inner <= 96 + gap || (columns - 1) * gap >= inner) return BOARD_BOUNDS;
    uint64_t available = inner - (columns - 1) * gap;
    if (available < columns) return BOARD_BOUNDS;
    if (!command(emit, context, BOARD_BACK, 0, margin, margin, 96, 64, 28, "返回", NULL, NULL, false) ||
        !command(emit, context, BOARD_TITLE, 0, margin + 96 + gap, margin + 8,
                 inner - 96 - gap, 48, 36, m->title, NULL, NULL, false)) return BOARD_SINK;
    uint64_t y = margin + 64 + gap, row_height = 0, col = 0;
    size_t index = 0;
    for (const board_card_t *c = m->cards; c; c = c->next, ++index)
    {
        uint64_t span = c->span;
        if (!span || span > columns) return BOARD_BOUNDS;
        if (col + span > columns) { y += row_height + gap; col = 0; row_height = 0; }
        uint64_t left = available * col / columns + col * gap;
        uint64_t right = available * (col + span) / columns + (col + span - 1) * gap;
        uint64_t cw = right - left, ch = c->height, x = margin + left;
        if (cw <= 2 * padding || ch < 2 * padding + 44 + 28 || y + ch + margin > h) return BOARD_BOUNDS;
        if (!command(emit, context, BOARD_PANEL, index, x, y, cw, ch, 0, "", NULL, NULL, false) ||
            !command(emit, context, BOARD_HEADING, index, x + padding, y + padding,
                     cw - 2 * padding, 44, 28, c->title, NULL, NULL, false) ||
            !command(emit, context, BOARD_BODY, index, x + padding, y + padding + 44,
                     cw - 2 * padding, ch - 2 * padding - 44, 28, c->text ? c->text : c->empty,
                     c->summary, c->empty, c->default_empty)) return BOARD_SINK;
        col += span;
        if (ch > row_height) row_height = ch;
    }
    return BOARD_OK;
}

board_result_t board_layout(const board_model_t *m, int32_t width, int32_t height, board_emit_t emit, void *context)
{
    board_result_t result = layout_run(m, width, height, NULL, NULL);
    return result == BOARD_OK && emit ? layout_run(m, width, height, emit, context) : result;
}

const char *board_result_text(board_result_t result)
{
    switch (result)
    {
    case BOARD_OK: return "ok";
    case BOARD_JSON: return "JSON parse failed (syntax or parser allocation)";
    case BOARD_SCHEMA: return "invalid board schema";
    case BOARD_MEMORY: return "allocation failed";
    case BOARD_BOUNDS: return "integer overflow or layout outside viewport";
    case BOARD_SINK: return "render target failed";
    default: return "unknown error";
    }
}
