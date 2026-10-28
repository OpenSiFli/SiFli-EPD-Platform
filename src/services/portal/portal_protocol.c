#include "portal_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool copy(char *out, size_t size, const char *value)
{
    size_t n = strlen(value);
    if (n >= size) return false;
    memcpy(out, value, n + 1); return true;
}
static bool same(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}
static bool decimal(const char *value, uint64_t *out)
{
    uint64_t result = 0;
    if (!*value) return false;
    for (; *value; ++value)
    {
        if (*value < '0' || *value > '9' || result > (UINT64_MAX - (*value - '0')) / 10) return false;
        result = result * 10 + (*value - '0');
    }
    *out = result; return true;
}
static bool token_char(unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c)); }
static bool cookies(portal_request_t *out)
{
    const char *p = out->cookie, *names[32]; size_t lengths[32]; unsigned count = 0;
    if (!*p) return false;
    while (*p)
    {
        while (*p == ' ' || *p == '\t') ++p;
        const char *name = p; while (token_char((unsigned char)*p)) ++p;
        size_t n = (size_t)(p - name);
        if (!n || *p++ != '=' || count == 32) return false;
        for (unsigned i = 0; i < count; ++i) if (lengths[i] == n && !memcmp(names[i], name, n)) return false;
        names[count] = name; lengths[count++] = n;
        const char *value = p;
        while (*p && *p != ';')
        {
            unsigned char c = (unsigned char)*p++;
            if (!(c == 0x21 || (c >= 0x23 && c <= 0x2b) || (c >= 0x2d && c <= 0x3a) ||
                  (c >= 0x3c && c <= 0x5b) || (c >= 0x5d && c <= 0x7e))) return false;
        }
        if (n == sizeof("epd_session") - 1 && !memcmp(name, "epd_session", n))
        {
            if (p - value != 64) return false;
            for (unsigned i = 0; i < 64; ++i) if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
            memcpy(out->session_token, value, 64); out->session_token[64] = 0;
        }
        if (*p && !*++p) return false;
    }
    return true;
}
int portal_request_parse(char *header, size_t length, portal_request_t *out)
{
    if (!header || !out || length < 4 || length >= PORTAL_HEADER_MAX ||
        memcmp(header + length - 4, "\r\n\r\n", 4) || memchr(header, 0, length)) return 400;
    memset(out, 0, sizeof(*out)); header[length] = 0;
    char *line = strstr(header, "\r\n");
    if (!line) return 400;
    *line = 0;
    char *space = strchr(header, ' ');
    if (!space) return 400;
    *space++ = 0;
    char *version = strchr(space, ' ');
    if (!version) return 400;
    *version++ = 0;
    if ((strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) ||
        !copy(out->method, sizeof(out->method), header) ||
        !copy(out->target, sizeof(out->target), space) || *space != '/') return 400;
    if (!*out->method) return 400;
    for (char *p = out->method; *p; ++p) if (!token_char((unsigned char)*p)) return 400;
    for (char *p = space; *p; ++p) if ((unsigned char)*p <= 32 || *p == 127 || *p == '#' || *p == '\\') return 400;
    unsigned seen = 0, count = 0; char *names[64];
    for (line += 2; *line; )
    {
        if (line[0] == '\r' && line[1] == '\n') { if (line != header + length - 2) return 400; break; }
        char *end = strstr(line, "\r\n"), *colon = strchr(line, ':');
        if (!end || !colon || colon >= end || colon == line) return 400;
        *end = 0; *colon++ = 0;
        for (char *p = line; *p; ++p) if (!(isalnum((unsigned char)*p) || *p == '-')) return 400;
        if (count == 64) return 431;
        for (unsigned i = 0; i < count; ++i) if (same(names[i], line)) return 400;
        names[count++] = line;
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *tail = end;
        while (tail > colon && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        for (char *p = colon; *p; ++p) if (((unsigned char)*p < 32 && *p != '\t') || *p == 127) return 400;
        unsigned bit = same(line, "Content-Length") ? 1 : same(line, "Cookie") ? 2 :
            same(line, "Origin") ? 4 : same(line, "Host") ? 8 : same(line, "Expect") ? 16 :
            same(line, "X-CSRF-Token") ? 32 : same(line, "Content-Type") ? 64 : 0;
        if (bit && (seen & bit)) return 400;
        seen |= bit;
        if (same(line, "Transfer-Encoding")) return 400;
        if (bit == 1) { if (!decimal(colon, &out->length)) return 400; out->has_length = true; }
        if (bit == 2 && !copy(out->cookie, sizeof(out->cookie), colon)) return 400;
        if (bit == 4 && !copy(out->origin, sizeof(out->origin), colon)) return 400;
        if (bit == 8 && !copy(out->host, sizeof(out->host), colon)) return 400;
        if (bit == 16) { if (!same(colon, "100-continue")) return 417; out->expect_continue = true; }
        if (bit == 32 && (!*colon || !copy(out->csrf, sizeof(out->csrf), colon))) return 400;
        if (bit == 64 && (!*colon || !copy(out->content_type, sizeof(out->content_type), colon))) return 400;
        line = end + 2;
    }
    if ((seen & 2) && !cookies(out)) return 400;
    if ((seen & 4) && !out->origin[0]) return 400;
    return (seen & 8) && out->host[0] ? 0 : 400;
}
static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int portal_query_value(const char *target, const char *key, char *out, size_t capacity)
{
    const char *p = strchr(target, '?'); bool found = false;
    if (!capacity) return -1;
    *out = 0;
    if (!p) return 0;
    ++p;
    while (*p)
    {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', end - p);
        if (eq && (size_t)(eq - p) == strlen(key) && !memcmp(p, key, eq - p))
        {
            if (found) return -1;
            found = true; size_t n = 0;
            for (p = eq + 1; p < end; ++p)
            {
                unsigned char c = (unsigned char)*p;
                if (c == '%')
                {
                    if (end - p < 3 || hex(p[1]) < 0 || hex(p[2]) < 0) return -1;
                    c = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
                }
                else if (c == '+') c = ' ';
                if (!c || c < 32 || c == 127 || n + 1 >= capacity) return -1;
                out[n++] = (char)c;
            }
            out[n] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}
bool portal_query(const char *target, const char *key, char *out, size_t capacity)
{ return portal_query_value(target,key,out,capacity)==1; }
static bool utf8(const unsigned char *s)
{
    while (*s)
    {
        unsigned c = *s++, count = 0, min = 0;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; min = 0x10000; c &= 7; }
        else return false;
        while (count--) { if ((*s & 0xc0) != 0x80) return false; c = (c << 6) | (*s++ & 63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
bool portal_path(const char *relative, char *absolute, size_t capacity)
{
    if (!relative || relative[0] != '/' || !utf8((const unsigned char *)relative)) return false;
    const char *segment = relative + 1;
    for (const char *p = segment; ; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 32 || c == 127 || strchr("\\:*?\"<>|~", c))) return false;
        if (!c || c == '/')
        {
            size_t n = p - segment;
            /* FAT aliases must not expose or replace an installer-owned file. */
            static const char reserved[] = ".portal-sap";
            if (n == sizeof(reserved)-1)
            {
                bool same = true;
                for (size_t i=0;i<n;++i)
                {
                    unsigned char ch=(unsigned char)segment[i];
                    if (ch>='A' && ch<='Z') ch+=(unsigned char)('a'-'A');
                    if (ch!=(unsigned char)reserved[i]) same=false;
                }
                if (same) return false;
            }
            if (!n && (c || segment != relative + 1)) return false;
            if (n > 255 || (n && (p[-1] == '.' || p[-1] == ' '))) return false;
            if (!c) break;
            segment = p + 1;
        }
    }
    int n = snprintf(absolute, capacity, "/sdcard%s", !strcmp(relative, "/") ? "" : relative);
    return n > 0 && (size_t)n < capacity;
}
bool portal_path_mutable(const char *relative)
{
    char absolute[PORTAL_PATH_MAX];
    if (!portal_path(relative,absolute,sizeof(absolute))) return false;
    static const char managed[]="/.epd/fonts";
    size_t i=0;
    while (relative[i] && managed[i])
    {
        unsigned char ch=(unsigned char)relative[i];
        if (ch>='A' && ch<='Z') ch+=(unsigned char)('a'-'A');
        if (ch!=(unsigned char)managed[i]) return true;
        ++i;
    }
    if (!relative[i]) return i!=1 && managed[i]!='/' && managed[i]!=0;
    return relative[i]!='/';
}
bool portal_equal_secret(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b); unsigned mismatch = (unsigned)(n ^ m);
    for (size_t i = 0; i < n; ++i) mismatch |= (unsigned char)a[i] ^ (i < m ? (unsigned char)b[i] : 0);
    return mismatch == 0;
}
void portal_json_init(portal_json_t *json, char *data, size_t capacity)
{
    *json = (portal_json_t){.data = data, .capacity = capacity};
    if (capacity) data[0] = 0; else json->failed = true;
}
void portal_json_raw(portal_json_t *json, const char *value)
{
    size_t n = strlen(value);
    if (json->failed || n >= json->capacity - json->used) { json->failed = true; return; }
    memcpy(json->data + json->used, value, n + 1); json->used += n;
}
void portal_json_string(portal_json_t *json, const char *value)
{
    portal_json_raw(json, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
    {
        char escaped[7] = {0};
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = *p; }
        else if (*p < 32) snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
        else escaped[0] = *p;
        portal_json_raw(json, escaped);
    }
    portal_json_raw(json, "\"");
}
void portal_json_number(portal_json_t *json, int64_t value)
{
    char text[24]; snprintf(text, sizeof(text), "%lld", (long long)value); portal_json_raw(json, text);
}
