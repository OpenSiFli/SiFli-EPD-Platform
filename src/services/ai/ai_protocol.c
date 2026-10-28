/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_internal.h"
#include "ai_json.h"
#include "ai_sse.h"
#include <string.h>
#include <limits.h>

typedef ai_cjson json;
typedef struct ai_field {
    struct ai_field *next;
    json *value;
    size_t size, capacity;
} ai_field_t;
typedef struct ai_part {
    struct ai_part *next;
    unsigned index, sub, group, kind;
    json *native;
    ai_field_t *fields;
    char *text, *identity;
    size_t native_size, text_size, text_capacity;
    int tool;
    bool opened, done, value_done, cited;
} ai_part_t;
typedef struct ai_notice {
    struct ai_notice *next;
    epd_ai_event_t event;
    char *text, *id, *name;
} ai_notice_t;

struct ai_protocol {
    epd_ai_protocol_t protocol;
    bool stream, seen, terminal, finished, message_started, stop_reason, refusal, turn_incomplete;
    epd_ai_error_t error, native_error;
    char profile[EPD_AI_ID_SIZE], model[EPD_AI_MODEL_SIZE];
    uint32_t revision, features;
    unsigned citation_count;
    epd_ai_event_cb_t callback;
    void *user;
    epd_ai_result_t *result;
    ai_sse_t sse;
    char *body;
    size_t body_size, body_capacity, native_bytes, part_count;
    size_t text_capacity, argument_capacity[EPD_AI_MAX_TOOLS];
    ai_part_t *parts;
    ai_notice_t *head, *tail;
    bool tool_started[EPD_AI_MAX_TOOLS], tool_done[EPD_AI_MAX_TOOLS];
};

static json *get(const json *o, const char *key)
{ return ai_cjson_GetObjectItemCaseSensitive(o, key); }
static const char *str(const json *o)
{ return ai_cjson_IsString(o) ? o->valuestring : NULL; }
static const char *field(const json *o, const char *key) { return str(get(o, key)); }
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static bool object(const json *o) { return ai_cjson_IsObject(o) != 0; }
static bool array(const json *o) { return ai_cjson_IsArray(o) != 0; }

static epd_ai_error_t fail(ai_protocol_t *s, epd_ai_error_t e)
{
    if (e == EPD_AI_INVALID) e = EPD_AI_PROTOCOL;
    if (e != EPD_AI_OK && s->error == EPD_AI_OK) s->error = e;
    if (s->error != EPD_AI_OK) {
        s->result->complete = false; s->result->replayable = false;
        s->result->error = s->error;
        if (!s->terminal) s->result->usage.valid &= ~(uint32_t)(EPD_AI_USAGE_OUTPUT | EPD_AI_USAGE_REASONING);
    }
    return s->error;
}

static bool valid_strings(const json *o)
{
    const json *p;
    if (!o) return false;
    if (o->string && !ai_utf8_valid(o->string, strlen(o->string))) return false;
    if (ai_cjson_IsString(o) && !ai_utf8_valid(o->valuestring, strlen(o->valuestring))) return false;
    for (p = o->child; p; p = p->next) if (!valid_strings(p)) return false;
    return true;
}

static bool index_value(const json *o, unsigned *v)
{
    if (!ai_cjson_IsNumber(o) || o->valuedouble < 0 || o->valuedouble > 4095 ||
        o->valuedouble != (double)(unsigned)o->valuedouble) return false;
    *v = (unsigned)o->valuedouble; return true;
}

static void fixed_string(char *dst, size_t capacity, const char *src)
{
    size_t n;
    if (!src) return;
    n = strlen(src); if (n >= capacity) n = capacity - 1;
    while (n && !ai_utf8_valid(src, n)) --n;
    memcpy(dst, src, n); dst[n] = 0;
}

static epd_ai_error_t append(char **dst, size_t *size, size_t *capacity,
                             const char *text, size_t n, size_t limit)
{
    if (n > limit || *size > limit - n) return EPD_AI_LIMIT;
    if (!n) return EPD_AI_OK;
    if (*size + n + 1 > *capacity) {
        size_t next = *capacity ? *capacity * 2 : 256;
        char *p;
        if (next < *size + n + 1) next = *size + n + 1;
        if (next > limit + 1) next = limit + 1;
        p = ai_realloc(*dst, next);
        if (!p) return EPD_AI_NO_MEMORY;
        *dst = p; *capacity = next;
    }
    memcpy(*dst + *size, text, n); *size += n; (*dst)[*size] = 0;
    return EPD_AI_OK;
}

static ai_part_t *part(ai_protocol_t *s, unsigned group, unsigned index, unsigned sub, bool create)
{
    ai_part_t *p;
    for (p = s->parts; p; p = p->next)
        if (p->group == group && p->index == index && p->sub == sub) return p;
    if (!create) return NULL;
    if (s->part_count == 4096) { fail(s, EPD_AI_LIMIT); return NULL; }
    p = ai_calloc(1, sizeof(*p));
    if (!p) { fail(s, EPD_AI_NO_MEMORY); return NULL; }
    p->group = group; p->index = index; p->sub = sub; p->tool = -1;
    p->next = s->parts; s->parts = p; ++s->part_count;
    return p;
}

/* Match the unformatted JSON writer, including escaped control characters. */
static size_t escaped_size(const char *text)
{
    size_t n = 0;
    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        n += c == '"' || c == '\\' || c == '\b' || c == '\f' ||
             c == '\n' || c == '\r' || c == '\t' ? 2 : c < 32 ? 6 : 1;
    }
    return n;
}

static size_t native_size(const json *o)
{
    size_t n;
    if (str(o)) return escaped_size(str(o)) + 2;
    if (array(o) || object(o)) {
        n = 2;
        for (const json *v = o->child; v; v = v->next) {
            n += native_size(v) + (v->next ? 1 : 0);
            if (object(o)) n += escaped_size(v->string) + 3;
        }
        return n;
    }
    /* Numbers use the same rounding/locale rules as the final writer. */
    char scalar[64];
    return ai_cjson_PrintPreallocated((json *)o, scalar, sizeof(scalar), 0) ?
           strlen(scalar) : EPD_AI_NATIVE_LIMIT + 1u;
}

static bool native_shape(const json *o, unsigned depth, size_t *nodes)
{
    if (++*nodes > 4096) return false;
    if (array(o) || object(o)) {
        if (depth == 32) return false;
        for (const json *v = o->child; v; v = v->next)
            if (!native_shape(v, depth + 1, nodes)) return false;
    }
    return true;
}

static epd_ai_error_t check_native(const json *o)
{
    size_t nodes = 0;
    return native_shape(o, 0, &nodes) ? EPD_AI_OK : EPD_AI_LIMIT;
}

static epd_ai_error_t resize_native(ai_protocol_t *s, ai_part_t *p, size_t old, size_t next)
{
    if (next > EPD_AI_NATIVE_LIMIT || s->native_bytes - old > EPD_AI_NATIVE_LIMIT - next)
        return EPD_AI_LIMIT;
    s->native_bytes = s->native_bytes - old + next;
    p->native_size = p->native_size - old + next;
    return EPD_AI_OK;
}

static void fields_clear(ai_part_t *p)
{
    while (p->fields) {
        ai_field_t *f = p->fields; p->fields = f->next; ai_free(f);
    }
}

/* Retain an event object's children; its envelope remains owned by the parser. */
static epd_ai_error_t save(ai_protocol_t *s, ai_part_t *p, json *o)
{
    if (o == p->native || (p->native && ai_cjson_Compare(p->native, o, 1))) return EPD_AI_OK;
    size_t n = native_size(o);
    epd_ai_error_t e = check_native(o);
    if (e) return e;
    e = resize_native(s, p, p->native_size, n);
    if (e) return e;
    json *owned = p->native ? p->native : ai_cjson_CreateObject();
    if (!owned) return EPD_AI_NO_MEMORY;
    fields_clear(p); ai_cjson_Delete(owned->child);
    owned->child = o->child; o->child = NULL; p->native = owned;
    return EPD_AI_OK;
}

static epd_ai_error_t put(json *o, const char *key, json *value)
{
    bool ok;
    if (!value) return EPD_AI_NO_MEMORY;
    ok = get(o, key) ? ai_cjson_ReplaceItemInObjectCaseSensitive(o, key, value) :
                      ai_cjson_AddItemToObject(o, key, value);
    if (!ok) { ai_cjson_Delete(value); return EPD_AI_NO_MEMORY; }
    return EPD_AI_OK;
}

static epd_ai_error_t put_native(ai_protocol_t *s, ai_part_t *p, json *o,
                                 const char *key, json *value)
{
    json *old = get(o, key);
    epd_ai_error_t e;
    if (!value) return EPD_AI_NO_MEMORY;
    size_t n = native_size(value);
    if (!old) n += escaped_size(key) + 3 + (o->child ? 1 : 0);
    e = resize_native(s, p, old ? native_size(old) : 0, n);
    if (e) { ai_cjson_Delete(value); return e; }
    return put(o, key, value);
}

static epd_ai_error_t add_native(ai_protocol_t *s, ai_part_t *p, json *a, const json *value)
{
    epd_ai_error_t e = resize_native(s, p, 0, native_size(value) + (a->child ? 1 : 0));
    if (e) return e;
    json *copy = ai_cjson_Duplicate(value, 1);
    if (!copy) return EPD_AI_NO_MEMORY;
    if (!ai_cjson_AddItemToArray(a, copy)) { ai_cjson_Delete(copy); return EPD_AI_NO_MEMORY; }
    return EPD_AI_OK;
}

static epd_ai_error_t append_field(ai_protocol_t *s, ai_part_t *p, json *o,
                                   const char *key, const char *value, size_t limit)
{
    json *v = get(o, key);
    ai_field_t *f;
    epd_ai_error_t e;
    if (v && !str(v) && !ai_cjson_IsNull(v)) return EPD_AI_PROTOCOL;
    if (!str(v)) {
        e = put_native(s, p, o, key, ai_cjson_CreateString("")); if (e) return e;
        v = get(o, key);
    }
    for (f = p->fields; f && f->value != v; f = f->next) {}
    if (!f) {
        f = ai_calloc(1, sizeof(*f)); if (!f) return EPD_AI_NO_MEMORY;
        f->value = v; f->size = strlen(v->valuestring); f->capacity = f->size + 1;
        f->next = p->fields; p->fields = f;
    }
    e = resize_native(s, p, 0, escaped_size(value)); if (e) return e;
    return append(&v->valuestring, &f->size, &f->capacity, value, strlen(value), limit);
}

static void notices_clear(ai_protocol_t *s)
{
    while (s->head) {
        ai_notice_t *p = s->head; s->head = p->next;
        ai_free(p->text); ai_free(p->id); ai_free(p->name); ai_free(p);
    }
    s->tail = NULL;
}

static epd_ai_error_t notice(ai_protocol_t *s, epd_ai_event_kind_t kind,
                             const char *text, size_t size, size_t tool)
{
    ai_notice_t *p;
    if (!s->callback) return EPD_AI_OK;
    p = ai_calloc(1, sizeof(*p)); if (!p) return EPD_AI_NO_MEMORY;
    p->event.kind = kind; p->event.text_size = size; p->event.tool_index = tool;
    p->event.usage = s->result->usage;
    if (text) { p->text = ai_strndup(text, size); if (!p->text) goto oom; }
    if (kind == EPD_AI_EVENT_TOOL_START || kind == EPD_AI_EVENT_TOOL_ARGUMENTS || kind == EPD_AI_EVENT_TOOL_END) {
        epd_ai_tool_call_t *t = &s->result->tools[tool];
        if (t->id) { p->id = ai_strndup(t->id, strlen(t->id)); if (!p->id) goto oom; }
        if (t->name) { p->name = ai_strndup(t->name, strlen(t->name)); if (!p->name) goto oom; }
    }
    p->event.text = p->text; p->event.call_id = p->id; p->event.tool_name = p->name;
    if (s->tail) s->tail->next = p; else s->head = p;
    s->tail = p; return EPD_AI_OK;
oom:
    ai_free(p->text); ai_free(p->id); ai_free(p->name); ai_free(p); return EPD_AI_NO_MEMORY;
}

static epd_ai_error_t publish(ai_protocol_t *s)
{
    ai_notice_t *p;
    for (p = s->head; p; p = p->next)
        if (!s->callback(&p->event, s->user)) { notices_clear(s); return EPD_AI_CANCELLED; }
    notices_clear(s); return EPD_AI_OK;
}

static epd_ai_error_t citations(ai_protocol_t *s, ai_part_t *part, const json *items)
{
    if (part->cited || !items || ai_cjson_IsNull(items)) return EPD_AI_OK;
    if (!array(items)) return EPD_AI_PROTOCOL;
    part->cited = true;
    for (const json *c = items->child; c; c = c->next) {
        const char *type = field(c, "type"), *url = field(c, "url"), *title = field(c, "title");
        if (!eq(type, "url_citation") && !eq(type, "web_search_result_location")) continue;
        if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) continue;
        size_t n = strlen(url), t = title ? strlen(title) : 0;
        if (!n || n > 2048 || t > 512 || s->citation_count >= 32) continue;
        bool valid = true;
        for (size_t i = 0; i < n; ++i)
            if ((unsigned char)url[i] <= 32 || url[i] == '<' || url[i] == '>') valid = false;
        if (!valid) continue;
        if (!t) { title = url; t = n; }
        char *link = ai_alloc(t * 2 + n + 16);
        if (!link) return EPD_AI_NO_MEMORY;
        size_t used = 0;
        memcpy(link, "\n\n[", 3); used = 3;
        for (size_t i = 0; i < t; ++i) {
            unsigned char ch = (unsigned char)title[i];
            if (ch == '[' || ch == ']' || ch == '\\') link[used++] = '\\';
            link[used++] = ch < 32 ? ' ' : (char)ch;
        }
        memcpy(link + used, "](<", 3); used += 3;
        memcpy(link + used, url, n); used += n;
        memcpy(link + used, ">)\n\n", 4); used += 4; link[used] = 0;
        epd_ai_error_t e = append(&s->result->text, &s->result->text_size, &s->text_capacity, link, used, EPD_AI_TEXT_LIMIT);
        if (!e) e = notice(s, EPD_AI_EVENT_TEXT, link, used, 0);
        ai_free(link);
        if (e) return e;
        ++s->citation_count;
    }
    return EPD_AI_OK;
}

static epd_ai_error_t search_error(ai_protocol_t *s, const char *code)
{
    if (!code || !*code) code = "web_search_failed";
    fixed_string(s->result->error_code, sizeof(s->result->error_code), code);
    return notice(s, EPD_AI_EVENT_SEARCH_ERROR, code, strlen(code), 0);
}

/* A final snapshot may extend a prefix, but cannot rewrite already published text. */
static epd_ai_error_t text_update(ai_protocol_t *s, ai_part_t *p, const char *text, bool full)
{
    size_t n, skip = 0;
    epd_ai_error_t e;
    if (!text) return EPD_AI_PROTOCOL;
    n = strlen(text);
    if (full) {
        if (n < p->text_size || (p->text_size && memcmp(text, p->text, p->text_size))) return EPD_AI_PROTOCOL;
        skip = p->text_size;
    } else if (p->done) return EPD_AI_PROTOCOL;
    e = append(&s->result->text, &s->result->text_size, &s->text_capacity, text + skip, n - skip, EPD_AI_TEXT_LIMIT);
    if (e != EPD_AI_OK) return e;
    e = append(&p->text, &p->text_size, &p->text_capacity, text + skip, n - skip, EPD_AI_TEXT_LIMIT);
    if (e != EPD_AI_OK) return e;
    return n > skip ? notice(s, EPD_AI_EVENT_TEXT, text + skip, n - skip, 0) : EPD_AI_OK;
}

static epd_ai_error_t tool_identity(ai_protocol_t *s, ai_part_t *p, const char *id, const char *name)
{
    epd_ai_tool_call_t *t;
    size_t i;
    if (p->tool < 0) {
        if (s->result->tool_count == EPD_AI_MAX_TOOLS) return EPD_AI_LIMIT;
        p->tool = (int)s->result->tool_count++;
    }
    t = &s->result->tools[p->tool];
    if (id && *id) {
        if (t->id && strcmp(t->id, id)) return EPD_AI_PROTOCOL;
        for (i = 0; i < s->result->tool_count; ++i)
            if (i != (size_t)p->tool && eq(s->result->tools[i].id, id)) return EPD_AI_PROTOCOL;
        if (!t->id) { t->id = ai_strndup(id, strlen(id)); if (!t->id) return EPD_AI_NO_MEMORY; }
    }
    if (name && *name) {
        size_t n = strlen(name), j;
        if (n > 64) return EPD_AI_PROTOCOL;
        for (j = 0; j < n; ++j) {
            unsigned char c = (unsigned char)name[j];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return EPD_AI_PROTOCOL;
        }
        if (t->name && strcmp(t->name, name)) return EPD_AI_PROTOCOL;
        if (!t->name) { t->name = ai_strndup(name, strlen(name)); if (!t->name) return EPD_AI_NO_MEMORY; }
    }
    if (!s->tool_started[p->tool] && t->id && t->name) {
        epd_ai_error_t e = notice(s, EPD_AI_EVENT_TOOL_START, NULL, 0, (size_t)p->tool);
        if (e != EPD_AI_OK) return e;
        s->tool_started[p->tool] = true;
    }
    return EPD_AI_OK;
}

static epd_ai_error_t tool_arguments(ai_protocol_t *s, ai_part_t *p, const char *args, bool full, bool end)
{
    epd_ai_tool_call_t *t;
    size_t n, skip = 0;
    epd_ai_error_t e;
    if (p->tool < 0 || !args) return EPD_AI_PROTOCOL;
    t = &s->result->tools[p->tool]; n = strlen(args);
    if (full) {
        if (n < t->arguments_size || (t->arguments_size && memcmp(t->arguments, args, t->arguments_size))) return EPD_AI_PROTOCOL;
        skip = t->arguments_size;
    } else if (s->tool_done[p->tool]) return EPD_AI_PROTOCOL;
    e = append(&t->arguments, &t->arguments_size, &s->argument_capacity[p->tool], args + skip, n - skip, EPD_AI_ARGUMENT_LIMIT);
    if (e != EPD_AI_OK) return e;
    if (n > skip) {
        if (!s->tool_started[p->tool]) return EPD_AI_PROTOCOL;
        e = notice(s, EPD_AI_EVENT_TOOL_ARGUMENTS, args + skip, n - skip, (size_t)p->tool);
        if (e != EPD_AI_OK) return e;
    }
    if (end && !s->tool_done[p->tool]) {
        json *parsed;
        if (!t->id || !t->name || !t->arguments) return EPD_AI_PROTOCOL;
        parsed = ai_json_parse(t->arguments, t->arguments_size, &e);
        if (!parsed) {
            if (s->stream && !s->stop_reason && (e == EPD_AI_INVALID || e == EPD_AI_PROTOCOL)) {
                s->native_error = EPD_AI_PROTOCOL; return EPD_AI_OK;
            }
            return e == EPD_AI_OK ? EPD_AI_PROTOCOL : e;
        }
        if (!object(parsed)) e = EPD_AI_PROTOCOL;
        ai_cjson_Delete(parsed);
        if (e != EPD_AI_OK) return e;
        s->tool_done[p->tool] = true;
        return notice(s, EPD_AI_EVENT_TOOL_END, NULL, 0, (size_t)p->tool);
    }
    return EPD_AI_OK;
}

static epd_ai_error_t usage_number(const json *u, const char *key, uint32_t bit,
                                  uint32_t *valid, uint64_t *dst)
{
    json *v = get(u, key);
    if (!v || ai_cjson_IsNull(v)) return EPD_AI_OK;
    if (!ai_cjson_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 9007199254740991.0 ||
        v->valuedouble != (double)(uint64_t)v->valuedouble) return EPD_AI_PROTOCOL;
    *dst = (uint64_t)v->valuedouble; *valid |= bit; return EPD_AI_OK;
}

static epd_ai_error_t usage(ai_protocol_t *s, const json *u)
{
    epd_ai_usage_t v = s->result->usage;
    epd_ai_error_t e;
    const json *details;
    if (!u || ai_cjson_IsNull(u)) return EPD_AI_OK;
    if (!object(u)) return EPD_AI_PROTOCOL;
#define COUNT(o,k,b,d) do { e = usage_number(o,k,b,&v.valid,&v.d); if (e) return e; } while (0)
    COUNT(u, s->protocol == EPD_AI_CHAT ? "prompt_tokens" : "input_tokens", EPD_AI_USAGE_INPUT, input);
    COUNT(u, s->protocol == EPD_AI_CHAT ? "completion_tokens" : "output_tokens", EPD_AI_USAGE_OUTPUT, output);
    if (s->protocol == EPD_AI_MESSAGES) COUNT(u, "cache_read_input_tokens", EPD_AI_USAGE_CACHED, cached);
    if (s->protocol == EPD_AI_CHAT) COUNT(u, "prompt_cache_hit_tokens", EPD_AI_USAGE_CACHED, cached);
    details = get(u, s->protocol == EPD_AI_CHAT ? "prompt_tokens_details" : "input_tokens_details");
    if (details && !ai_cjson_IsNull(details) && !object(details)) return EPD_AI_PROTOCOL;
    COUNT(details, "cached_tokens", EPD_AI_USAGE_CACHED, cached);
    details = get(u, s->protocol == EPD_AI_CHAT ? "completion_tokens_details" : "output_tokens_details");
    if (details && !ai_cjson_IsNull(details) && !object(details)) return EPD_AI_PROTOCOL;
    COUNT(details, "reasoning_tokens", EPD_AI_USAGE_REASONING, reasoning);
#undef COUNT
    if (v.valid == s->result->usage.valid && v.input == s->result->usage.input &&
        v.output == s->result->usage.output && v.cached == s->result->usage.cached &&
        v.reasoning == s->result->usage.reasoning) return EPD_AI_OK;
    s->result->usage = v;
    return notice(s, EPD_AI_EVENT_USAGE, NULL, 0, 0);
}

static epd_ai_error_t provider_error(ai_protocol_t *s, const json *o)
{
    const char *code = field(o, "code");
    if (!code) code = field(o, "type");
    fixed_string(s->result->error_code, sizeof(s->result->error_code), code);
    if (eq(code, "rate_limit_error") || eq(code, "rate_limit_exceeded")) return EPD_AI_RATE_LIMIT;
    if (eq(code, "authentication_error") || eq(code, "invalid_api_key")) return EPD_AI_AUTH;
    if (eq(code, "invalid_request_error")) return EPD_AI_INVALID;
    if (eq(code, "permission_error")) return EPD_AI_UNAUTHORIZED;
    return EPD_AI_SERVER;
}

static epd_ai_error_t finish_reason(ai_protocol_t *s, const char *reason)
{
    epd_ai_finish_t v;
    if (!reason) return EPD_AI_PROTOCOL;
    if (eq(reason, "stop") || eq(reason, "end_turn") || eq(reason, "stop_sequence")) v = EPD_AI_FINISH_STOP;
    else if (eq(reason, "tool_calls") || eq(reason, "tool_use")) v = EPD_AI_FINISH_TOOLS;
    else if (eq(reason, "length") || eq(reason, "max_tokens") || eq(reason, "max_output_tokens") ||
             eq(reason, "model_context_window_exceeded")) v = EPD_AI_FINISH_LENGTH;
    else if (eq(reason, "content_filter") || eq(reason, "refusal")) v = EPD_AI_FINISH_REFUSAL;
    else if (s->protocol == EPD_AI_MESSAGES && eq(reason, "pause_turn")) v = EPD_AI_FINISH_PAUSE;
    else return EPD_AI_UNSUPPORTED;
    if (s->stop_reason && s->result->finish != v) return EPD_AI_PROTOCOL;
    s->stop_reason = true; s->result->finish = v; return EPD_AI_OK;
}

static epd_ai_error_t envelope(ai_protocol_t *s, const json *data)
{
    json *root = ai_cjson_CreateObject();
    char *text = NULL;
    epd_ai_error_t e = EPD_AI_NO_MEMORY;
    if (!root) return e;
    if (!ai_cjson_AddNumberToObject(root, "version", 1) ||
        !ai_cjson_AddNumberToObject(root, "protocol", s->protocol) ||
        !ai_cjson_AddStringToObject(root, "profile", s->profile) ||
        !ai_cjson_AddNumberToObject(root, "revision", s->revision) ||
        !ai_cjson_AddStringToObject(root, "model", s->model)) goto done;
    e = put(root, "data", array(data) ? ai_cjson_CreateArrayReference(data->child) :
                                      ai_cjson_CreateObjectReference(data->child));
    if (e) goto done;
    text = ai_json_print(root, &e);
    if (!text) goto done;
    if (strlen(text) > EPD_AI_NATIVE_LIMIT) { e = EPD_AI_LIMIT; goto done; }
    e = (epd_ai_error_t)ai_json_preflight(text, strlen(text));
    if (e) goto done;
    ai_free(s->result->native); s->result->native = text;
    s->result->native_size = strlen(text); text = NULL; e = EPD_AI_OK;
done:
    ai_free(text); ai_cjson_Delete(root); return e;
}

static epd_ai_error_t terminal(ai_protocol_t *s, const json *native)
{
    size_t i;
    ai_part_t *p;
    epd_ai_error_t e;
    bool truncated = s->turn_incomplete || s->result->finish == EPD_AI_FINISH_LENGTH;
    if (!s->stop_reason) return EPD_AI_PROTOCOL;
    if (s->result->finish == EPD_AI_FINISH_TOOLS && !s->result->tool_count) return EPD_AI_PROTOCOL;
    if (s->result->tool_count && s->result->finish != EPD_AI_FINISH_TOOLS && !truncated) return EPD_AI_PROTOCOL;
    if (!truncated)
        for (i = 0; i < s->result->tool_count; ++i)
            if (!s->tool_done[i]) return EPD_AI_PROTOCOL;
    if (s->native_error && !truncated) return s->native_error;
    /* Prefix trackers are no longer needed while producing the final record. */
    for (p = s->parts; p; p = p->next) {
        fields_clear(p); ai_free(p->text); p->text = NULL;
        p->text_size = p->text_capacity = 0;
        /* Responses supplies a separate final event tree. */
        if (s->protocol == EPD_AI_RESPONSES) {
            ai_cjson_Delete(p->native); p->native = NULL;
        }
    }
    if (!s->native_error) { e = envelope(s, native); if (e != EPD_AI_OK) return e; }
    for (p = s->parts; p; p = p->next) {
        ai_cjson_Delete(p->native); p->native = NULL; p->native_size = 0;
    }
    s->native_bytes = 0;
    s->terminal = true; s->sse.stopped = true;
    /* A length-limited turn has a terminal, but is not a complete history item. */
    s->result->complete = !truncated && s->result->finish != EPD_AI_FINISH_PAUSE;
    s->result->replayable = !truncated;
    return EPD_AI_OK;
}

static epd_ai_error_t chat_message(ai_protocol_t *s, const json *message, bool end)
{
    const json *tools, *t, *v;
    const char *text;
    unsigned index = 0;
    epd_ai_error_t e;
    ai_part_t *p;
    if (!object(message) || !eq(field(message, "role"), "assistant")) return EPD_AI_PROTOCOL;
    for (v = message->child; v; v = v->next) {
        if (!eq(v->string, "role") && !eq(v->string, "content") &&
            !eq(v->string, "reasoning_content") && !eq(v->string, "tool_calls") &&
            !eq(v->string, "refusal") && !eq(v->string, "annotations")) return EPD_AI_UNSUPPORTED;
    }
    v = get(message, "reasoning_content");
    if (v && !str(v)) return EPD_AI_UNSUPPORTED;
    v = get(message, "content"); text = str(v);
    if (v && !text && !ai_cjson_IsNull(v)) return EPD_AI_UNSUPPORTED;
    p = part(s, 1, 0, 0, true); if (!p) return s->error;
    if (text) { e = text_update(s, p, text, true); if (e) return e; }
    v = get(message, "refusal"); text = str(v);
    if (v && !text && !ai_cjson_IsNull(v)) return EPD_AI_PROTOCOL;
    if (text && *text) {
        s->refusal = true;
        p = part(s, 1, 0, 1, true); if (!p) return s->error;
        e = text_update(s, p, text, true); if (e) return e;
    }
    tools = get(message, "tool_calls");
    if (tools && !array(tools) && !ai_cjson_IsNull(tools)) return EPD_AI_PROTOCOL;
    for (t = tools ? tools->child : NULL; t; t = t->next, ++index) {
        const json *fn = get(t, "function");
        const char *args = field(fn, "arguments");
        if (index >= EPD_AI_MAX_TOOLS) return EPD_AI_LIMIT;
        if (!object(t) || !eq(field(t, "type"), "function")) return EPD_AI_UNSUPPORTED;
        if (!object(fn) || !field(t, "id") || !field(fn, "name") || !args) return EPD_AI_PROTOCOL;
        p = part(s, 2, index, 0, true); if (!p) return s->error;
        e = tool_identity(s, p, field(t, "id"), field(fn, "name")); if (e) return e;
        e = tool_arguments(s, p, args, true, end); if (e) return e;
    }
    if (s->result->tool_count > index) return EPD_AI_PROTOCOL;
    return EPD_AI_OK;
}

static epd_ai_error_t chat_delta(ai_protocol_t *s, ai_part_t *owner, json *message, const json *delta)
{
    const json *v;
    epd_ai_error_t e = EPD_AI_OK;
    if (!object(delta)) return EPD_AI_PROTOCOL;
    for (v = delta->child; v; v = v->next) {
        if (eq(v->string, "role")) {
            if (!eq(str(v), "assistant")) return EPD_AI_PROTOCOL;
        } else if (eq(v->string, "content") || eq(v->string, "reasoning_content") || eq(v->string, "refusal")) {
            ai_part_t *p;
            if (ai_cjson_IsNull(v)) continue;
            if (!str(v)) return EPD_AI_UNSUPPORTED;
            e = append_field(s, owner, message, v->string, str(v), eq(v->string, "reasoning_content") ? EPD_AI_NATIVE_LIMIT : EPD_AI_TEXT_LIMIT);
            if (e) return e;
            if (eq(v->string, "reasoning_content")) continue;
            if (eq(v->string, "refusal") && *str(v)) s->refusal = true;
            p = part(s, 1, 0, eq(v->string, "refusal") ? 1 : 0, true);
            if (!p) return s->error;
            e = text_update(s, p, str(v), false); if (e) return e;
        } else if (eq(v->string, "tool_calls")) {
            const json *call;
            json *calls = get(message, "tool_calls");
            if (!array(v)) return EPD_AI_PROTOCOL;
            if (!calls) {
                e = put_native(s, owner, message, "tool_calls", ai_cjson_CreateArray()); if (e) return e;
                calls = get(message, "tool_calls");
            }
            for (call = v->child; call; call = call->next) {
                unsigned index;
                json *target, *fn;
                const json *prop, *function = get(call, "function");
                ai_part_t *p;
                if (!object(call) || !index_value(get(call, "index"), &index)) return EPD_AI_PROTOCOL;
                if (index >= EPD_AI_MAX_TOOLS) return EPD_AI_LIMIT;
                while ((unsigned)ai_cjson_GetArraySize(calls) <= index) {
                    target = ai_cjson_CreateObject();
                    if (!target) return EPD_AI_NO_MEMORY;
                    e = resize_native(s, owner, 0, 2 + (calls->child ? 1 : 0));
                    if (e) { ai_cjson_Delete(target); return e; }
                    if (!ai_cjson_AddItemToArray(calls, target)) { ai_cjson_Delete(target); return EPD_AI_NO_MEMORY; }
                }
                target = ai_cjson_GetArrayItem(calls, (int)index);
                for (prop = call->child; prop; prop = prop->next) {
                    if (eq(prop->string, "index") || eq(prop->string, "function")) continue;
                    if (eq(prop->string, "id")) {
                        if (!str(prop)) return EPD_AI_PROTOCOL;
                        e = append_field(s, owner, target, "id", str(prop), EPD_AI_ARGUMENT_LIMIT);
                    } else if (eq(prop->string, "type")) {
                        if (!eq(str(prop), "function")) return EPD_AI_UNSUPPORTED;
                        e = put_native(s, owner, target, "type", ai_cjson_Duplicate(prop, 1));
                    } else return EPD_AI_UNSUPPORTED;
                    if (e) return e;
                }
                fn = get(target, "function");
                if (!fn) {
                    e = put_native(s, owner, target, "function", ai_cjson_CreateObject()); if (e) return e;
                    fn = get(target, "function");
                }
                if (function && !object(function)) return EPD_AI_PROTOCOL;
                for (prop = function ? function->child : NULL; prop; prop = prop->next) {
                    if ((!eq(prop->string, "name") && !eq(prop->string, "arguments")) || !str(prop)) return EPD_AI_UNSUPPORTED;
                    e = append_field(s, owner, fn, prop->string, str(prop), EPD_AI_ARGUMENT_LIMIT); if (e) return e;
                }
                p = part(s, 2, index, 0, true); if (!p) return s->error;
                if (field(fn, "arguments") && *field(fn, "arguments")) {
                    e = tool_identity(s, p, field(target, "id"), field(fn, "name")); if (e) return e;
                    /* The retained field already contains the prefix; publish only this delta. */
                    e = tool_arguments(s, p, field(function, "arguments") ? field(function, "arguments") : "", false, false); if (e) return e;
                }
            }
        } else if (eq(v->string, "annotations")) {
            json *annotations = get(message, "annotations");
            const json *annotation;
            if (!array(v)) return EPD_AI_PROTOCOL;
            if (!annotations) {
                e = put_native(s, owner, message, "annotations", ai_cjson_CreateArray()); if (e) return e;
                annotations = get(message, "annotations");
            }
            for (annotation = v->child; annotation; annotation = annotation->next) {
                e = add_native(s, owner, annotations, annotation); if (e) return e;
            }
        } else return EPD_AI_UNSUPPORTED;
    }
    return EPD_AI_OK;
}

static epd_ai_error_t chat(ai_protocol_t *s, const json *root, bool streaming)
{
    const json *choices = get(root, "choices"), *choice;
    json *message;
    ai_part_t *p;
    epd_ai_error_t e;
    unsigned index;
    e = usage(s, get(root, "usage")); if (e) return e;
    if ((!choices || ai_cjson_IsNull(choices)) && streaming && object(get(root, "usage"))) return EPD_AI_OK;
    if (!array(choices)) return EPD_AI_PROTOCOL;
    if (!choices->child) return streaming && get(root, "usage") ? EPD_AI_OK : EPD_AI_PROTOCOL;
    choice = choices->child;
    if (choice->next || !object(choice)) return EPD_AI_UNSUPPORTED;
    if (!index_value(get(choice, "index"), &index) || index != 0) return EPD_AI_UNSUPPORTED;
    p = part(s, 0, 0, 0, true); if (!p) return s->error;
    if (streaming && get(choice, "delta")) {
        if (s->stop_reason) return EPD_AI_PROTOCOL;
        if (!p->native) {
            message = ai_cjson_CreateObject();
            if (!message) return EPD_AI_NO_MEMORY;
            if (!ai_cjson_AddStringToObject(message, "role", "assistant") ||
                !ai_cjson_AddNullToObject(message, "content")) e = EPD_AI_NO_MEMORY;
            else e = save(s, p, message);
            ai_cjson_Delete(message); if (e) return e;
        }
        message = p->native;
        e = chat_delta(s, p, message, get(choice, "delta")); if (e) return e;
        e = check_native(message); if (e) return e;
    } else {
        json *full = get(choice, "message");
        if (!object(full)) return EPD_AI_PROTOCOL;
        if (p->native) {
            const char *a = field(p->native, "reasoning_content"), *b = field(full, "reasoning_content");
            if (a && (!b || strlen(b) < strlen(a) || memcmp(a, b, strlen(a)))) return EPD_AI_PROTOCOL;
        }
        e = chat_message(s, full, false); if (e) return e;
        e = save(s, p, full); if (e) return e;
        message = p->native;
    }
    if (get(choice, "finish_reason") && !ai_cjson_IsNull(get(choice, "finish_reason"))) {
        e = finish_reason(s, field(choice, "finish_reason")); if (e) return e;
        e = chat_message(s, message, s->result->finish != EPD_AI_FINISH_LENGTH); if (e) return e;
        if (s->refusal && s->result->finish == EPD_AI_FINISH_STOP) s->result->finish = EPD_AI_FINISH_REFUSAL;
    } else if (!streaming) return EPD_AI_PROTOCOL;
    return streaming ? EPD_AI_OK : terminal(s, message);
}

static epd_ai_error_t response_content(ai_protocol_t *s, unsigned index, unsigned sub,
                                      const json *content, bool complete)
{
    const char *type = field(content, "type"), *text;
    ai_part_t *p;
    unsigned kind;
    epd_ai_error_t e;
    if (eq(type, "output_text")) { text = field(content, "text"); kind = 1; }
    else if (eq(type, "refusal")) { text = field(content, "refusal"); kind = 2; s->refusal = true; }
    else return EPD_AI_UNSUPPORTED;
    p = part(s, 1, index, sub, true); if (!p) return s->error;
    if (p->kind && p->kind != kind) return EPD_AI_PROTOCOL;
    p->kind = kind;
    e = text_update(s, p, text, true);
    if (!e && complete) e = citations(s, p, get(content, "annotations"));
    if (!e && complete) p->value_done = true;
    return e;
}

static epd_ai_error_t response_item(ai_protocol_t *s, unsigned index, json *item, bool complete, bool retain)
{
    const char *type = field(item, "type"), *id = field(item, "id");
    ai_part_t *p = part(s, 0, index, 0, true);
    epd_ai_error_t e = EPD_AI_OK;
    if (!p) return s->error;
    if (!object(item) || !id || !*id) return EPD_AI_PROTOCOL;
    if (complete && field(item, "status") && !eq(field(item, "status"), "completed") &&
        !eq(field(item, "status"), "incomplete") &&
        !(eq(type, "web_search_call") && eq(field(item, "status"), "failed"))) return EPD_AI_PROTOCOL;
    if (!p->identity) {
        ai_part_t *other;
        for (other = s->parts; other; other = other->next)
            if (other != p && !other->group && eq(other->identity, id)) return EPD_AI_PROTOCOL;
        p->identity = ai_strndup(id, strlen(id));
        if (!p->identity) return EPD_AI_NO_MEMORY;
    } else if (!eq(p->identity, id)) return EPD_AI_PROTOCOL;
    if (p->native) {
        json *old = p->native;
        if (!eq(field(old, "id"), id) || !eq(field(old, "type"), type)) e = EPD_AI_PROTOCOL;
        if (p->done && !ai_cjson_Compare(old, item, 1)) e = EPD_AI_PROTOCOL;
        if (e) return e;
    }
    if (eq(type, "message")) {
        const json *contents = get(item, "content"), *v;
        unsigned sub = 0;
        ai_part_t *q;
        if (!eq(field(item, "role"), "assistant") || !array(contents)) return EPD_AI_PROTOCOL;
        for (v = contents->child; v; v = v->next, ++sub) {
            if (sub >= 4096) return EPD_AI_LIMIT;
            e = response_content(s, index, sub, v, complete); if (e) return e;
        }
        for (q = s->parts; q; q = q->next)
            if (q->group == 1 && q->index == index &&
                (q->sub >= sub || (complete && q->opened && !q->done))) return EPD_AI_PROTOCOL;
    } else if (eq(type, "function_call")) {
        const char *args = field(item, "arguments");
        e = tool_identity(s, p, field(item, "call_id"), field(item, "name")); if (e) return e;
        if (!args) return EPD_AI_PROTOCOL;
        e = tool_arguments(s, p, args, true, complete && !s->turn_incomplete && s->result->finish != EPD_AI_FINISH_LENGTH);
        if (e) return e;
    } else if (eq(type, "reasoning")) {
        ai_part_t *q;
        if (complete) {
            if (s->features & EPD_AI_FEATURE_DEEPSEEK) {
                if (!array(get(item, "content"))) s->native_error = EPD_AI_UNSUPPORTED;
            } else if (!field(item, "encrypted_content") || !*field(item, "encrypted_content"))
                s->native_error = EPD_AI_UNSUPPORTED;
        }
        if (complete) for (q = s->parts; q; q = q->next) {
            const char *final_text;
            json *content;
            if ((q->group != 3 && q->group != 4) || q->index != index) continue;
            if (q->opened && !q->done) return EPD_AI_PROTOCOL;
            content = ai_cjson_GetArrayItem(get(item, q->group == 3 ? "summary" : "content"), (int)q->sub);
            final_text = field(content, "text");
            if (!final_text || strlen(final_text) < q->text_size ||
                (q->text_size && memcmp(final_text, q->text, q->text_size))) return EPD_AI_PROTOCOL;
        }
    } else if (eq(type, "web_search_call")) {
        if (!p->opened && !complete) e = notice(s, EPD_AI_EVENT_SEARCH_START, NULL, 0, 0);
        if (complete && !p->done) e = eq(field(item, "status"), "failed") ?
            search_error(s, field(get(item, "error"), "code")) :
            notice(s, EPD_AI_EVENT_SEARCH_DONE, NULL, 0, 0);
        if (e) return e;
    } else return EPD_AI_UNSUPPORTED;
    if (retain) e = save(s, p, item);
    if (!e && complete) p->done = true;
    return e;
}

static epd_ai_error_t response_full(ai_protocol_t *s, const json *root, const char *event)
{
    json *output = get(root, "output"), *v;
    const char *status = field(root, "status");
    unsigned index = 0;
    ai_part_t *p;
    epd_ai_error_t e;
    if (!object(root)) return EPD_AI_PROTOCOL;
    if (event && ((eq(event, "response.completed") && !eq(status, "completed")) ||
                  (eq(event, "response.failed") && !eq(status, "failed")) ||
                  (eq(event, "response.incomplete") && !eq(status, "incomplete")))) return EPD_AI_PROTOCOL;
    e = usage(s, get(root, "usage")); if (e) return e;
    if (eq(event, "response.failed") || eq(status, "failed")) {
        s->terminal = true; s->sse.stopped = true;
        return provider_error(s, get(root, "error"));
    }
    if (get(root, "error") && !ai_cjson_IsNull(get(root, "error"))) return provider_error(s, get(root, "error"));
    if (eq(event, "response.incomplete") || eq(status, "incomplete")) {
        if (!eq(status, "incomplete")) return EPD_AI_PROTOCOL;
        s->turn_incomplete = true;
        e = finish_reason(s, field(get(root, "incomplete_details"), "reason"));
        if (e) return e;
    } else {
        if (!eq(status, "completed") || (event && !eq(event, "response.completed"))) return EPD_AI_PROTOCOL;
        s->stop_reason = true; s->result->finish = EPD_AI_FINISH_STOP;
    }
    if (!array(output)) return EPD_AI_PROTOCOL;
    for (v = output->child; v; v = v->next, ++index) {
        if (index >= 4096) return EPD_AI_LIMIT;
        p = part(s, 0, index, 0, false);
        if (p && p->opened && !p->done) return EPD_AI_PROTOCOL;
        if (!s->turn_incomplete && field(v, "status") &&
            !eq(field(v, "status"), "completed") &&
            !(eq(field(v, "type"), "web_search_call") && eq(field(v, "status"), "failed"))) return EPD_AI_PROTOCOL;
        e = response_item(s, index, v, true, false); if (e) return e;
    }
    for (p = s->parts; p; p = p->next)
        if (p->group == 0 && p->index >= index) return EPD_AI_PROTOCOL;
    if (s->result->finish == EPD_AI_FINISH_STOP)
        s->result->finish = s->result->tool_count ? EPD_AI_FINISH_TOOLS :
                            s->refusal ? EPD_AI_FINISH_REFUSAL : EPD_AI_FINISH_STOP;
    return terminal(s, output);
}

static epd_ai_error_t response_event(ai_protocol_t *s, const json *root, const char *type)
{
    unsigned index, sub;
    ai_part_t *p;
    epd_ai_error_t e = EPD_AI_OK;
    bool reasoning_part = (eq(type, "response.content_part.added") || eq(type, "response.content_part.done")) &&
                          eq(field(get(root, "part"), "type"), "reasoning_text");
    if (eq(type, "response.completed") || eq(type, "response.failed") || eq(type, "response.incomplete"))
        return response_full(s, get(root, "response"), type);
    if (eq(type, "response.created") || eq(type, "response.in_progress") || eq(type, "response.queued")) {
        const json *response = get(root, "response");
        if (!object(response)) return EPD_AI_PROTOCOL;
        return usage(s, get(response, "usage"));
    }
    if (!index_value(get(root, "output_index"), &index)) return EPD_AI_UNSUPPORTED;
    if (eq(type, "response.output_item.added")) {
        p = part(s, 0, index, 0, true); if (!p) return s->error;
        if (p->opened || p->done) return EPD_AI_PROTOCOL;
        e = response_item(s, index, get(root, "item"), false, true);
        if (!e) p->opened = true;
        return e;
    }
    p = part(s, 0, index, 0, false);
    if (!p || !p->native) return EPD_AI_PROTOCOL;
    if (field(root, "item_id")) {
        json *item = p->native;
        if (!eq(field(item, "id"), field(root, "item_id"))) e = EPD_AI_PROTOCOL;
        if (e) return e;
    }
    if (eq(type, "response.output_item.done")) {
        if (p->done) return EPD_AI_PROTOCOL;
        return response_item(s, index, get(root, "item"), true, true);
    }
    if (p->done) return EPD_AI_PROTOCOL;
    if (eq(type, "response.web_search_call.in_progress") || eq(type, "response.web_search_call.searching") ||
        eq(type, "response.web_search_call.completed")) {
        json *item = p->native;
        bool valid = eq(field(item, "type"), "web_search_call");
        if (!valid) return EPD_AI_PROTOCOL;
        return notice(s, eq(type, "response.web_search_call.completed") ?
            EPD_AI_EVENT_SEARCH_DONE : EPD_AI_EVENT_SEARCH_START, NULL, 0, 0);
    }
    if (eq(type, "response.function_call_arguments.delta") || eq(type, "response.function_call_arguments.done")) {
        bool full = eq(type, "response.function_call_arguments.done");
        return tool_arguments(s, p, field(root, full ? "arguments" : "delta"), full, full);
    }
    /* Display-free reasoning trackers verify identities and required closes;
     * replay data always comes from the completed native item. */
    if (eq(type, "response.reasoning_summary_part.added") || eq(type, "response.reasoning_summary_part.done") ||
        eq(type, "response.reasoning_summary_text.delta") || eq(type, "response.reasoning_summary_text.done") ||
        eq(type, "response.reasoning_text.delta") || eq(type, "response.reasoning_text.done") || reasoning_part) {
        json *item = p->native;
        bool summary = strncmp(type, "response.reasoning_summary_", 27) == 0;
        bool added = eq(type, "response.reasoning_summary_part.added") ||
                     (reasoning_part && eq(type, "response.content_part.added"));
        bool part_done = eq(type, "response.reasoning_summary_part.done") ||
                         (reasoning_part && eq(type, "response.content_part.done"));
        bool full = part_done || eq(type, "response.reasoning_summary_text.done") || eq(type, "response.reasoning_text.done");
        const char *text;
        size_t n, skip = 0;
        if (!eq(field(item, "type"), "reasoning")) e = EPD_AI_PROTOCOL;
        if (e) return e;
        if (!index_value(get(root, summary ? "summary_index" : "content_index"), &sub)) return EPD_AI_PROTOCOL;
        p = part(s, summary ? 3 : 4, index, sub, true); if (!p) return s->error;
        if (p->done || (added && p->opened)) return EPD_AI_PROTOCOL;
        if (added || part_done) {
            if (!eq(field(get(root, "part"), "type"), summary ? "summary_text" : "reasoning_text"))
                return EPD_AI_UNSUPPORTED;
            text = field(get(root, "part"), "text");
            if (part_done && !p->opened) return EPD_AI_PROTOCOL;
        } else text = field(root, full ? "text" : "delta");
        if (!text) return EPD_AI_PROTOCOL;
        n = strlen(text);
        if (full || added) {
            if (n < p->text_size || (p->text_size && memcmp(text, p->text, p->text_size))) return EPD_AI_PROTOCOL;
            skip = p->text_size;
        } else if (p->value_done) return EPD_AI_PROTOCOL;
        e = append(&p->text, &p->text_size, &p->text_capacity, text + skip, n - skip, EPD_AI_NATIVE_LIMIT);
        if (!e) {
            if (added) p->opened = true;
            if (part_done) p->done = true;
            if (full) p->value_done = true;
        }
        return e;
    }
    if (!index_value(get(root, "content_index"), &sub)) return EPD_AI_UNSUPPORTED;
    {
        json *item = p->native;
        if (!eq(field(item, "type"), "message")) e = EPD_AI_PROTOCOL;
        if (e) return e;
    }
    p = part(s, 1, index, sub, true); if (!p) return s->error;
    if (eq(type, "response.content_part.added")) {
        if (p->opened || p->done) return EPD_AI_PROTOCOL;
        e = response_content(s, index, sub, get(root, "part"), false);
        if (!e) p->opened = true;
        return e;
    }
    if (eq(type, "response.content_part.done")) {
        if (!p->opened || p->done) return EPD_AI_PROTOCOL;
        e = response_content(s, index, sub, get(root, "part"), true);
        if (!e) p->done = true;
        return e;
    }
    if (eq(type, "response.output_text.annotation.added"))
        return !p->done && object(get(root, "annotation")) ? EPD_AI_OK : EPD_AI_PROTOCOL;
    if (eq(type, "response.output_text.delta") || eq(type, "response.output_text.done") ||
        eq(type, "response.refusal.delta") || eq(type, "response.refusal.done")) {
        bool refusal = eq(type, "response.refusal.delta") || eq(type, "response.refusal.done");
        bool full = eq(type, "response.output_text.done") || eq(type, "response.refusal.done");
        if (p->kind && p->kind != (refusal ? 2u : 1u)) return EPD_AI_PROTOCOL;
        if (p->done || p->value_done) return EPD_AI_PROTOCOL;
        p->kind = refusal ? 2 : 1; if (refusal) s->refusal = true;
        e = text_update(s, p, field(root, full ? (refusal ? "refusal" : "text") : "delta"), full);
        if (!e && full) p->value_done = true;
        return e;
    }
    return EPD_AI_UNSUPPORTED;
}

static epd_ai_error_t message_block(ai_protocol_t *s, ai_part_t *p, const json *block, bool complete)
{
    const char *type = field(block, "type");
    epd_ai_error_t e = EPD_AI_OK;
    if (!object(block)) return EPD_AI_PROTOCOL;
    if (eq(type, "text")) {
        e = text_update(s, p, field(block, "text"), true);
        if (!e && complete) e = citations(s, p, get(block, "citations"));
    } else if (eq(type, "server_tool_use")) {
        if (!eq(field(block, "name"), "web_search") || !field(block, "id") ||
            !object(get(block, "input"))) return EPD_AI_UNSUPPORTED;
        if (!complete) e = notice(s, EPD_AI_EVENT_SEARCH_START, NULL, 0, 0);
    } else if (eq(type, "web_search_tool_result")) {
        const json *content = get(block, "content");
        if (!field(block, "tool_use_id")) return EPD_AI_PROTOCOL;
        if (!array(content) && !eq(field(content, "type"), "web_search_tool_result_error"))
            return EPD_AI_PROTOCOL;
        if (complete) e = array(content) ? notice(s, EPD_AI_EVENT_SEARCH_DONE, NULL, 0, 0) :
                                          search_error(s, field(content, "error_code"));
    } else if (eq(type, "thinking")) {
        if (!field(block, "thinking")) return EPD_AI_PROTOCOL;
        if (complete && (!field(block, "signature") || !*field(block, "signature"))) {
            if (!s->stream && s->result->finish != EPD_AI_FINISH_LENGTH) return EPD_AI_UNSUPPORTED;
            s->native_error = EPD_AI_UNSUPPORTED;
        }
    } else if (eq(type, "redacted_thinking")) {
        if (!field(block, "data") || !*field(block, "data")) return EPD_AI_PROTOCOL;
    } else if (eq(type, "tool_use")) {
        const json *input = get(block, "input");
        char *args;
        if (!object(input)) return EPD_AI_PROTOCOL;
        e = tool_identity(s, p, field(block, "id"), field(block, "name")); if (e) return e;
        if (!complete) return EPD_AI_OK;
        args = ai_json_print(input, &e); if (!args) return e;
        /* The parsed input is authoritative; streamed whitespace is harmless. */
        if (s->result->tools[p->tool].arguments_size) {
            json *streamed = ai_json_parse(s->result->tools[p->tool].arguments,
                                           s->result->tools[p->tool].arguments_size, &e);
            if (!streamed) { ai_free(args); return e; }
            if (!ai_cjson_Compare(streamed, input, 1)) e = EPD_AI_PROTOCOL;
            ai_cjson_Delete(streamed);
            if (!e) e = tool_arguments(s, p, "", false, true);
        } else e = tool_arguments(s, p, args, true, true);
        ai_free(args);
    } else return EPD_AI_UNSUPPORTED;
    return e;
}

static json *message_array(ai_protocol_t *s, epd_ai_error_t *e)
{
    json *content = ai_cjson_CreateArray();
    ai_part_t *p;
    unsigned i, count = 0;
    *e = EPD_AI_OK;
    if (!content) { *e = EPD_AI_NO_MEMORY; return NULL; }
    for (p = s->parts; p; p = p->next) if (!p->group) ++count;
    for (i = 0; i < count; ++i) {
        json *block;
        p = part(s, 0, i, 0, false);
        if (!p || !p->done || !p->native) { *e = EPD_AI_PROTOCOL; goto bad; }
        block = ai_cjson_CreateObjectReference(p->native->child);
        if (!block) { *e = EPD_AI_NO_MEMORY; goto bad; }
        if (!ai_cjson_AddItemToArray(content, block)) {
            ai_cjson_Delete(block); *e = EPD_AI_NO_MEMORY; goto bad;
        }
    }
    return content;
bad:
    ai_cjson_Delete(content); return NULL;
}

static epd_ai_error_t messages_full(ai_protocol_t *s, const json *root)
{
    const json *content = get(root, "content"), *v;
    unsigned index = 0;
    epd_ai_error_t e;
    if (!eq(field(root, "role"), "assistant") || !eq(field(root, "type"), "message") || !array(content)) return EPD_AI_PROTOCOL;
    e = finish_reason(s, field(root, "stop_reason")); if (e) return e;
    e = usage(s, get(root, "usage")); if (e) return e;
    for (v = content->child; v; v = v->next, ++index) {
        ai_part_t *p;
        if (index >= 4096) return EPD_AI_LIMIT;
        p = part(s, 0, index, 0, true); if (!p) return s->error;
        e = message_block(s, p, v, true); if (e) return e;
        p->done = true;
    }
    return terminal(s, content);
}

static epd_ai_error_t message_delta(ai_protocol_t *s, ai_part_t *p, const json *delta)
{
    epd_ai_error_t e = EPD_AI_OK;
    json *block = p->native;
    const char *type = field(delta, "type"), *block_type;
    if (!block) return EPD_AI_PROTOCOL;
    block_type = field(block, "type");
    if (eq(type, "text_delta") && eq(block_type, "text")) {
        const char *text = field(delta, "text");
        if (!text) { e = EPD_AI_PROTOCOL; goto done; }
        e = append_field(s, p, block, "text", text, EPD_AI_TEXT_LIMIT);
        if (!e) e = text_update(s, p, text, false);
    } else if (eq(type, "thinking_delta") && eq(block_type, "thinking")) {
        const char *thinking = field(delta, "thinking");
        e = thinking ? append_field(s, p, block, "thinking", thinking, EPD_AI_NATIVE_LIMIT) : EPD_AI_PROTOCOL;
    } else if (eq(type, "signature_delta") && eq(block_type, "thinking")) {
        const char *signature = field(delta, "signature");
        e = signature ? append_field(s, p, block, "signature", signature, EPD_AI_NATIVE_LIMIT) : EPD_AI_PROTOCOL;
    } else if (eq(type, "input_json_delta") && eq(block_type, "tool_use")) {
        e = tool_arguments(s, p, field(delta, "partial_json"), false, false);
        /* The argument string stays separate until a closed JSON object exists. */
        goto done;
    } else if (eq(type, "input_json_delta") && eq(block_type, "server_tool_use")) {
        const char *value = field(delta, "partial_json");
        e = value ? append(&p->text, &p->text_size, &p->text_capacity, value, strlen(value), EPD_AI_ARGUMENT_LIMIT) : EPD_AI_PROTOCOL;
        goto done;
    } else if (eq(type, "citations_delta") && eq(block_type, "text")) {
        const json *citation = get(delta, "citation");
        json *citations = get(block, "citations");
        if (!object(citation)) { e = EPD_AI_PROTOCOL; goto done; }
        if (!citations) {
            e = put_native(s, p, block, "citations", ai_cjson_CreateArray()); if (e) goto done;
            citations = get(block, "citations");
        }
        if (!array(citations)) { e = EPD_AI_PROTOCOL; goto done; }
        e = add_native(s, p, citations, citation);
    } else e = EPD_AI_UNSUPPORTED;
done:
    return e ? e : check_native(block);
}

static epd_ai_error_t messages_event(ai_protocol_t *s, const json *root, const char *type)
{
    unsigned index;
    ai_part_t *p;
    epd_ai_error_t e = EPD_AI_OK;
    if (eq(type, "message_start")) {
        const json *message = get(root, "message"), *content = get(message, "content");
        if (s->message_started || !object(message) || !eq(field(message, "role"), "assistant") ||
            !eq(field(message, "type"), "message") || !array(content) || content->child) return EPD_AI_PROTOCOL;
        s->message_started = true;
        return usage(s, get(message, "usage"));
    }
    if (!s->message_started) return EPD_AI_PROTOCOL;
    if (eq(type, "message_stop")) {
        json *content;
        if (!s->stop_reason) return EPD_AI_PROTOCOL;
        content = message_array(s, &e); if (!content) return e;
        e = terminal(s, content); ai_cjson_Delete(content); return e;
    }
    if (eq(type, "message_delta")) {
        const json *delta = get(root, "delta");
        const json *v;
        if (!object(delta)) return EPD_AI_PROTOCOL;
        for (v = delta->child; v; v = v->next)
            if (!eq(v->string, "stop_reason") && !eq(v->string, "stop_sequence")) return EPD_AI_UNSUPPORTED;
        for (p = s->parts; p; p = p->next)
            if (!p->group && p->opened && !p->done) return EPD_AI_PROTOCOL;
        if (get(delta, "stop_reason") && !ai_cjson_IsNull(get(delta, "stop_reason"))) {
            e = finish_reason(s, field(delta, "stop_reason")); if (e) return e;
        }
        return usage(s, get(root, "usage"));
    }
    if (s->stop_reason) return EPD_AI_PROTOCOL;
    if (!index_value(get(root, "index"), &index)) return EPD_AI_UNSUPPORTED;
    if (eq(type, "content_block_start")) {
        json *block = get(root, "content_block");
        p = part(s, 0, index, 0, true); if (!p) return s->error;
        if (p->opened || p->done) return EPD_AI_PROTOCOL;
        e = message_block(s, p, block, false); if (e) return e;
        e = save(s, p, block); if (!e) p->opened = true;
        return e;
    }
    p = part(s, 0, index, 0, false);
    if (!p || !p->opened || p->done) return EPD_AI_PROTOCOL;
    if (eq(type, "content_block_delta")) return message_delta(s, p, get(root, "delta"));
    if (eq(type, "content_block_stop")) {
        json *block = p->native;
        if (!block) return EPD_AI_PROTOCOL;
        if (eq(field(block, "type"), "server_tool_use") && p->text_size) {
            json *input = ai_json_parse(p->text, p->text_size, &e);
            if (!input) goto stop_done;
            if (!object(input)) { ai_cjson_Delete(input); e = EPD_AI_PROTOCOL; goto stop_done; }
            e = put_native(s, p, block, "input", input); if (e) goto stop_done;
        }
        if (p->tool >= 0 && s->result->tools[p->tool].arguments_size) {
            epd_ai_tool_call_t *t = &s->result->tools[p->tool];
            json *input = ai_json_parse(t->arguments, t->arguments_size, &e);
            if (!input) {
                if (e == EPD_AI_INVALID || e == EPD_AI_PROTOCOL) {
                    s->native_error = EPD_AI_PROTOCOL; p->done = true; e = EPD_AI_OK;
                }
                goto stop_done;
            }
            if (!object(input)) { ai_cjson_Delete(input); e = EPD_AI_PROTOCOL; goto stop_done; }
            e = put_native(s, p, block, "input", input); if (e) goto stop_done;
        }
        e = message_block(s, p, block, true); if (e) goto stop_done;
        e = check_native(block); if (e) goto stop_done;
        p->done = true;
stop_done:
        return e;
    }
    return EPD_AI_UNSUPPORTED;
}

static bool progress_event(const json *root, const char *type)
{
    const json *v;
    size_t n = type ? strlen(type) : 0;
    if (!eq(type, "ping") && !eq(type, "heartbeat") && !eq(type, "progress") &&
        !(n > 9 && !strcmp(type + n - 9, ".progress"))) return false;
    for (v = root->child; v; v = v->next)
        if (!eq(v->string, "type") && !eq(v->string, "sequence_number") &&
            !eq(v->string, "progress") && !eq(v->string, "percent")) return false;
    return true;
}

static epd_ai_error_t parse_event(void *context, const char *event, const char *data, size_t size)
{
    ai_protocol_t *s = context;
    json *root = NULL;
    epd_ai_error_t e = EPD_AI_OK;
    const char *type;
    if (s->terminal) return EPD_AI_OK;
    if (!ai_json_begin()) return EPD_AI_NO_MEMORY;
    if (s->protocol == EPD_AI_CHAT && size == 6 && !memcmp(data, "[DONE]", 6)) {
        ai_part_t *p = part(s, 0, 0, 0, false);
        if (!s->stop_reason || !p) { e = EPD_AI_PROTOCOL; goto done; }
        if (!p->native) { e = EPD_AI_PROTOCOL; goto done; }
        e = terminal(s, p->native); goto done;
    }
    root = ai_json_parse(data, size, &e);
    /* Reuse ordinary event storage, but release unusually large snapshots. */
    if (s->sse.capacity > 8192u) {
        ai_free(s->sse.data); s->sse.data = NULL; s->sse.size = s->sse.capacity = 0;
    }
    if (!root) { if (!e) e = EPD_AI_PROTOCOL; goto done; }
    if (!object(root) || !valid_strings(root)) { e = EPD_AI_PROTOCOL; goto done; }
    if (field(root, "request_id"))
        fixed_string(s->result->request_id, sizeof(s->result->request_id), field(root, "request_id"));
    if (get(root, "error") && !ai_cjson_IsNull(get(root, "error"))) {
        s->terminal = true; s->sse.stopped = true;
        e = provider_error(s, get(root, "error")); goto done;
    }
    type = field(root, "type");
    if (event && *event && !eq(event, "message") && !eq(event, type)) { e = EPD_AI_PROTOCOL; goto done; }
    if (s->protocol != EPD_AI_CHAT && progress_event(root, type)) goto done;
    if (s->protocol == EPD_AI_CHAT) e = chat(s, root, true);
    else if (!type) e = EPD_AI_PROTOCOL;
    else if (eq(type, "error")) { s->terminal = true; s->sse.stopped = true; e = provider_error(s, root); }
    else if (s->protocol == EPD_AI_RESPONSES) e = response_event(s, root, type);
    else e = messages_event(s, root, type);
    if (!e && !eq(type, "ping")) s->seen = true;
done:
    ai_cjson_Delete(root); ai_json_end();
    if (!e) e = publish(s); else notices_clear(s);
    return e;
}

epd_ai_error_t ai_protocol_open(const ai_profile_t *profile, const epd_ai_request_t *request,
                               epd_ai_event_cb_t callback, void *user,
                               epd_ai_result_t *result, ai_protocol_t **out)
{
    ai_protocol_t *s;
    if (!out) return EPD_AI_INVALID;
    *out = NULL;
    if (!profile || !request || !result || !request->model ||
        strlen(request->model) >= EPD_AI_MODEL_SIZE ||
        !memchr(profile->public.id, 0, sizeof(profile->public.id)) ||
        profile->public.protocol > EPD_AI_MESSAGES || profile->public.protocol < EPD_AI_RESPONSES) return EPD_AI_INVALID;
    if (result->text || result->native || result->tool_count) return EPD_AI_INVALID;
    s = ai_calloc(1, sizeof(*s)); if (!s) return EPD_AI_NO_MEMORY;
    s->protocol = profile->public.protocol; s->revision = profile->public.identity_revision;
    s->features = profile->public.features;
    strcpy(s->profile, profile->public.id); strcpy(s->model, request->model);
    s->stream = request->stream; s->callback = callback; s->user = user; s->result = result;
    result->struct_size = sizeof(*result); result->version = EPD_AI_VERSION;
    result->complete = result->replayable = false;
    result->error = EPD_AI_OK; result->finish = EPD_AI_FINISH_UNKNOWN;
    memset(&result->usage, 0, sizeof(result->usage));
    ai_sse_init(&s->sse, parse_event, s); *out = s; return EPD_AI_OK;
}

epd_ai_error_t ai_protocol_feed(ai_protocol_t *s, const void *data, size_t size)
{
    epd_ai_error_t e;
    if (!s || (!data && size)) return EPD_AI_INVALID;
    if (s->error) return s->error;
    if (s->terminal) return EPD_AI_OK;
    if (s->finished) return EPD_AI_INVALID;
    if (s->stream) return fail(s, ai_sse_feed(&s->sse, data, size));
    if (size) s->seen = true;
    if (size > EPD_AI_EVENT_LIMIT || s->body_size > EPD_AI_EVENT_LIMIT - size) return fail(s, EPD_AI_LIMIT);
    if (s->body_size + size + 1 > s->body_capacity) {
        size_t n = s->body_capacity ? s->body_capacity * 2 : 512;
        char *p;
        if (n < s->body_size + size + 1) n = s->body_size + size + 1;
        if (n > EPD_AI_EVENT_LIMIT + 1u) n = EPD_AI_EVENT_LIMIT + 1u;
        p = ai_realloc(s->body, n); if (!p) return fail(s, EPD_AI_NO_MEMORY);
        s->body = p; s->body_capacity = n;
    }
    if (size) memcpy(s->body + s->body_size, data, size);
    s->body_size += size; s->body[s->body_size] = 0;
    e = EPD_AI_OK; return e;
}

bool ai_protocol_has_event(const ai_protocol_t *s) { return s && s->seen; }
bool ai_protocol_complete(const ai_protocol_t *s) { return s && s->terminal; }

epd_ai_error_t ai_protocol_finish(ai_protocol_t *s)
{
    epd_ai_error_t e = EPD_AI_OK;
    json *root;
    if (!s) return EPD_AI_INVALID;
    if (s->error) return s->error;
    if (s->finished) return s->error;
    s->finished = true;
    if (s->stream) return fail(s, s->terminal ? EPD_AI_OK : EPD_AI_INCOMPLETE);
    if (!s->body_size) return fail(s, EPD_AI_INCOMPLETE);
    if (!ai_utf8_valid(s->body, s->body_size)) return fail(s, EPD_AI_PROTOCOL);
    if (!ai_json_begin()) return fail(s, EPD_AI_NO_MEMORY);
    root = ai_json_parse(s->body, s->body_size, &e);
    ai_free(s->body); s->body = NULL; s->body_size = s->body_capacity = 0;
    if (!root) { if (!e) e = EPD_AI_PROTOCOL; }
    else if (!object(root) || !valid_strings(root)) e = EPD_AI_PROTOCOL;
    else if (get(root, "error") && !ai_cjson_IsNull(get(root, "error"))) e = provider_error(s, get(root, "error"));
    else if (s->protocol == EPD_AI_CHAT) e = chat(s, root, false);
    else if (s->protocol == EPD_AI_RESPONSES) e = response_full(s, root, NULL);
    else e = messages_full(s, root);
    ai_cjson_Delete(root); ai_json_end();
    if (!e) e = publish(s); else notices_clear(s);
    return fail(s, e);
}

void ai_protocol_close(ai_protocol_t *s)
{
    if (!s) return;
    bool has_native = false;
    for (ai_part_t *p = s->parts; p; p = p->next) if (p->native) { has_native = true; break; }
    bool acquired = has_native && !ai_json_locked();
    if (acquired && !ai_json_begin()) return;
    notices_clear(s); ai_sse_clear(&s->sse); ai_free(s->body);
    while (s->parts) {
        ai_part_t *p = s->parts; s->parts = p->next;
        fields_clear(p);
        if (p->native) ai_cjson_Delete(p->native);
        ai_free(p->text); ai_free(p->identity); ai_free(p);
    }
    ai_free(s);
    if (acquired) ai_json_end();
}

epd_ai_error_t ai_protocol_merge(epd_ai_result_t *previous, epd_ai_result_t *current)
{ return ai_protocol_merge_reserved(previous, current, 0); }

epd_ai_error_t ai_protocol_merge_reserved(epd_ai_result_t *previous,
                                         epd_ai_result_t *current, size_t capacity)
{
    if (!previous->native && !previous->text) return EPD_AI_OK;
    if (previous->text_size > EPD_AI_TEXT_LIMIT) return EPD_AI_LIMIT;
    size_t keep = current->text_size, room = EPD_AI_TEXT_LIMIT - previous->text_size;
    epd_ai_error_t e = keep > room ? EPD_AI_LIMIT : EPD_AI_OK;
    if (keep > room) {
        keep = room;
        while (keep && ((unsigned char)current->text[keep] & 0xc0) == 0x80) --keep;
    }
    size_t size = previous->text_size + keep;
    char *text = previous->text, *native = NULL;
    if (!text || capacity < size + 1) {
        text = ai_alloc(size + 1);
        if (!text) {
            text = previous->text; keep = 0; size = previous->text_size;
            e = EPD_AI_NO_MEMORY;
        } else if (previous->text_size) memcpy(text, previous->text, previous->text_size);
    }
    if (keep) memcpy(text + previous->text_size, current->text, keep);
    if (text) text[size] = 0;
    if (text != previous->text) ai_free(previous->text);
    previous->text = NULL;
    ai_free(current->text); current->text = text; current->text_size = size;

    /* Text ownership is settled before any fallible history reconstruction. */
    bool locked = false;
    json *a = NULL, *b = NULL;
    if (!e && !current->error) {
        locked = ai_json_begin();
        if (!locked) e = EPD_AI_BUSY;
        else {
            if (previous->native) a = ai_json_parse(previous->native, previous->native_size, &e);
            if (!e && current->native) b = ai_json_parse(current->native, current->native_size, &e);
        }
    }
    if (!e && a && b) {
        static const char *keys[] = {"version", "protocol", "profile", "revision", "model"};
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
            if (!ai_cjson_Compare(get(a, keys[i]), get(b, keys[i]), 1)) e = EPD_AI_PROTOCOL;
        json *out = get(a, "data"), *in = get(b, "data");
        if (!array(out) || !array(in)) e = EPD_AI_PROTOCOL;
        if (!e) while (in->child) {
            json *item = ai_cjson_DetachItemFromArray(in, 0);
            if (!ai_cjson_AddItemToArray(out, item)) { ai_cjson_Delete(item); e = EPD_AI_NO_MEMORY; break; }
        }
    }
    if (!e && (a || b)) native = ai_json_print(a ? a : b, &e);
    if (!e && native && strlen(native) > EPD_AI_NATIVE_LIMIT) e = EPD_AI_LIMIT;
    if (!e && native) e = (epd_ai_error_t)ai_json_preflight(native, strlen(native));
    if (locked) { ai_cjson_Delete(a); ai_cjson_Delete(b); ai_json_end(); }
    if (e || current->error) {
        ai_free(native); native = NULL;
        current->complete = current->replayable = false;
        if (!current->error) current->error = e;
    }
    ai_free(current->native);
    current->native = native; current->native_size = native ? strlen(native) : 0;
    current->usage.valid &= previous->usage.valid;
    current->usage.input += previous->usage.input; current->usage.output += previous->usage.output;
    current->usage.cached += previous->usage.cached; current->usage.reasoning += previous->usage.reasoning;
    ai_free(previous->native);
    memset(previous, 0, sizeof(*previous));
    return e;
}
