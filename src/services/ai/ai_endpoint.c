/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_internal.h"
#include <string.h>

static unsigned char lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int hex(unsigned char c)
{
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static bool sensitive_query(const char *path)
{
    const char *query = strchr(path, '?');
    if (!query) return false;
    ++query;
    while (*query)
    {
        char key[64];
        size_t length = 0;
        while (*query && *query != '=' && *query != '&' && *query != ';')
        {
            unsigned char c = (unsigned char)*query++;
            if (c == '%')
            {
                if (!query[0] || !query[1] || hex(query[0]) < 0 || hex(query[1]) < 0) return true;
                c = (unsigned char)(hex(query[0]) * 16 + hex(query[1])); query += 2;
            }
            if (c <= 32 || c >= 127 || length + 1 >= sizeof(key)) return true;
            key[length++] = (char)lower(c);
        }
        key[length] = 0;
        if (!strcmp(key, "key") || !strcmp(key, "api_key") || !strcmp(key, "api-key") ||
            !strcmp(key, "apikey") || !strcmp(key, "token") || !strcmp(key, "access_token") ||
            !strcmp(key, "authorization")) return true;
        while (*query && *query != '&' && *query != ';') ++query;
        if (*query) ++query;
    }
    return false;
}

epd_ai_error_t ai_endpoint_parse(const char *url, ai_endpoint_t *endpoint)
{
    if (!url || !endpoint || strncmp(url, "https://", 8)) return EPD_AI_INVALID;
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
    {
        if (++length >= AI_ENDPOINT_SIZE || *p <= 32 || *p >= 127 || *p == '#' || *p == '\\')
            return EPD_AI_INVALID;
        if (*p == '%' && (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0 ||
                         hex(p[1]) * 16 + hex(p[2]) <= 32 || hex(p[1]) * 16 + hex(p[2]) == 127))
            return EPD_AI_INVALID;
    }
    const char *host = url + 8, *end = host;
    while (*end && *end != '/' && *end != '?' && *end != ':') ++end;
    size_t host_size = (size_t)(end - host), label = 0;
    if (!host_size || host_size >= sizeof(endpoint->host)) return EPD_AI_INVALID;
    for (size_t i = 0; i < host_size; ++i)
    {
        unsigned char c = lower((unsigned char)host[i]);
        if (c == '.')
        {
            if (!label || host[i - 1] == '-') return EPD_AI_INVALID;
            label = 0;
        }
        else
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
                (!label && c == '-') || ++label > 63) return EPD_AI_INVALID;
        }
        endpoint->host[i] = (char)c;
    }
    if (!label || host[host_size - 1] == '-') return EPD_AI_INVALID;
    endpoint->host[host_size] = 0;
    unsigned port = 443;
    if (*end == ':')
    {
        port = 0; ++end;
        const char *start = end;
        while (*end >= '0' && *end <= '9')
        {
            port = port * 10 + (unsigned)(*end++ - '0');
            if (port > 65535) return EPD_AI_INVALID;
        }
        if (end == start || !port) return EPD_AI_INVALID;
    }
    if (*end && *end != '/' && *end != '?') return EPD_AI_INVALID;
    endpoint->port = (uint16_t)port;
    if (*end != '/')
    {
        endpoint->path[0] = '/';
        memcpy(endpoint->path + 1, end, strlen(end) + 1);
    }
    else memcpy(endpoint->path, end, strlen(end) + 1);
    return sensitive_query(endpoint->path) ? EPD_AI_INVALID : EPD_AI_OK;
}
