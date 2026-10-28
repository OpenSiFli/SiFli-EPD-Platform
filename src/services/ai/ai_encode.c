/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *body; size_t used, capacity; epd_ai_error_t error; } writer_t;
typedef struct { char *id[EPD_AI_MAX_TOOLS]; bool done[EPD_AI_MAX_TOOLS]; size_t count; } calls_t;

static size_t bounded_length(const char *s, size_t limit)
{
    size_t n = 0;
    if (s) while (n <= limit && s[n]) ++n;
    return n;
}

static void bytes(writer_t *w, const char *s, size_t n)
{
    if (w->error != EPD_AI_OK) return;
    if (n > SIZE_MAX - w->used) { w->error = EPD_AI_LIMIT; return; }
    if (w->body)
    {
        if (w->used > w->capacity || n > w->capacity - w->used)
        { w->error = EPD_AI_LIMIT; return; }
        memcpy(w->body + w->used, s, n);
    }
    w->used += n;
}

static void raw(writer_t *w, const char *s) { bytes(w, s, strlen(s)); }

static void quoted(writer_t *w, const char *s, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    if (!ai_json_utf8(s, n)) { w->error = EPD_AI_INVALID; return; }
    raw(w, "\"");
    size_t start = 0;
    for (size_t i = 0; i < n; ++i)
    {
        unsigned c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 32)
        {
            bytes(w, s + start, i - start);
            if (c == '"') raw(w, "\\\"");
            else if (c == '\\') raw(w, "\\\\");
            else
            {
                char escape[] = "\\u0000";
                escape[4] = digits[c >> 4]; escape[5] = digits[c & 15];
                bytes(w, escape, 6);
            }
            start = i + 1;
        }
    }
    if (n > start) bytes(w, s + start, n - start);
    raw(w, "\"");
}

static void string(writer_t *w, const char *s)
{
    size_t n = bounded_length(s, EPD_AI_REQUEST_LIMIT);
    if (n > EPD_AI_REQUEST_LIMIT) { w->error = EPD_AI_LIMIT; return; }
    quoted(w, s, n);
}

static void number(writer_t *w, double n)
{
    char s[32];
    if (!isfinite(n)) { w->error = EPD_AI_INVALID; return; }
    int count = snprintf(s, sizeof(s), "%.17g", n);
    if (count <= 0 || (size_t)count >= sizeof(s)) { w->error = EPD_AI_INVALID; return; }
    /* JSON numbers always use a period, independently of the host locale. */
    for (int i = 0; i < count; ++i) if (s[i] == ',') s[i] = '.';
    bytes(w, s, (size_t)count);
}

/* Only bounded schema/native/extension subtrees are traversed here. */
static void json(writer_t *w, const ai_cjson *v, unsigned depth)
{
    if (!v || depth > 32) { w->error = EPD_AI_INVALID; return; }
    if (ai_cjson_IsString(v)) string(w, v->valuestring);
    else if (ai_cjson_IsNumber(v)) number(w, v->valuedouble);
    else if (ai_cjson_IsBool(v)) raw(w, ai_cjson_IsTrue(v) ? "true" : "false");
    else if (ai_cjson_IsNull(v)) raw(w, "null");
    else if (ai_cjson_IsObject(v) || ai_cjson_IsArray(v))
    {
        bool object = ai_cjson_IsObject(v);
        raw(w, object ? "{" : "[");
        for (const ai_cjson *p = v->child; p; p = p->next)
        {
            if (p != v->child) raw(w, ",");
            if (object) { string(w, p->string); raw(w, ":"); }
            json(w, p, depth + 1);
        }
        raw(w, object ? "}" : "]");
    }
    else w->error = EPD_AI_INVALID;
}

static ai_cjson *field(const ai_cjson *v, const char *name)
{ return ai_cjson_GetObjectItemCaseSensitive(v, name); }

static bool equal(const ai_cjson *v, const char *s)
{ return ai_cjson_IsString(v) && !strcmp(v->valuestring, s); }

static bool nonempty(const ai_cjson *v)
{ return ai_cjson_IsString(v) && v->valuestring[0]; }

static bool integer(const ai_cjson *v, double n)
{ return ai_cjson_IsNumber(v) && v->valuedouble == n; }

static bool identifier(const char *s, size_t max)
{
    size_t n = bounded_length(s, max);
    if (!n || n > max) return false;
    for (size_t i = 0; i < n; ++i)
    {
        unsigned c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static ai_cjson *parse(writer_t *w, const char *s, size_t n, size_t limit)
{
    if (w->error != EPD_AI_OK) return NULL;
    if (!s || !n) { w->error = EPD_AI_INVALID; return NULL; }
    if (n > limit) { w->error = EPD_AI_LIMIT; return NULL; }
    return ai_json_parse(s, n, &w->error);
}

static ai_cjson *parse_string(writer_t *w, const char *s, size_t limit)
{ return parse(w, s, bounded_length(s, limit), limit); }

static void schema(writer_t *w, const char *s, bool tool)
{
    ai_cjson *v = parse_string(w, s, EPD_AI_REQUEST_LIMIT);
    if (v && (!ai_cjson_IsObject(v) || (tool && !equal(field(v, "type"), "object"))))
        w->error = EPD_AI_INVALID;
    if (w->error == EPD_AI_OK) json(w, v, 0);
    ai_cjson_Delete(v);
}

static bool allowed_keys(const ai_cjson *v, const char *const *names, size_t count)
{
    if (!ai_cjson_IsObject(v)) return false;
    for (const ai_cjson *p = v->child; p; p = p->next)
    {
        size_t i;
        for (i = 0; i < count; ++i) if (!strcmp(p->string, names[i])) break;
        if (i == count) return false;
    }
    return true;
}

static bool one_of(const ai_cjson *v, const char *const *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (equal(v, values[i])) return true;
    return false;
}

static void extensions(writer_t *w, const ai_profile_t *p, const epd_ai_model_t *m,
                       const epd_ai_request_t *r)
{
    if (!r->extensions_json) return;
    ai_cjson *v = parse_string(w, r->extensions_json, EPD_AI_ARGUMENT_LIMIT);
    if (!v) return;
    if (!ai_cjson_IsObject(v)) w->error = EPD_AI_INVALID;
    for (ai_cjson *x = v->child; x && w->error == EPD_AI_OK; x = x->next)
    {
        bool ok = false;
        bool native = (m->capabilities & EPD_AI_CAP_NATIVE) != 0;
        static const char *const efforts[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max" };
        if (p->public.protocol == EPD_AI_RESPONSES && (p->public.features & EPD_AI_FEATURE_OPENAI) &&
            native && !strcmp(x->string, "reasoning"))
        {
            static const char *const keys[] = { "effort", "summary" };
            static const char *const summaries[] = { "auto", "concise", "detailed" };
            ok = allowed_keys(x, keys, 2) && x->child &&
                 (!field(x, "effort") || one_of(field(x, "effort"), efforts, 7)) &&
                 (!field(x, "summary") || one_of(field(x, "summary"), summaries, 3));
        }
        else if (p->public.protocol == EPD_AI_CHAT && (p->public.features & EPD_AI_FEATURE_OPENAI) &&
                 native && !strcmp(x->string, "reasoning_effort")) ok = one_of(x, efforts, 7);
        else if (p->public.protocol == EPD_AI_CHAT && (p->public.features & EPD_AI_FEATURE_DEEPSEEK) &&
                 native && !strcmp(x->string, "reasoning_effort"))
        {
            static const char *const deepseek_efforts[] = { "low", "high", "max" };
            ok = one_of(x, deepseek_efforts, 3);
        }
        else if (p->public.protocol == EPD_AI_CHAT && (p->public.features & EPD_AI_FEATURE_DEEPSEEK) &&
                 native && !strcmp(x->string, "thinking"))
        {
            static const char *const keys[] = { "type" };
            ok = allowed_keys(x, keys, 1) &&
                 (equal(field(x, "type"), "enabled") || equal(field(x, "type"), "disabled"));
        }
        else if (p->public.protocol == EPD_AI_MESSAGES && native && !strcmp(x->string, "thinking"))
        {
            static const char *const keys[] = { "type", "budget_tokens" };
            ai_cjson *budget = field(x, "budget_tokens");
            ok = allowed_keys(x, keys, 2);
            if (equal(field(x, "type"), "enabled"))
                ok = ok && ai_cjson_IsNumber(budget) && budget->valuedouble >= 1024 &&
                     budget->valuedouble < r->max_output_tokens && floor(budget->valuedouble) == budget->valuedouble;
            else ok = ok && !budget && (equal(field(x, "type"), "disabled") || equal(field(x, "type"), "adaptive"));
            if (!equal(field(x, "type"), "disabled") &&
                ((r->has_temperature && r->temperature != 1.0) || r->has_top_p)) ok = false;
        }
        if (!ok) { w->error = EPD_AI_UNSUPPORTED; break; }
        raw(w, ","); string(w, x->string); raw(w, ":"); json(w, x, 0);
    }
    ai_cjson_Delete(v);
}

static void clear_calls(calls_t *calls)
{
    for (size_t i = 0; i < calls->count; ++i) ai_free(calls->id[i]);
    memset(calls, 0, sizeof(*calls));
}

static bool calls_done(const calls_t *calls)
{
    for (size_t i = 0; i < calls->count; ++i) if (!calls->done[i]) return false;
    return true;
}

static void add_call(writer_t *w, calls_t *calls, const ai_cjson *id,
                     const ai_cjson *name, const ai_cjson *arguments, bool encoded)
{
    if (w->error != EPD_AI_OK) return;
    if (!nonempty(id) || !nonempty(name) || !identifier(name->valuestring, 64))
    { w->error = EPD_AI_INVALID; return; }
    if (calls->count == EPD_AI_MAX_TOOLS) { w->error = EPD_AI_LIMIT; return; }
    for (size_t i = 0; i < calls->count; ++i)
        if (!strcmp(calls->id[i], id->valuestring)) { w->error = EPD_AI_INVALID; return; }
    if (encoded)
    {
        if (!ai_cjson_IsString(arguments)) { w->error = EPD_AI_INVALID; return; }
        ai_cjson *args = parse_string(w, arguments->valuestring, EPD_AI_ARGUMENT_LIMIT);
        if (args && !ai_cjson_IsObject(args)) w->error = EPD_AI_INVALID;
        ai_cjson_Delete(args);
    }
    else
    {
        writer_t measure = {0};
        if (!ai_cjson_IsObject(arguments)) { w->error = EPD_AI_INVALID; return; }
        json(&measure, arguments, 0);
        if (measure.error) w->error = measure.error;
        if (measure.used > EPD_AI_ARGUMENT_LIMIT) w->error = EPD_AI_LIMIT;
    }
    if (w->error != EPD_AI_OK) return;
    char *copy = ai_strndup(id->valuestring, strlen(id->valuestring));
    if (!copy) { w->error = EPD_AI_NO_MEMORY; return; }
    calls->id[calls->count++] = copy;
}

static void native_calls(writer_t *w, const ai_profile_t *profile, const ai_cjson *data, calls_t *calls)
{
    epd_ai_protocol_t protocol = profile->public.protocol;
    if (protocol == EPD_AI_CHAT)
    {
        if (!ai_cjson_IsObject(data) || !equal(field(data, "role"), "assistant"))
        { w->error = EPD_AI_INVALID; return; }
        ai_cjson *content = field(data, "content"), *reason = field(data, "reasoning_content");
        if ((content && !ai_cjson_IsNull(content) && !ai_cjson_IsString(content)) ||
            (reason && !ai_cjson_IsNull(reason) && !ai_cjson_IsString(reason)))
        { w->error = EPD_AI_UNSUPPORTED; return; }
        ai_cjson *tools = field(data, "tool_calls");
        if (tools && !ai_cjson_IsNull(tools) && !ai_cjson_IsArray(tools))
        { w->error = EPD_AI_INVALID; return; }
        if (!content && !tools) { w->error = EPD_AI_INVALID; return; }
        for (ai_cjson *t = tools ? tools->child : NULL; t; t = t->next)
        {
            if (!equal(field(t, "type"), "function")) { w->error = EPD_AI_UNSUPPORTED; break; }
            ai_cjson *fn = field(t, "function");
            add_call(w, calls, field(t, "id"), field(fn, "name"), field(fn, "arguments"), true);
        }
        return;
    }
    if (!ai_cjson_IsArray(data) || !data->child) { w->error = EPD_AI_INVALID; return; }
    for (ai_cjson *t = data->child; t && !w->error; t = t->next)
    {
        ai_cjson *type = field(t, "type");
        if (protocol == EPD_AI_RESPONSES)
        {
            ai_cjson *status = field(t, "status");
            if (status && !equal(status, "completed") &&
                !(equal(type, "web_search_call") && equal(status, "failed")))
            { w->error = EPD_AI_INCOMPLETE; break; }
            if (equal(type, "function_call"))
                add_call(w, calls, field(t, "call_id"), field(t, "name"), field(t, "arguments"), true);
            else if (equal(type, "reasoning"))
            {
                if (profile->public.features & EPD_AI_FEATURE_DEEPSEEK) {
                    if (!ai_cjson_IsArray(field(t, "content"))) w->error = EPD_AI_UNSUPPORTED;
                } else if (!nonempty(field(t, "encrypted_content"))) w->error = EPD_AI_UNSUPPORTED;
            }
            else if (equal(type, "web_search_call"))
            { if (!nonempty(field(t, "id"))) w->error = EPD_AI_INVALID; }
            else if (equal(type, "message"))
            {
                ai_cjson *content = field(t, "content");
                if (!equal(field(t, "role"), "assistant") || !ai_cjson_IsArray(content))
                { w->error = EPD_AI_INVALID; break; }
                for (ai_cjson *c = content->child; c; c = c->next)
                    if (!((equal(field(c, "type"), "output_text") && ai_cjson_IsString(field(c, "text"))) ||
                          (equal(field(c, "type"), "refusal") && ai_cjson_IsString(field(c, "refusal")))))
                        w->error = EPD_AI_UNSUPPORTED;
            }
            else w->error = EPD_AI_UNSUPPORTED;
        }
        else if (equal(type, "tool_use"))
            add_call(w, calls, field(t, "id"), field(t, "name"), field(t, "input"), false);
        else if (equal(type, "text"))
        { if (!ai_cjson_IsString(field(t, "text"))) w->error = EPD_AI_INVALID; }
        else if (equal(type, "thinking"))
        {
            if (!ai_cjson_IsString(field(t, "thinking")) || !nonempty(field(t, "signature")))
                w->error = EPD_AI_INVALID;
        }
        else if (equal(type, "redacted_thinking"))
        { if (!nonempty(field(t, "data"))) w->error = EPD_AI_INVALID; }
        else if (equal(type, "server_tool_use"))
        {
            if (!equal(field(t, "name"), "web_search") || !nonempty(field(t, "id")) ||
                !ai_cjson_IsObject(field(t, "input"))) w->error = EPD_AI_UNSUPPORTED;
        }
        else if (equal(type, "web_search_tool_result"))
        {
            if (!nonempty(field(t, "tool_use_id")) ||
                (!ai_cjson_IsArray(field(t, "content")) &&
                 !equal(field(field(t, "content"), "type"), "web_search_tool_result_error")))
                w->error = EPD_AI_INVALID;
        }
        else w->error = EPD_AI_UNSUPPORTED;
    }
}

static void comma(writer_t *w, bool *first)
{ if (!*first) raw(w, ","); *first = false; }

static void native_message(writer_t *w, const ai_profile_t *p, const epd_ai_request_t *r,
                           const epd_ai_message_t *message, calls_t *calls, bool *first)
{
    if (message->text || message->text_size || message->call_id || message->tool_error)
    { w->error = EPD_AI_INVALID; return; }
    ai_cjson *v = parse(w, message->native, message->native_size, EPD_AI_NATIVE_LIMIT);
    if (!v) return;
    static const char *const keys[] = { "version", "protocol", "profile", "revision", "model", "data" };
    if (!allowed_keys(v, keys, 6) || !integer(field(v, "version"), 1) ||
        !integer(field(v, "protocol"), p->public.protocol) || !equal(field(v, "profile"), p->public.id) ||
        !integer(field(v, "revision"), p->public.identity_revision) || !equal(field(v, "model"), r->model))
        w->error = EPD_AI_INVALID;
    ai_cjson *data = field(v, "data");
    if (!w->error) native_calls(w, p, data, calls);
    if (!w->error)
    {
        if (p->public.protocol == EPD_AI_RESPONSES)
            for (ai_cjson *item = data->child; item; item = item->next)
            { comma(w, first); json(w, item, 0); }
        else
        {
            comma(w, first);
            if (p->public.protocol == EPD_AI_MESSAGES) raw(w, "{\"role\":\"assistant\",\"content\":");
            json(w, data, 0);
            if (p->public.protocol == EPD_AI_MESSAGES) raw(w, "}");
        }
    }
    ai_cjson_Delete(v);
}

static void text(writer_t *w, const epd_ai_message_t *m)
{
    size_t n = m->text_size ? m->text_size : bounded_length(m->text, EPD_AI_REQUEST_LIMIT);
    if (n > EPD_AI_REQUEST_LIMIT) { w->error = EPD_AI_LIMIT; return; }
    if (!m->text && n) { w->error = EPD_AI_INVALID; return; }
    quoted(w, m->text, n);
}

static void match_result(writer_t *w, const epd_ai_message_t *m, calls_t *calls)
{
    if (!m->call_id || bounded_length(m->call_id, EPD_AI_NATIVE_LIMIT) > EPD_AI_NATIVE_LIMIT)
    { w->error = EPD_AI_INVALID; return; }
    for (size_t i = 0; i < calls->count; ++i)
        if (!strcmp(calls->id[i], m->call_id))
        {
            if (calls->done[i]) w->error = EPD_AI_INVALID;
            calls->done[i] = true;
            return;
        }
    w->error = EPD_AI_INVALID;
}

static void messages(writer_t *w, const ai_profile_t *p, const epd_ai_model_t *model,
                     const epd_ai_request_t *r, bool measure_only)
{
    epd_ai_protocol_t protocol = p->public.protocol;
    calls_t calls = {0};
    bool first = true;
    raw(w, protocol == EPD_AI_RESPONSES ? ",\"input\":[" : ",\"messages\":[");
    if (protocol == EPD_AI_CHAT && r->instructions)
    {
        raw(w, "{\"role\":\"system\",\"content\":"); string(w, r->instructions); raw(w, "}"); first = false;
    }
    for (size_t i = 0; i < r->message_count && !w->error; ++i)
    {
        const epd_ai_message_t *m = &r->messages[i];
        if (m->kind == EPD_AI_NATIVE_ASSISTANT)
        {
            if (!(model->capabilities & EPD_AI_CAP_NATIVE)) { w->error = EPD_AI_UNSUPPORTED; break; }
            if (!calls_done(&calls)) { w->error = EPD_AI_INVALID; break; }
            if (i && (r->messages[i-1].kind == EPD_AI_ASSISTANT ||
                      r->messages[i-1].kind == EPD_AI_NATIVE_ASSISTANT))
            { w->error = EPD_AI_INVALID; break; }
            clear_calls(&calls);
            native_message(w, p, r, m, &calls, &first);
            if (!w->error && calls.count && !(model->capabilities & EPD_AI_CAP_TOOLS)) w->error = EPD_AI_UNSUPPORTED;
            continue;
        }
        if (m->native || m->native_size) { w->error = EPD_AI_INVALID; break; }
        if (m->kind == EPD_AI_TOOL_RESULT)
        {
            if (!(model->capabilities & EPD_AI_CAP_TOOLS)) { w->error = EPD_AI_UNSUPPORTED; break; }
            if (protocol == EPD_AI_MESSAGES)
            {
                comma(w, &first); raw(w, "{\"role\":\"user\",\"content\":[");
                bool block_first = true;
                for (; i < r->message_count && r->messages[i].kind == EPD_AI_TOOL_RESULT; ++i)
                {
                    m = &r->messages[i];
                    if (m->native || m->native_size) { w->error = EPD_AI_INVALID; break; }
                    match_result(w, m, &calls); comma(w, &block_first);
                    raw(w, "{\"type\":\"tool_result\",\"tool_use_id\":"); string(w, m->call_id);
                    raw(w, ",\"is_error\":"); raw(w, m->tool_error ? "true" : "false");
                    raw(w, ",\"content\":"); text(w, m); raw(w, "}");
                }
                if (!w->error && !calls_done(&calls) && (!measure_only || i < r->message_count)) w->error = EPD_AI_INVALID;
                if (i < r->message_count && r->messages[i].kind == EPD_AI_USER)
                {
                    m = &r->messages[i++];
                    if (m->native || m->native_size || m->call_id || m->tool_error) w->error = EPD_AI_INVALID;
                    comma(w, &block_first); raw(w, "{\"type\":\"text\",\"text\":"); text(w, m); raw(w, "}");
                }
                --i;
                raw(w, "]}");
            }
            else
            {
                match_result(w, m, &calls); comma(w, &first);
                raw(w, protocol == EPD_AI_RESPONSES ? "{\"type\":\"function_call_output\",\"call_id\":" :
                                                      "{\"role\":\"tool\",\"tool_call_id\":");
                string(w, m->call_id);
                raw(w, protocol == EPD_AI_RESPONSES ? ",\"output\":" : ",\"content\":");
                text(w, m); raw(w, "}");
            }
            continue;
        }
        if (m->kind != EPD_AI_USER && m->kind != EPD_AI_ASSISTANT)
        { w->error = EPD_AI_INVALID; break; }
        if (!calls_done(&calls) || m->call_id || m->tool_error ||
            (m->kind == EPD_AI_ASSISTANT && i && r->messages[i-1].kind == EPD_AI_NATIVE_ASSISTANT))
        { w->error = EPD_AI_INVALID; break; }
        clear_calls(&calls);
        comma(w, &first);
        raw(w, m->kind == EPD_AI_USER ? "{\"role\":\"user\",\"content\":" : "{\"role\":\"assistant\",\"content\":");
        text(w, m); raw(w, "}");
    }
    /* Capacity measurement can precede tool effects; dispatch needs all results. */
    if (!w->error && !measure_only && !calls_done(&calls)) w->error = EPD_AI_INVALID;
    raw(w, "]");
    clear_calls(&calls);
}

static void tools(writer_t *w, epd_ai_protocol_t protocol, const epd_ai_request_t *r)
{
    bool search = ai_search_requested(r);
    if (!r->tool_count && !search) return;
    raw(w, ",\"tools\":[");
    if (search) raw(w, protocol == EPD_AI_MESSAGES ?
        "{\"type\":\"web_search_20250305\",\"name\":\"web_search\",\"max_uses\":5}" :
        "{\"type\":\"web_search\"}");
    for (size_t i = 0; i < r->tool_count && !w->error; ++i)
    {
        const epd_ai_tool_t *t = &r->tools[i];
        if (!identifier(t->name, 64) || !t->max_result_encoded_bytes)
        { w->error = EPD_AI_INVALID; break; }
        if (t->max_result_encoded_bytes > EPD_AI_REQUEST_LIMIT) { w->error = EPD_AI_LIMIT; break; }
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(t->name, r->tools[j].name)) w->error = EPD_AI_INVALID;
        if (i || search) raw(w, ",");
        if (protocol == EPD_AI_CHAT) raw(w, "{\"type\":\"function\",\"function\":{");
        else if (protocol == EPD_AI_RESPONSES) raw(w, "{\"type\":\"function\",");
        else raw(w, "{");
        raw(w, "\"name\":"); string(w, t->name);
        if (t->description) { raw(w, ",\"description\":"); string(w, t->description); }
        raw(w, protocol == EPD_AI_MESSAGES ? ",\"input_schema\":" : ",\"parameters\":");
        schema(w, t->parameters_json, true);
        raw(w, ",\"strict\":"); raw(w, t->strict ? "true" : "false");
        raw(w, protocol == EPD_AI_CHAT ? "}}" : "}");
    }
    raw(w, "]");
}

static void format(writer_t *w, epd_ai_protocol_t protocol, const epd_ai_request_t *r)
{
    if (r->format == EPD_AI_FORMAT_TEXT) return;
    if (protocol == EPD_AI_RESPONSES) raw(w, ",\"text\":{\"format\":{");
    else if (protocol == EPD_AI_CHAT) raw(w, ",\"response_format\":{");
    else raw(w, ",\"output_config\":{\"format\":{");
    if (r->format == EPD_AI_FORMAT_JSON) raw(w, "\"type\":\"json_object\"");
    else
    {
        raw(w, "\"type\":\"json_schema\",");
        if (protocol == EPD_AI_CHAT) raw(w, "\"json_schema\":{");
        if (protocol != EPD_AI_MESSAGES)
        {
            raw(w, "\"name\":"); string(w, r->schema_name);
            raw(w, ",\"strict\":true,");
        }
        raw(w, "\"schema\":"); schema(w, r->schema_json, false);
        if (protocol == EPD_AI_CHAT) raw(w, "}");
    }
    raw(w, protocol == EPD_AI_CHAT ? "}" : "}}");
}

static void encode(writer_t *w, const ai_profile_t *p, const epd_ai_model_t *model,
                   const epd_ai_request_t *r, bool measure_only)
{
    epd_ai_protocol_t protocol = p->public.protocol;
    raw(w, "{\"model\":"); string(w, r->model);
    raw(w, ",\"stream\":"); raw(w, r->stream ? "true" : "false");
    if (protocol != EPD_AI_MESSAGES && !(p->public.features & EPD_AI_FEATURE_DEEPSEEK)) raw(w, ",\"store\":false");
    if (protocol == EPD_AI_CHAT && r->stream) raw(w, ",\"stream_options\":{\"include_usage\":true}");
    if (protocol == EPD_AI_RESPONSES &&
        ((p->public.features & EPD_AI_FEATURE_ENCRYPTED_REASONING) ||
         (ai_search_requested(r) && (p->public.features & EPD_AI_FEATURE_OPENAI))) &&
        !(p->public.features & EPD_AI_FEATURE_DEEPSEEK))
        raw(w, ",\"include\":[\"reasoning.encrypted_content\"]");
    raw(w, protocol == EPD_AI_RESPONSES ? ",\"max_output_tokens\":" :
           protocol == EPD_AI_CHAT && model->token_field == EPD_AI_MAX_COMPLETION_TOKENS ?
               ",\"max_completion_tokens\":" : ",\"max_tokens\":");
    number(w, r->max_output_tokens);
    if (r->instructions && protocol != EPD_AI_CHAT)
    { raw(w, protocol == EPD_AI_MESSAGES ? ",\"system\":" : ",\"instructions\":"); string(w, r->instructions); }
    if (r->has_temperature) { raw(w, ",\"temperature\":"); number(w, r->temperature); }
    if (r->has_top_p) { raw(w, ",\"top_p\":"); number(w, r->top_p); }
    if (r->stop_count)
    {
        raw(w, protocol == EPD_AI_MESSAGES ? ",\"stop_sequences\":[" : ",\"stop\":[");
        for (size_t i = 0; i < r->stop_count; ++i)
        {
            if (!r->stop[i] || !*r->stop[i]) { w->error = EPD_AI_INVALID; break; }
            if (i) raw(w, ",");
            string(w, r->stop[i]);
        }
        raw(w, "]");
    }
    messages(w, p, model, r, measure_only);
    tools(w, protocol, r);
    format(w, protocol, r);
    extensions(w, p, model, r);
    raw(w, "}");
}

epd_ai_error_t ai_protocol_encode(const ai_profile_t *p, const epd_ai_request_t *r,
                                char **body, size_t *size)
{
    if (body) *body = NULL;
    if (size) *size = 0;
    if (!p || !r || !size || r->struct_size < EPD_AI_REQUEST_V1_SIZE || r->version != EPD_AI_VERSION ||
        !r->model || bounded_length(r->model, EPD_AI_MODEL_SIZE - 1) >= EPD_AI_MODEL_SIZE ||
        p->public.protocol > EPD_AI_MESSAGES || p->public.protocol < EPD_AI_RESPONSES ||
        p->public.model_count > EPD_AI_MAX_MODELS || !r->message_count || !r->messages ||
        (r->tool_count && !r->tools) || (r->stop_count && !r->stop) || !r->max_output_tokens ||
        r->format < EPD_AI_FORMAT_TEXT || r->format > EPD_AI_FORMAT_SCHEMA)
        return EPD_AI_INVALID;
    if (r->message_count > 4096 || r->tool_count > EPD_AI_MAX_TOOLS || r->stop_count > 4) return EPD_AI_LIMIT;
    if ((r->has_temperature && (!isfinite(r->temperature) || r->temperature < 0 ||
             r->temperature > (p->public.protocol == EPD_AI_MESSAGES ? 1 : 2))) ||
        (r->has_top_p && (!isfinite(r->top_p) || r->top_p < 0 || r->top_p > 1))) return EPD_AI_INVALID;
    if (r->format == EPD_AI_FORMAT_SCHEMA && (!r->schema_json || !identifier(r->schema_name, 64)))
        return EPD_AI_INVALID;
    if (r->format != EPD_AI_FORMAT_SCHEMA && (r->schema_name || r->schema_json)) return EPD_AI_INVALID;
    if (p->public.protocol == EPD_AI_RESPONSES && r->stop_count) return EPD_AI_UNSUPPORTED;
    if (p->public.protocol == EPD_AI_MESSAGES && r->format == EPD_AI_FORMAT_JSON) return EPD_AI_UNSUPPORTED;
    const epd_ai_model_t *model = NULL;
    for (size_t i = 0; i < p->public.model_count; ++i)
        if (bounded_length(p->public.models[i].id, EPD_AI_MODEL_SIZE - 1) < EPD_AI_MODEL_SIZE &&
            !strcmp(p->public.models[i].id, r->model)) { model = &p->public.models[i]; break; }
    if (!model) return EPD_AI_NOT_FOUND;
    if (ai_search_requested(r) && ai_model_search_support(&p->public, model) != AI_SEARCH_SUPPORTED)
        return EPD_AI_UNSUPPORTED;
    if (r->max_output_tokens > model->max_output_tokens) return EPD_AI_LIMIT;
    if (p->public.protocol == EPD_AI_CHAT && model->token_field != EPD_AI_MAX_TOKENS &&
        model->token_field != EPD_AI_MAX_COMPLETION_TOKENS) return EPD_AI_INVALID;
    uint32_t needed = EPD_AI_CAP_TEXT | (r->stream ? EPD_AI_CAP_STREAM : 0) |
        (r->tool_count ? EPD_AI_CAP_TOOLS : 0) | (r->format == EPD_AI_FORMAT_JSON ? EPD_AI_CAP_JSON : 0) |
        (r->format == EPD_AI_FORMAT_SCHEMA ? EPD_AI_CAP_SCHEMA : 0) |
        (ai_search_requested(r) ? EPD_AI_CAP_WEB_SEARCH | EPD_AI_CAP_NATIVE : 0);
    if ((model->capabilities & needed) != needed) return EPD_AI_UNSUPPORTED;
    if (!ai_json_begin()) return EPD_AI_BUSY;
    writer_t measure = {0};
    encode(&measure, p, model, r, body == NULL);
    *size = measure.used;
    epd_ai_error_t error = measure.error;
    if (!error && measure.used > EPD_AI_REQUEST_LIMIT) error = EPD_AI_LIMIT;
    if (!error && body)
    {
        char *buffer = ai_alloc(measure.used + 1);
        if (!buffer) error = EPD_AI_NO_MEMORY;
        else
        {
            writer_t write = { buffer, 0, measure.used, EPD_AI_OK };
            encode(&write, p, model, r, false);
            error = write.error;
            if (!error && write.used != measure.used) error = EPD_AI_INVALID;
            if (error) ai_free(buffer);
            else { buffer[write.used] = 0; *body = buffer; }
        }
    }
    ai_json_end();
    return error;
}
