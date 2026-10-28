/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_json.h"
#include <rthw.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct rt_mutex json_mutex;
static bool json_ready;
static rt_thread_t json_owner;

bool ai_json_begin(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!json_ready)
        json_ready = rt_mutex_init(&json_mutex, "ai_json", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = json_ready;
    rt_hw_interrupt_enable(level);
    if (!ready || !rt_thread_self()) return false;
    /* Nested transactions are programming errors; never release an outer lock. */
    if (ai_json_locked()) return false;
    if (rt_mutex_take(&json_mutex, RT_WAITING_FOREVER) != RT_EOK) return false;
    level = rt_hw_interrupt_disable();
    json_owner = rt_thread_self();
    rt_hw_interrupt_enable(level);
    return true;
}

int ai_json_locked(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int locked = json_owner && json_owner == rt_thread_self();
    rt_hw_interrupt_enable(level);
    return locked;
}

void ai_json_end(void)
{
    if (!ai_json_locked()) return;
    rt_base_t level = rt_hw_interrupt_disable();
    json_owner = NULL;
    rt_hw_interrupt_enable(level);
    rt_mutex_release(&json_mutex);
}

typedef struct {
    const unsigned char *s;
    size_t n, p, nodes;
    epd_ai_error_t error;
} scan_t;

static void whitespace(scan_t *s)
{
    while (s->p < s->n && (s->s[s->p] == ' ' || s->s[s->p] == '\t' ||
           s->s[s->p] == '\r' || s->s[s->p] == '\n')) ++s->p;
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(scan_t *s, uint32_t *value)
{
    *value = 0;
    if (s->n - s->p < 4) return false;
    for (unsigned i = 0; i < 4; ++i)
    {
        int h = hex(s->s[s->p++]);
        if (h < 0) return false;
        *value = (*value << 4) | (unsigned)h;
    }
    return true;
}

static bool utf8_scalar(scan_t *s, uint32_t *cp)
{
    if (s->p == s->n) return false;
    unsigned c = s->s[s->p++], more;
    uint32_t min;
    if (c < 0x80) { *cp = c; return c != 0; }
    if (c >= 0xc2 && c <= 0xdf) { more = 1; min = 0x80; c &= 31; }
    else if (c >= 0xe0 && c <= 0xef) { more = 2; min = 0x800; c &= 15; }
    else if (c >= 0xf0 && c <= 0xf4) { more = 3; min = 0x10000; c &= 7; }
    else return false;
    if (more > s->n - s->p) return false;
    while (more--)
    {
        unsigned tail = s->s[s->p++];
        if ((tail & 0xc0) != 0x80) return false;
        c = (c << 6) | (tail & 63);
    }
    *cp = c;
    return c >= min && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff);
}

int ai_json_utf8(const char *text, size_t size)
{
    if (!text) return size == 0;
    scan_t s = { (const unsigned char *)text, size, 0, 0, EPD_AI_OK };
    uint32_t cp;
    while (s.p < s.n) if (!utf8_scalar(&s, &cp)) return 0;
    return 1;
}

/* Validate escapes and UTF-8 scalars without allocating decoded strings. */
static bool string_scalar(scan_t *s, uint32_t *cp)
{
    if (s->p >= s->n || s->s[s->p] < 32) return false;
    if (s->s[s->p] != '\\') return utf8_scalar(s, cp);
    if (++s->p == s->n) return false;
    switch (s->s[s->p++])
    {
    case '"': *cp = '"'; return true;
    case '\\': *cp = '\\'; return true;
    case '/': *cp = '/'; return true;
    case 'b': *cp = 8; return true;
    case 'f': *cp = 12; return true;
    case 'n': *cp = 10; return true;
    case 'r': *cp = 13; return true;
    case 't': *cp = 9; return true;
    case 'u':
        if (!hex4(s, cp) || !*cp) return false; /* cJSON strings are NUL terminated. */
        if (*cp >= 0xd800 && *cp <= 0xdbff)
        {
            uint32_t low;
            if (s->n - s->p < 6 || s->s[s->p++] != '\\' || s->s[s->p++] != 'u' ||
                !hex4(s, &low) || low < 0xdc00 || low > 0xdfff) return false;
            *cp = 0x10000 + ((*cp - 0xd800) << 10) + low - 0xdc00;
        }
        return !(*cp >= 0xdc00 && *cp <= 0xdfff);
    default: return false;
    }
}

static bool string(scan_t *s)
{
    if (s->p == s->n || s->s[s->p++] != '"') return false;
    uint32_t cp;
    while (s->p < s->n && s->s[s->p] != '"')
        if (!string_scalar(s, &cp)) return false;
    if (s->p == s->n) return false;
    ++s->p;
    return true;
}

static bool value(scan_t *s, unsigned depth)
{
    whitespace(s);
    if (++s->nodes > 4096) { s->error = EPD_AI_LIMIT; return false; }
    if (s->p == s->n) return false;
    unsigned c = s->s[s->p];
    if (c == '"') return string(s);
    if (c == '[' || c == '{')
    {
        if (depth == 32) { s->error = EPD_AI_LIMIT; return false; }
        unsigned close = c == '[' ? ']' : '}';
        ++s->p;
        whitespace(s);
        if (s->p < s->n && s->s[s->p] == close) { ++s->p; return true; }
        for (;;)
        {
            if (c == '{')
            {
                if (!string(s)) return false;
                whitespace(s);
                if (s->p == s->n || s->s[s->p++] != ':') return false;
            }
            if (!value(s, depth + 1)) return false;
            whitespace(s);
            if (s->p == s->n) return false;
            if (s->s[s->p] == close) { ++s->p; return true; }
            if (s->s[s->p++] != ',') return false;
            whitespace(s);
        }
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal)
    {
        size_t n = strlen(literal);
        if (n > s->n - s->p || memcmp(s->s + s->p, literal, n)) return false;
        s->p += n;
        return true;
    }
    size_t number_start = s->p;
    if (c == '-') ++s->p;
    if (s->p == s->n) return false;
    if (s->s[s->p] == '0') ++s->p;
    else
    {
        if (s->s[s->p] < '1' || s->s[s->p] > '9') return false;
        while (s->p < s->n && s->s[s->p] >= '0' && s->s[s->p] <= '9') ++s->p;
    }
    if (s->p < s->n && s->s[s->p] == '.')
    {
        size_t start = ++s->p;
        while (s->p < s->n && s->s[s->p] >= '0' && s->s[s->p] <= '9') ++s->p;
        if (s->p == start) return false;
    }
    if (s->p < s->n && (s->s[s->p] == 'e' || s->s[s->p] == 'E'))
    {
        ++s->p;
        if (s->p < s->n && (s->s[s->p] == '+' || s->s[s->p] == '-')) ++s->p;
        size_t start = s->p;
        while (s->p < s->n && s->s[s->p] >= '0' && s->s[s->p] <= '9') ++s->p;
        if (s->p == start) return false;
    }
    /* The underlying number reader has a 64-byte conversion buffer. Reject
     * numbers it would truncate or turn into an infinity before allocating. */
    size_t number_size = s->p - number_start;
    if (number_size >= 64) return false;
    char numeric[64];
    memcpy(numeric, s->s + number_start, number_size);
    numeric[number_size] = 0;
    return isfinite(strtod(numeric, NULL));
}

int ai_json_preflight(const char *text, size_t size)
{
    if (!ai_json_locked() || !text || !size) return EPD_AI_INVALID;
    if (size > EPD_AI_EVENT_LIMIT) return EPD_AI_LIMIT;
    scan_t scan = { (const unsigned char *)text, size, 0, 0, EPD_AI_INVALID };
    if (!value(&scan, 0)) return scan.error;
    whitespace(&scan);
    return scan.p == scan.n ? EPD_AI_OK : EPD_AI_INVALID;
}

ai_cjson *ai_json_parse(const char *text, size_t size, epd_ai_error_t *error)
{
    epd_ai_error_t e = EPD_AI_INVALID;
    ai_cjson *json = NULL;
    if (size > EPD_AI_EVENT_LIMIT) e = EPD_AI_LIMIT;
    else if (ai_json_locked() && text && size && text[size - 1] != '\0')
    {
        /* The private parser runs the quota/grammar preflight once, then
         * rejects duplicate decoded keys while parsing each object. */
        json = ai_cjson_ParseWithLength(text, size);
        e = (epd_ai_error_t)ai_cjson_GetParseError();
    }
    if (error) *error = e;
    return json;
}

char *ai_json_print(const ai_cjson *json, epd_ai_error_t *error)
{
    char *text = NULL;
    epd_ai_error_t e = EPD_AI_INVALID;
    if (ai_json_locked() && json)
    {
        text = ai_cjson_PrintUnformatted(json);
        e = text ? EPD_AI_OK : EPD_AI_NO_MEMORY;
        if (text && strlen(text) > EPD_AI_EVENT_LIMIT)
        { ai_free(text); text = NULL; e = EPD_AI_LIMIT; }
    }
    if (error) *error = e;
    return text;
}
