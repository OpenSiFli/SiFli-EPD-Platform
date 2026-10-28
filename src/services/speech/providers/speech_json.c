/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_wire.h"
#include <limits.h>
#include <string.h>

bool sp_utf8_feed(sp_utf8 *s, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = b[i];
        if (s->need) {
            if ((c & 0xc0u) != 0x80u) return false;
            s->value = (s->value << 6) | (c & 63u);
            if (!--s->need && (s->value < s->minimum || s->value > 0x10ffffu ||
                              (s->value >= 0xd800u && s->value <= 0xdfffu))) return false;
        } else if (c < 0x80u) {
            if (!c) return false;
        } else if (c >= 0xc2u && c <= 0xdfu) {
            s->value = c & 31u; s->minimum = 0x80; s->need = 1;
        } else if (c >= 0xe0u && c <= 0xefu) {
            s->value = c & 15u; s->minimum = 0x800; s->need = 2;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            s->value = c & 7u; s->minimum = 0x10000; s->need = 3;
        } else return false;
    }
    return true;
}

bool sp_utf8_text(const char *p, size_t n, size_t *characters)
{
    sp_utf8 state = {0};
    size_t count = 0;
    if ((!p && n) || !sp_utf8_feed(&state, p, n) || state.need) return false;
    for (size_t i = 0; i < n; ++i) if (((unsigned char)p[i] & 0xc0u) != 0x80u) ++count;
    if (characters) *characters = count;
    return true;
}

static void space(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) ++*p;
}
static int hex(unsigned c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}
static bool unicode(const char **p, const char *end, uint32_t *cp)
{
    uint32_t value = 0;
    if (end - *p < 4) return false;
    for (unsigned i = 0; i < 4; ++i) {
        int v = hex((unsigned char)*(*p)++);
        if (v < 0) return false;
        value = value * 16u + (unsigned)v;
    }
    if (value >= 0xd800 && value <= 0xdbff) {
        uint32_t low = 0;
        if (end - *p < 6 || (*p)[0] != '\\' || (*p)[1] != 'u') return false;
        *p += 2;
        for (unsigned i = 0; i < 4; ++i) {
            int v = hex((unsigned char)*(*p)++);
            if (v < 0) return false;
            low = low * 16u + (unsigned)v;
        }
        if (low < 0xdc00 || low > 0xdfff) return false;
        value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
    } else if (value >= 0xdc00 && value <= 0xdfff) return false;
    if (!value) return false;
    *cp = value;
    return true;
}
static bool string_end(const char **p, const char *end)
{
    sp_utf8 utf = {0};
    if (*p == end || *(*p)++ != '"') return false;
    while (*p < end) {
        unsigned char c = (unsigned char)*(*p)++;
        if (c == '"') return !utf.need;
        if (c < 32) return false;
        if (c == '\\') {
            if (utf.need || *p == end) return false;
            c = (unsigned char)*(*p)++;
            if (c == 'u') { uint32_t cp; if (!unicode(p, end, &cp)) return false; }
            else if (!strchr("\"\\/bfnrt", c)) return false;
        } else if (!sp_utf8_feed(&utf, &c, 1)) return false;
    }
    return false;
}
static bool value(const char **p, const char *end, unsigned depth, sp_json *out)
{
    const char *start;
    char type;
    if (depth > 24) return false;
    space(p, end); start = *p;
    if (start == end) return false;
    type = **p;
    if (type == '"') {
        if (!string_end(p, end)) return false;
    } else if (type == '{' || type == '[') {
        char close = type == '{' ? '}' : ']';
        ++*p; space(p, end);
        if (*p < end && **p == close) ++*p;
        else for (;;) {
            if (type == '{') {
                if (!string_end(p, end)) return false;
                space(p, end);
                if (*p == end || *(*p)++ != ':') return false;
            }
            if (!value(p, end, depth + 1, NULL)) return false;
            space(p, end);
            if (*p == end) return false;
            if (**p == close) { ++*p; break; }
            if (*(*p)++ != ',') return false;
            space(p, end);
        }
    } else if (type == 't' || type == 'f' || type == 'n') {
        const char *literal = type == 't' ? "true" : type == 'f' ? "false" : "null";
        size_t length = strlen(literal);
        if ((size_t)(end - *p) < length || memcmp(*p, literal, length)) return false;
        *p += length;
    } else {
        type = '#';
        if (**p == '-') ++*p;
        if (*p == end || **p < '0' || **p > '9') return false;
        if (**p == '0') ++*p;
        else while (*p < end && **p >= '0' && **p <= '9') ++*p;
        if (*p < end && **p == '.') {
            ++*p;
            if (*p == end || **p < '0' || **p > '9') return false;
            while (*p < end && **p >= '0' && **p <= '9') ++*p;
        }
        if (*p < end && (**p == 'e' || **p == 'E')) {
            ++*p;
            if (*p < end && (**p == '+' || **p == '-')) ++*p;
            if (*p == end || **p < '0' || **p > '9') return false;
            while (*p < end && **p >= '0' && **p <= '9') ++*p;
        }
    }
    if (out) { out->p = start; out->n = (size_t)(*p - start); out->type = type; }
    return true;
}
bool sp_json_parse(const char *p, size_t n, sp_json *out)
{
    const char *end;
    if (!p || !out || n > SP_JSON_MAX) return false;
    end = p + n;
    if (!value(&p, end, 0, out)) return false;
    space(&p, end);
    return p == end;
}
sp_json sp_json_get(sp_json object, const char *name)
{
    sp_json none = {0}, found = {0}, key, item;
    if (object.type != '{') return none;
    const char *p = object.p + 1, *end = object.p + object.n - 1;
    while (p < end) {
        space(&p, end);
        if (p == end) break;
        if (!value(&p, end, 0, &key)) return none;
        space(&p, end);
        if (p == end || *p++ != ':' || !value(&p, end, 0, &item)) return none;
        if (sp_json_is(key, name)) {
            if (found.p) return none; /* Ambiguous protocol fields are invalid. */
            found = item;
        }
        space(&p, end);
        if (p < end && *p++ != ',') return none;
    }
    return found;
}
sp_json sp_json_at(sp_json array, unsigned index)
{
    sp_json none = {0}, item;
    if (array.type != '[') return none;
    const char *p = array.p + 1, *end = array.p + array.n - 1;
    for (unsigned i = 0; p < end; ++i) {
        if (!value(&p, end, 0, &item)) return none;
        if (i == index) return item;
        space(&p, end);
        if (p < end && *p++ != ',') return none;
    }
    return none;
}
bool sp_json_i64(sp_json v, int64_t *out)
{
    uint64_t x = 0;
    size_t i = 0;
    bool negative;
    if (v.type != '#' || !v.n) return false;
    negative = v.p[0] == '-';
    if (negative) ++i;
    for (; i < v.n; ++i) {
        unsigned c = (unsigned char)v.p[i];
        uint64_t limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
        if (c < '0' || c > '9' || x > (limit - (c - '0')) / 10u) return false;
        x = x * 10u + c - '0';
    }
    *out = negative ? (x == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)x) : (int64_t)x;
    return true;
}
bool sp_json_bool(sp_json v, bool *out)
{
    if (v.type != 't' && v.type != 'f') return false;
    *out = v.type == 't'; return true;
}
bool sp_json_string(sp_json v, char *out, size_t capacity, size_t *used)
{
    size_t n = 0;
    const char *p, *end;
    if (v.type != '"' || v.n < 2 || !capacity) return false;
    p = v.p + 1; end = v.p + v.n - 1;
    while (p < end) {
        unsigned char bytes[4]; size_t count = 1;
        bytes[0] = (unsigned char)*p++;
        if (bytes[0] == '\\') {
            unsigned c = (unsigned char)*p++;
            if (c == 'u') {
                uint32_t cp;
                if (!unicode(&p, end, &cp)) return false;
                if (cp < 0x80) bytes[0] = (unsigned char)cp;
                else if (cp < 0x800) {
                    bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
                    bytes[1] = (unsigned char)(0x80 | (cp & 63)); count = 2;
                } else if (cp < 0x10000) {
                    bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
                    bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
                    bytes[2] = (unsigned char)(0x80 | (cp & 63)); count = 3;
                } else {
                    bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
                    bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
                    bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
                    bytes[3] = (unsigned char)(0x80 | (cp & 63)); count = 4;
                }
            } else {
                const char *from = "\"\\/bfnrt", *to = "\"\\/\b\f\n\r\t";
                const char *match = strchr(from, (int)c);
                if (!match) return false;
                bytes[0] = (unsigned char)to[match - from];
            }
        }
        if (count >= capacity - n) return false;
        memcpy(out + n, bytes, count); n += count;
    }
    out[n] = 0;
    if (used) *used = n;
    return true;
}
bool sp_json_is(sp_json v, const char *text)
{
    char decoded[128];
    return sp_json_string(v, decoded, sizeof(decoded), NULL) && !strcmp(decoded, text);
}
bool sp_json_quote(const char *p, size_t n, char *out, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0;
    if (capacity < 3 || !sp_utf8_text(p, n, NULL)) return false;
    out[used++] = '"';
    for (size_t i = 0; i < n; ++i) {
        unsigned c = (unsigned char)p[i];
        size_t count = c < 32 ? 6u : (c == '"' || c == '\\') ? 2u : 1u;
        if (count + 2 > capacity - used) return false;
        if (c < 32) {
            memcpy(out + used, "\\u00", 4); used += 4;
            out[used++] = digits[c >> 4]; out[used++] = digits[c & 15];
        } else {
            if (count == 2) out[used++] = '\\';
            out[used++] = (char)c;
        }
    }
    out[used++] = '"'; out[used] = 0; return true;
}
