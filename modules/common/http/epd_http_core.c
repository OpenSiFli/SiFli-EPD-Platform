/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_http_internal.h"
#include "epd_memory.h"
#include "network.h"
#include <tls_client.h>
#include <lwip/api.h>
#include <lwip/sockets.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

#define LINE_SIZE 4096u
#define HEADERS_LIMIT 32768u
#define BODY_INITIAL 1024u
#define CLOSE_TIMEOUT_MS 100u

bool epd_http_core_fail(http_reader_t *reader, const char *stage, int detail)
{
    reader->response->stage = stage;
    reader->response->detail = detail;
    return false;
}

bool epd_http_core_expired(http_reader_t *reader)
{
    network_snapshot_t route;
    if (reader->request->cancelled && reader->request->cancelled(reader->request->user))
        return !fail(reader, "cancelled", -RT_ERROR);
    if (reader->local_route)
    {
        network_interface_snapshot_t current;
        if (network_interface_snapshot(reader->route.source, &current) != RT_EOK || !current.ready ||
            current.address != reader->local_interface.address ||
            current.netmask != reader->local_interface.netmask ||
            memcmp(current.mac, reader->local_interface.mac, sizeof(current.mac)))
            return !fail(reader, "network-changed", -RT_ERROR);
    }
    else
    {
        network_snapshot(&route);
        if (!route.ready || route.generation != reader->route.generation)
            return !fail(reader, "network-changed", -RT_ERROR);
    }
    if ((uint64_t)(rt_tick_t)(rt_tick_get() - reader->start) >= reader->timeout_ticks)
        return !fail(reader, "timeout", -RT_ETIMEOUT);
    if (reader->stream)
    {
        rt_tick_t now = rt_tick_get();
        if (reader->phase == 0 && (uint64_t)(rt_tick_t)(now - reader->start) >= reader->connect_ticks)
            return !fail(reader, "connect-timeout", -RT_ETIMEOUT);
        if (reader->phase == 1 && (uint64_t)(rt_tick_t)(now - reader->progress) >= reader->send_ticks)
            return !fail(reader, "send-timeout", -RT_ETIMEOUT);
        if (reader->phase == 2)
        {
            if (!reader->valid_data && (uint64_t)(rt_tick_t)(now - reader->phase_start) >= reader->first_ticks)
                return !fail(reader, "first-data-timeout", -RT_ETIMEOUT);
            if (reader->valid_data && (uint64_t)(rt_tick_t)(now - reader->progress) >= reader->idle_ticks)
                return !fail(reader, "idle-timeout", -RT_ETIMEOUT);
        }
    }
    return false;
}

static unsigned char lower(unsigned char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + 'a' - 'A') : ch;
}

static bool equal(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { ++a; ++b; }
    return *a == *b;
}

static bool alpha(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static bool digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

static bool token(unsigned char ch)
{
    return alpha(ch) || digit(ch) || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

/* Validate the authority before DNS or writing credentials to the connection. */
static bool valid_host(const char *host)
{
    size_t length = 0, label = 0;
    bool numeric = true;
    if (!host || !*host) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
    {
        if (++length > 253) return false;
        if (*p == '.')
        {
            if (!label || p[-1] == '-') return false;
            label = 0;
        }
        else
        {
            if (!(alpha(*p) || digit(*p) || *p == '-') || (!label && *p == '-')) return false;
            if (++label > 63) return false;
            if (!digit(*p)) numeric = false;
        }
    }
    if (host[length - 1] == '-') return false;
    if (numeric)
    {
        unsigned parts = 0;
        const char *p = host;
        do
        {
            unsigned value = 0, digits = 0;
            while (digit((unsigned char)*p))
            {
                if (++digits > 3) return false;
                value = value * 10 + (unsigned)(*p++ - '0');
            }
            if (!digits || value > 255 || ++parts > 4) return false;
            if (!*p) break;
            if (*p++ != '.' || !*p) return false;
        } while (*p);
        return parts == 4;
    }
    return true;
}

static bool request_valid(http_reader_t *reader)
{
    const epd_http_request_t *request = reader->request;
    if (!valid_host(request->host) || !request->path || request->path[0] != '/' ||
        (request->method && strcmp(request->method, "GET") && strcmp(request->method, "POST")) ||
        (request->body_size && !request->body) || (request->header_count && !request->headers))
        return fail(reader, "request-invalid", -RT_EINVAL);
    for (const unsigned char *p = (const unsigned char *)request->path; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == '#') return fail(reader, "request-path", -RT_EINVAL);
    for (size_t i = 0; i < request->header_count; ++i)
    {
        if (expired(reader)) return false;
        const epd_http_header_t *header = &request->headers[i];
        if (!header->name || !*header->name || !header->value)
            return fail(reader, "request-header", -RT_EINVAL);
        for (const unsigned char *p = (const unsigned char *)header->name; *p; ++p)
            if (!token(*p)) return fail(reader, "request-header", -RT_EINVAL);
        for (const unsigned char *p = (const unsigned char *)header->value; *p; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127)
                return fail(reader, "request-header", -RT_EINVAL);
        if (equal(header->name, "Host") || equal(header->name, "Connection") ||
            equal(header->name, "Content-Length") || equal(header->name, "Transfer-Encoding") ||
            equal(header->name, "Expect"))
            return fail(reader, "request-header-reserved", -RT_EINVAL);
        if (reader->stream && equal(header->name, "Accept-Encoding") && !equal(header->value, "identity"))
            return fail(reader, "request-content-encoding", -RT_EINVAL);
    }
    return !expired(reader);
}

static bool body_reserve(http_reader_t *reader, char **body, size_t used,
                         size_t *capacity, size_t needed, bool exact)
{
    if (expired(reader)) return false;
    if (reader->stream) return true;
    if (needed > reader->limit) return fail(reader, "body-limit", -RT_EFULL);
    if (*body && needed <= *capacity) return true;
    size_t next = exact ? needed : (*capacity ? *capacity : BODY_INITIAL);
    if (next > reader->limit) next = reader->limit;
    while (next < needed) next = next > reader->limit / 2 ? reader->limit : next * 2;
    char *resized = epd_app_alloc(next + 1, EPD_APP_PSRAM);
    if (!resized) return fail(reader, "body-alloc", -RT_ENOMEM);
    for (size_t offset = 0; offset < used;)
    {
        size_t count = used - offset > IO_SLICE ? IO_SLICE : used - offset;
        if (expired(reader)) { epd_app_free(resized); return false; }
        memcpy(resized + offset, *body + offset, count);
        offset += count;
    }
    epd_app_free(*body);
    *body = resized;
    *capacity = next;
    return true;
}

static int tls_read(http_reader_t *reader, unsigned char *buffer, size_t size)
{
    if (size > IO_SLICE) size = IO_SLICE;
    while (!expired(reader))
    {
        int ret = mbedtls_ssl_read(&reader->tls->ssl, buffer, size);
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if (expired(reader)) return -1;
            if (ret > 0) reader->progress = rt_tick_get();
            reader->response->detail = ret;
            return ret;
        }
        rt_thread_mdelay(10);
    }
    return -1;
}

static bool tls_write(http_reader_t *reader, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    while (size)
    {
        if (expired(reader)) return false;
        size_t slice = size > IO_SLICE ? IO_SLICE : size;
        int ret = mbedtls_ssl_write(&reader->tls->ssl, bytes, slice);
        if (ret > 0 && (size_t)ret <= slice)
        {
            if (expired(reader)) return false;
            reader->progress = rt_tick_get();
            bytes += ret;
            size -= (size_t)ret;
        }
        else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            rt_thread_mdelay(10);
        else return fail(reader, "request-write", ret ? ret : -RT_ERROR);
    }
    return !expired(reader);
}

static bool deliver(http_reader_t *reader, size_t count, size_t *used)
{
    epd_http_stream_response_t *response = reader->stream_response;
    bool valid_data = false;
    if (count > reader->limit - *used) return fail(reader, "body-limit", -RT_EFULL);
    *used += count;
    response->body_bytes = *used;
    response->status = reader->response->status;
    response->action = reader->stream->on_body(reader->stream->user, reader->buffer, count, &valid_data);
    if (response->action == EPD_HTTP_STREAM_CANCEL) return fail(reader, "body-cancelled", -RT_ERROR);
    if (response->action == EPD_HTTP_STREAM_ERROR) return fail(reader, "body-callback", -RT_ERROR);
    if (response->action != EPD_HTTP_STREAM_CONTINUE && response->action != EPD_HTTP_STREAM_COMPLETE)
        return fail(reader, "body-action", -RT_EINVAL);
    if (expired(reader)) return false;
    if (valid_data || reader->error_status || (reader->stream->options & EPD_HTTP_STREAM_FIRST_BODY_DATA))
        reader->valid_data = response->first_data_received = true;
    if (response->action == EPD_HTTP_STREAM_COMPLETE)
    {
        response->protocol_complete = true;
        reader->stopped = true;
    }
    return true;
}

static bool diagnostic_limit(http_reader_t *reader, size_t used)
{
    if (!reader->stream || !reader->error_status || used < reader->limit) return false;
    reader->stream_response->error_body_truncated = true;
    reader->stopped = true;
    return true;
}

static bool read_exact(http_reader_t *reader, char *body, size_t size, size_t *used, bool final_segment)
{
    while (size)
    {
        if (diagnostic_limit(reader, *used)) return true;
        size_t want = size;
        if (reader->stream && reader->error_status && want > reader->limit - *used)
            want = reader->limit - *used;
        int count = tls_read(reader, reader->stream ? reader->buffer : (unsigned char *)body + *used, want);
        if (count <= 0) return false;
        if ((size_t)count > want) return fail(reader, "body-read", -RT_EINVAL);
        size -= (size_t)count;
        if (final_segment && !size) reader->http_complete = true;
        if (reader->stream)
        {
            if (!deliver(reader, (size_t)count, used)) return false;
            if (reader->stopped) return true;
        }
        else *used += (size_t)count;
    }
    return true;
}

static int read_line(http_reader_t *reader, char *line)
{
    size_t used = 0;
    while (used < LINE_SIZE)
    {
        unsigned char ch;
        if (tls_read(reader, &ch, 1) != 1) return -1;
        if (ch == '\n')
        {
            if (!used || line[used - 1] != '\r') break;
            line[--used] = '\0';
            return (int)used;
        }
        if (!ch || (ch < 32 && ch != '\r' && ch != '\t') || ch == 127 ||
            (used && line[used - 1] == '\r')) break;
        line[used++] = (char)ch;
    }
    reader->response->detail = -RT_EINVAL;
    return -1;
}

static int socket_result(int result, int retry, int failure)
{
    if (result >= 0) return result;
    int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return retry;
    if (error == ECONNRESET || error == EPIPE) return MBEDTLS_ERR_NET_CONN_RESET;
    return failure;
}

/* The SDK BIO treats every error on an O_NONBLOCK socket as retryable. */
static int socket_send(void *context, const unsigned char *buffer, size_t size)
{
    int fd = ((mbedtls_net_context *)context)->fd;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    return socket_result((int)send(fd, buffer, size, 0),
                         MBEDTLS_ERR_SSL_WANT_WRITE, MBEDTLS_ERR_NET_SEND_FAILED);
}

static int socket_recv(void *context, unsigned char *buffer, size_t size)
{
    int fd = ((mbedtls_net_context *)context)->fd;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    return socket_result((int)recv(fd, buffer, size, 0),
                         MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_NET_RECV_FAILED);
}

static bool local_route(http_reader_t *reader, uint32_t destination, bool resolved)
{
    const network_source_t sources[] = {NETWORK_WIFI, NETWORK_PAN, NETWORK_WIFI_AP};
    bool found = false;
    unsigned best_prefix = 0;
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i)
    {
        network_interface_snapshot_t state;
        if (network_interface_snapshot(sources[i], &state) != RT_EOK || !state.ready ||
            !state.address || !state.netmask) continue;
        if (resolved && (destination & state.netmask) != (state.address & state.netmask)) continue;
        unsigned prefix = 0;
        for (uint32_t mask = state.netmask; mask; mask >>= 1) prefix += mask & 1u;
        if (found && (!resolved || prefix <= best_prefix)) continue;
        reader->route.source = sources[i]; reader->route.address = state.address;
        reader->local_interface = state;
        reader->local_route = found = true; best_prefix = prefix;
    }
    return found;
}

static bool tls_connect(http_reader_t *reader)
{
    ip_addr_t ip;
    struct sockaddr_in address = {0}, local = {0};
    int ret, nonblocking = 1, socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    reader->response->stage = "dns";
    if (expired(reader)) return false;
    ret = netconn_gethostbyname(reader->request->host, &ip);
    if (expired(reader)) return false;
    if (ret != ERR_OK) return fail(reader, "dns", ret);
    address.sin_family = AF_INET;
    address.sin_port = htons(reader->port);
    address.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&ip));
    /* Local destinations use their own interface, independent of the uplink. */
    if (!local_route(reader, address.sin_addr.s_addr, true) && reader->local_route)
        return fail(reader, "network-no-route", -RT_ERROR);
    reader->response->stage = "tcp-connect";
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fail(reader, "tcp-connect", errno);
    reader->tls->server_fd.fd = fd;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) return fail(reader, "tcp-nonblock", errno);
    if (reader->stream)
    {
        /* lwIP also uses SO_SNDTIMEO to bound close retries when FIN allocation
         * fails. Nonblocking send/receive still use the request deadlines. */
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
        int close_timeout = CLOSE_TIMEOUT_MS;
#else
        struct timeval close_timeout = {0, CLOSE_TIMEOUT_MS * 1000u};
#endif
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &close_timeout, sizeof(close_timeout)) != 0)
            return fail(reader, "tcp-close-timeout", errno);
    }
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = reader->route.address;
    if (expired(reader)) return false;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) return fail(reader, "tcp-bind", errno);
    ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (ret < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
        return fail(reader, "tcp-connect", errno);
    while (ret < 0)
    {
        fd_set writefds;
        struct timeval timeout = {0, 100000};
        if (expired(reader)) return false;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        ret = select(fd + 1, RT_NULL, &writefds, RT_NULL, &timeout);
        if (expired(reader)) return false;
        if (ret < 0) return fail(reader, "tcp-select", errno);
        if (ret > 0)
        {
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0)
                return fail(reader, "tcp-connect", errno);
            if (socket_error) return fail(reader, "tcp-connect", socket_error);
            break;
        }
        ret = -1;
    }
    if (expired(reader)) return false;
    mbedtls_ssl_set_bio(&reader->tls->ssl, &reader->tls->server_fd,
                        socket_send, socket_recv, RT_NULL);
    reader->response->stage = "tls-handshake";
    while (!expired(reader))
    {
        ret = mbedtls_ssl_handshake(&reader->tls->ssl);
        reader->response->verify_flags = mbedtls_ssl_get_verify_result(&reader->tls->ssl);
        if (expired(reader)) return false;
        if (!ret)
        {
            if (reader->response->verify_flags) return fail(reader, "tls-verify", -RT_ERROR);
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return fail(reader, "tls-handshake", ret);
        rt_thread_mdelay(10);
    }
    return false;
}

static bool append_text(http_reader_t *reader, char *buffer, size_t *used, const char *text)
{
    size_t size = strlen(text);
    if (size > SIZE_MAX - 1 - *used) return fail(reader, "request-size", -RT_EFULL);
    for (size_t offset = 0; offset < size;)
    {
        if (expired(reader)) return false;
        size_t count = size - offset > IO_SLICE ? IO_SLICE : size - offset;
        if (buffer) memcpy(buffer + *used + offset, text + offset, count);
        offset += count;
    }
    *used += size;
    return true;
}

static bool request_headers(http_reader_t *reader, char *buffer, size_t *used)
{
    const epd_http_request_t *request = reader->request;
    const char *method = request->method ? request->method : "GET";
    char length[3 * sizeof(size_t) + 4];
    if (!append_text(reader, buffer, used, method) || !append_text(reader, buffer, used, " ") ||
        !append_text(reader, buffer, used, request->path) || !append_text(reader, buffer, used, " HTTP/1.1\r\nHost: ") ||
        !append_text(reader, buffer, used, request->host)) return false;
    if (reader->port != 443)
    {
        snprintf(length, sizeof(length), ":%u", (unsigned)reader->port);
        if (!append_text(reader, buffer, used, length)) return false;
    }
    if (!append_text(reader, buffer, used, "\r\nConnection: close\r\n")) return false;
    if (reader->stream && !append_text(reader, buffer, used, "Accept-Encoding: identity\r\n")) return false;
    for (size_t i = 0; i < request->header_count; ++i)
    {
        if (reader->stream && equal(request->headers[i].name, "Accept-Encoding")) continue;
        if (!append_text(reader, buffer, used, request->headers[i].name) || !append_text(reader, buffer, used, ": ") ||
            !append_text(reader, buffer, used, request->headers[i].value) || !append_text(reader, buffer, used, "\r\n")) return false;
    }
    if (request->body_size || !strcmp(method, "POST"))
    {
        snprintf(length, sizeof(length), "%zu", request->body_size);
        if (!append_text(reader, buffer, used, "Content-Length: ") || !append_text(reader, buffer, used, length) ||
            !append_text(reader, buffer, used, "\r\n")) return false;
    }
    return append_text(reader, buffer, used, "\r\n");
}

static bool send_request(http_reader_t *reader)
{
    size_t size = 0, used = 0;
    reader->response->stage = "request-build";
    if (!request_headers(reader, NULL, &size)) return false;
    char *headers = epd_app_alloc(size + 1, EPD_APP_PSRAM);
    if (!headers) return fail(reader, "request-alloc", -RT_ENOMEM);
    bool ok = request_headers(reader, headers, &used);
    if (ok)
    {
        headers[used] = '\0';
        reader->response->stage = "request-write";
        ok = tls_write(reader, headers, used);
    }
    epd_app_free(headers);
    return ok && tls_write(reader, reader->request->body, reader->request->body_size);
}

static bool number(const char *value, unsigned base, size_t *result, const char **end)
{
    size_t total = 0;
    const char *p = value;
    for (;; ++p)
    {
        unsigned ch = lower((unsigned char)*p);
        unsigned n = digit((unsigned char)ch) ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : base;
        if (n >= base) break;
        if (total > (SIZE_MAX - n) / base) return false;
        total = total * base + n;
    }
    if (p == value) return false;
    *end = p;
    *result = total;
    return true;
}

static bool split_header(char *line, char **value)
{
    char *p = line;
    while (token((unsigned char)*p)) ++p;
    if (p == line || *p != ':') return false;
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') ++p;
    *value = p;
    p += strlen(p);
    while (p > *value && (p[-1] == ' ' || p[-1] == '\t')) --p;
    *p = '\0';
    return true;
}

static bool header_callback(http_reader_t *reader, const char *name, const char *value)
{
    if (expired(reader)) return false;
    if (reader->request->on_header && !reader->request->on_header(reader->request->user, name, value))
        return fail(reader, "header-cancelled", -RT_ERROR);
    return !expired(reader);
}

static int header_line(http_reader_t *reader, char *line, size_t *total)
{
    int n = read_line(reader, line);
    if (n < 0) return -1;
    if ((size_t)n + 2 > HEADERS_LIMIT - *total)
    {
        fail(reader, "headers-limit", -RT_EFULL);
        return -1;
    }
    *total += (size_t)n + 2;
    return n;
}

static bool receive_body(http_reader_t *reader, char *line, char **body, size_t *used, bool *gzip)
{
    size_t total = 0, capacity = 0, length = 0;
    bool has_length = false, chunked = false, has_encoding = false;
    int status = 0;
    for (unsigned interim = 0;; ++interim)
    {
        reader->response->stage = "status-read";
        int n = header_line(reader, line, &total);
        if (n < 0) return false;
        if (n < 12 || (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) ||
            line[9] < '1' || line[9] > '5' || !digit((unsigned char)line[10]) ||
            !digit((unsigned char)line[11]) || (n > 12 && line[12] != ' '))
            return fail(reader, "status-read", -RT_EINVAL);
        status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
        reader->response->status = status;
        if (reader->stream_response) reader->stream_response->status = status;
        if (status == 101 || interim >= 8) return fail(reader, "http-status", -RT_EINVAL);
        has_length = chunked = has_encoding = *gzip = false;
        for (;;)
        {
            char *value;
            reader->response->stage = "headers-read";
            n = header_line(reader, line, &total);
            if (n < 0) return false;
            if (!n) break;
            if (!split_header(line, &value)) return fail(reader, "headers-read", -RT_EINVAL);
            if (equal(line, "content-length"))
            {
                size_t parsed;
                const char *end;
                if (!number(value, 10, &parsed, &end) || *end || (has_length && length != parsed))
                    return fail(reader, "headers-content-length", -RT_EINVAL);
                length = parsed;
                has_length = true;
            }
            else if (equal(line, "transfer-encoding"))
            {
                if (chunked || !equal(value, "chunked"))
                    return fail(reader, "headers-transfer-encoding", -RT_EINVAL);
                chunked = true;
            }
            else if (equal(line, "content-encoding"))
            {
                if (reader->stream && !equal(value, "identity"))
                    return fail(reader, "content-encoding", -RT_EINVAL);
                if (has_encoding || (!equal(value, "gzip") && !equal(value, "identity")))
                    return fail(reader, "headers-content-encoding", -RT_EINVAL);
                *gzip = equal(value, "gzip");
                has_encoding = true;
            }
            if (status >= 200 && !header_callback(reader, line, value)) return false;
        }
        if ((chunked && has_length) || ((status < 200 || status == 204) && (chunked || has_length)))
            return fail(reader, "headers-framing", -RT_EINVAL);
        if (status >= 200) break;
    }
    reader->error_status = status >= 300;
    if (reader->stream && reader->error_status) reader->limit = EPD_HTTP_STREAM_ERROR_BODY_LIMIT;
    if (status == 204 || status == 304)
    {
        *gzip = false;
        reader->http_complete = true;
        return body_reserve(reader, body, 0, &capacity, 0, true);
    }
    if (chunked)
    {
        for (;;)
        {
            size_t chunk;
            const char *end;
            reader->response->stage = "chunk-size";
            int n = read_line(reader, line);
            if (n < 0) return false;
            if (!number(line, 16, &chunk, &end) || (*end && *end != ';'))
                return fail(reader, "chunk-size", -RT_EINVAL);
            if (!chunk) break;
            if (diagnostic_limit(reader, *used)) return true;
            if (!(reader->stream && reader->error_status) && chunk > reader->limit - *used)
                return fail(reader, "body-limit", -RT_EFULL);
            if (!reader->stream && !body_reserve(reader, body, *used, &capacity, *used + chunk, false)) return false;
            reader->response->stage = "chunk-read";
            if (!read_exact(reader, *body, chunk, used, false)) return false;
            if (reader->stopped) return true;
            reader->response->stage = "chunk-terminator";
            n = read_line(reader, line);
            if (n < 0) return false;
            if (n) return fail(reader, "chunk-terminator", -RT_EINVAL);
        }
        for (;;)
        {
            char *value;
            reader->response->stage = "trailers-read";
            int n = header_line(reader, line, &total);
            if (n < 0) return false;
            if (!n) break;
            if (!split_header(line, &value) || equal(line, "content-length") ||
                equal(line, "transfer-encoding") || equal(line, "content-encoding"))
                return fail(reader, "trailers-read", -RT_EINVAL);
            if (!header_callback(reader, line, value)) return false;
        }
    }
    else if (has_length)
    {
        if (!body_reserve(reader, body, 0, &capacity, length, true)) return false;
        if (reader->stream && !reader->error_status && length > reader->limit)
            return fail(reader, "body-limit", -RT_EFULL);
        reader->response->stage = "body-read";
        if (!read_exact(reader, *body, length, used, true)) return false;
        reader->http_complete = *used == length;
        if (reader->stopped) return true;
    }
    else
    {
        for (;;)
        {
            if (diagnostic_limit(reader, *used)) return true;
            reader->response->stage = "body-eof";
            size_t want = IO_SLICE;
            if (reader->stream && reader->error_status && want > reader->limit - *used)
                want = reader->limit - *used;
            int count = tls_read(reader, reader->buffer, want);
            /* Only authenticated TLS close_notify completes an EOF body. A
             * bare TCP FIN cannot distinguish a complete body from truncation. */
            if (count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (count <= 0) return false;
            if ((size_t)count > want) return fail(reader, "body-read", -RT_EINVAL);
            if (reader->stream)
            {
                if (!deliver(reader, (size_t)count, used)) return false;
                if (reader->stopped) return true;
                continue;
            }
            if ((size_t)count > reader->limit - *used) return fail(reader, "body-limit", -RT_EFULL);
            if (!body_reserve(reader, body, *used, &capacity, *used + (size_t)count, false)) return false;
            memcpy(*body + *used, reader->buffer, (size_t)count);
            *used += (size_t)count;
        }
    }
    reader->http_complete = true;
    return body_reserve(reader, body, *used, &capacity, *used, true);
}

static uint64_t deadline_ticks(uint32_t milliseconds, uint32_t fallback)
{
    return ((uint64_t)(milliseconds ? milliseconds : fallback) * RT_TICK_PER_SECOND + 999u) / 1000u;
}

bool epd_http_core_perform(http_reader_t *reader, char **body, size_t *used, bool *gzip)
{
    const epd_http_request_t *request = reader->request;
    const epd_http_stream_request_t *stream = reader->stream;
    char *line = NULL;
    bool complete = false;
    if (!request) return fail(reader, "request-invalid", -RT_EINVAL);
    reader->start = reader->progress = reader->phase_start = rt_tick_get();
    reader->port = stream && stream->port ? stream->port : 443;
    reader->timeout_ticks = deadline_ticks(stream ? stream->total_timeout_ms : request->timeout_ms,
                                            stream ? 300000u : 30000u);
    if (stream)
    {
        reader->connect_ticks = deadline_ticks(stream->connect_timeout_ms, 30000u);
        reader->send_ticks = deadline_ticks(stream->send_idle_timeout_ms, 30000u);
        reader->first_ticks = deadline_ticks(stream->first_data_timeout_ms, 90000u);
        reader->idle_ticks = deadline_ticks(stream->idle_timeout_ms, 60000u);
        if (stream->total_timeout_ms > 900000u || reader->connect_ticks > (rt_tick_t)-1 ||
            reader->send_ticks > (rt_tick_t)-1 || reader->first_ticks > (rt_tick_t)-1 ||
            reader->idle_ticks > (rt_tick_t)-1)
            return fail(reader, "request-timeout", -RT_EINVAL);
    }
    /* Unsigned subtraction remains valid through one monotonic tick wrap. */
    if (reader->timeout_ticks > (rt_tick_t)-1) return fail(reader, "request-timeout", -RT_EINVAL);
    reader->limit = request->body_limit && request->body_limit < SIZE_MAX ? request->body_limit : SIZE_MAX - 1;
    network_snapshot(&reader->route);
    if (!reader->route.ready) local_route(reader, 0, false);
    if (expired(reader) || !request_valid(reader)) return false;
    line = epd_app_alloc(LINE_SIZE, EPD_APP_PSRAM);
    if (!line) return fail(reader, "headers-alloc", -RT_ENOMEM);
    reader->buffer = epd_app_alloc(IO_SLICE, EPD_APP_PSRAM);
    if (!reader->buffer) { fail(reader, "receive-alloc", -RT_ENOMEM); goto finish; }
    reader->tls = stream ? epd_app_alloc(sizeof(*reader->tls), EPD_APP_PSRAM) : rt_calloc(1, sizeof(*reader->tls));
    if (!reader->tls) { fail(reader, "tls-session-alloc", -RT_ENOMEM); goto finish; }
    if (stream) memset(reader->tls, 0, sizeof(*reader->tls));
    reader->response->stage = "tls-init";
    reader->response->detail = mbedtls_client_init(reader->tls, "epd-http", 8);
    if (reader->response->detail || expired(reader)) goto finish;
    if (stream)
    {
        size_t length = strlen(request->host) + 1;
        reader->tls->host = epd_app_alloc(length, EPD_APP_PSRAM);
        if (reader->tls->host) memcpy(reader->tls->host, request->host, length);
    }
    else reader->tls->host = rt_strdup(request->host);
    if (!reader->tls->host) { fail(reader, "tls-host-alloc", -RT_ENOMEM); goto finish; }
    reader->response->stage = "tls-context";
    reader->response->detail = mbedtls_client_context(reader->tls);
    if (reader->response->detail || expired(reader)) goto finish;
    if (reader->tls->conf.authmode != MBEDTLS_SSL_VERIFY_REQUIRED)
    { fail(reader, "tls-authmode", -RT_ERROR); goto finish; }
    if (!tls_connect(reader)) goto finish;
    reader->phase = 1;
    reader->progress = rt_tick_get();
    if (!send_request(reader)) goto finish;
    reader->phase = 2;
    reader->phase_start = reader->progress = rt_tick_get();
    if (stream && stream->on_sent)
    {
        stream->on_sent(stream->user);
        if (expired(reader)) goto finish;
    }
    complete = receive_body(reader, line, body, used, gzip) && !expired(reader);
finish:
    /* Once reception ends, only cancellation, route and total time apply. */
    if (stream) reader->phase = 3;
    epd_app_free(line);
    epd_app_free(reader->buffer);
    reader->buffer = NULL;
    /* SDK tls_free is rt_free; MEMHEAP_AS_HEAP finds the owning PSRAM heap.
     * Release record buffers here before the legacy wrapper allocates gzip. */
    if (reader->tls) mbedtls_client_close(reader->tls);
    reader->tls = NULL;
    if (stream && complete) complete = !expired(reader);
    if (!complete && !reader->response->detail) reader->response->detail = -RT_ERROR;
    return complete;
}

bool epd_http_stream_perform(const epd_http_stream_request_t *request,
                             epd_http_stream_response_t *response)
{
    epd_http_request_t base = {0};
    epd_http_response_t diagnostics = {0};
    http_reader_t reader = {0};
    char *body = NULL;
    size_t used = 0;
    bool gzip = false, complete = false;
    uint32_t response_size;
    if (!response || response->struct_size < sizeof(*response) || response->version != EPD_HTTP_STREAM_VERSION)
        return false;
    response_size = response->struct_size;
    memset(response, 0, sizeof(*response));
    response->struct_size = response_size;
    response->version = EPD_HTTP_STREAM_VERSION;
    reader.response = &diagnostics;
    if (!request || request->struct_size < sizeof(*request) || request->version != EPD_HTTP_STREAM_VERSION ||
        (request->options & ~EPD_HTTP_STREAM_FIRST_BODY_DATA) || !request->on_body)
    {
        fail(&reader, "request-version-options", -RT_EINVAL);
        goto finish;
    }
    base.host = request->host;
    base.path = request->path;
    base.method = request->method;
    base.headers = request->headers;
    base.header_count = request->header_count;
    base.body = request->body;
    base.body_size = request->body_size;
    base.body_limit = request->body_limit;
    base.cancelled = request->cancelled;
    base.on_header = request->on_header;
    base.user = request->user;
    reader.request = &base;
    reader.stream = request;
    reader.stream_response = response;
    complete = epd_http_core_perform(&reader, &body, &used, &gzip);
finish:
    response->status = diagnostics.status;
    response->verify_flags = diagnostics.verify_flags;
    response->http_complete = reader.http_complete;
    response->stage = complete ? (response->error_body_truncated ? "http-error-body-limit" :
        response->protocol_complete ? "protocol-complete" : "complete") : diagnostics.stage;
    response->detail = complete ? 0 : diagnostics.detail;
    return complete;
}
