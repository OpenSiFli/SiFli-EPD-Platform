/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_wire.h"
#include <stdio.h>
#include <string.h>

struct sp_ws {
    sp_transport *transport;
    speech_failure_t *failure;
    unsigned char input[1024], header[10], control[125], reply[131];
    size_t available, cursor, hused, hneed, control_used;
    size_t reply_size, reply_sent;
    uint64_t remaining, message_bytes;
    unsigned opcode, message_opcode;
    bool fin, first, frame, upgraded, closed, close_sent, close_received;
    sp_utf8 utf;
};
static bool fail(sp_ws *w, const char *stage)
{
    sp_fail(w->failure, EPD_SPEECH_PROTOCOL, stage, 0); return false;
}
sp_ws *sp_ws_new(sp_transport *transport, speech_failure_t *failure)
{
    sp_ws *w = speech_alloc(sizeof(*w));
    if (!w) { sp_fail(failure, EPD_SPEECH_NO_MEMORY, "ws-alloc", 0); return NULL; }
    memset(w, 0, sizeof(*w)); w->transport = transport; w->failure = failure;
    w->hneed = 2; return w;
}
static int read_some(sp_ws *w, void *out, size_t n)
{
    if (w->cursor == w->available) {
        int r = sp_transport_read(w->transport, w->input, sizeof(w->input));
        if (r <= 0) return r;
        w->available = (size_t)r; w->cursor = 0;
    }
    if (n > w->available - w->cursor) n = w->available - w->cursor;
    memcpy(out, w->input + w->cursor, n); w->cursor += n;
    return (int)n;
}
static bool write_all(sp_ws *w, const void *data, size_t n, sp_guard *g)
{
    const unsigned char *p = data;
    while (n && sp_check(g) && sp_transport_route(w->transport, g)) {
        int r = sp_transport_write(w->transport, p, n);
        if (r == SP_IO_AGAIN) { sp_delay(10); continue; }
        if (r <= 0 || (size_t)r > n) {
            sp_fail(w->failure, EPD_SPEECH_CONNECT, "ws-write", r); return false;
        }
        p += r; n -= (size_t)r;
    }
    return n == 0 && sp_check(g);
}
/* Control replies retain their masked bytes across nonblocking TLS retries. */
static bool queue_reply(sp_ws *w, unsigned op, const unsigned char *p, size_t n)
{
    unsigned char mask[4];
    if (w->reply_size || n > 125) return fail(w, "ws-control-queue");
    if (!sp_transport_random(w->transport, mask, sizeof(mask))) return fail(w, "ws-mask-random");
    w->reply[0] = (unsigned char)(128u | op); w->reply[1] = (unsigned char)(128u | n);
    memcpy(w->reply + 2, mask, 4);
    for (size_t i = 0; i < n; ++i) w->reply[6 + i] = p[i] ^ mask[i & 3];
    w->reply_size = n + 6; w->reply_sent = 0;
    if (op == 8) w->close_sent = true;
    return true;
}
static int flush_reply(sp_ws *w)
{
    if (w->reply_sent < w->reply_size) {
        size_t n = w->reply_size - w->reply_sent;
        int r = sp_transport_write(w->transport, w->reply + w->reply_sent, n);
        if (r == SP_IO_AGAIN) return 0;
        if (r <= 0 || (size_t)r > n) {
            sp_fail(w->failure, EPD_SPEECH_CONNECT, "ws-control-write", r); return -1;
        }
        w->reply_sent += (size_t)r;
        if (w->reply_sent < w->reply_size) return 0;
    }
    w->reply_sent = w->reply_size = 0;
    return 1;
}
static bool equal_case(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return !*a && !*b;
}
static bool token(const char *p, const char *wanted)
{
    char word[64];
    while (*p) {
        size_t n = 0;
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        while (*p && *p != ',') {
            if (n + 1 >= sizeof(word)) return false;
            word[n++] = *p++;
        }
        while (n && (word[n - 1] == ' ' || word[n - 1] == '\t')) --n;
        word[n] = 0;
        if (equal_case(word, wanted)) return true;
    }
    return false;
}
bool sp_ws_upgrade(sp_ws *w, const char *host, const char *path,
                   const char *headers, sp_guard *g)
{
    unsigned char nonce[16], digest[20];
    char key[32], accept[32], joined[80];
    char *buffer = speech_alloc(8192);
    bool ok = false, upgrade = false, connection = false, accepted = false;
    size_t used = 0;
    int count;
    if (!buffer) { sp_fail(w->failure, EPD_SPEECH_NO_MEMORY, "upgrade-alloc", 0); return false; }
    if (!sp_transport_random(w->transport, nonce, sizeof(nonce)) ||
        !sp_base64(nonce, sizeof(nonce), key, sizeof(key))) { fail(w, "ws-random"); goto end; }
    count = snprintf(joined, sizeof(joined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    if (count < 0 || !sp_sha1(joined, (size_t)count, digest) ||
        !sp_base64(digest, sizeof(digest), accept, sizeof(accept))) { fail(w, "ws-sha1"); goto end; }
    count = snprintf(buffer, 8192,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n", path, host, key, headers);
    if (count < 0 || count >= 8192) { fail(w, "upgrade-request-size"); goto end; }
    if (!write_all(w, buffer, (size_t)count, g)) goto end;
    sp_wipe(buffer, 8192);
    while (sp_check(g) && sp_transport_route(w->transport, g)) {
        int r = read_some(w, buffer + used, 1);
        if (r == SP_IO_AGAIN) { sp_delay(10); continue; }
        if (r <= 0) { sp_fail(w->failure, EPD_SPEECH_CONNECT, "upgrade-read", r); goto end; }
        if (!buffer[used] || ((unsigned char)buffer[used] < 32 && buffer[used] != '\r' &&
                             buffer[used] != '\n' && buffer[used] != '\t')) { fail(w, "upgrade-control"); goto end; }
        ++used;
        if (used >= 8191) { fail(w, "upgrade-header-size"); goto end; }
        if (used >= 4 && !memcmp(buffer + used - 4, "\r\n\r\n", 4)) break;
    }
    if (!sp_check(g)) goto end;
    buffer[used] = 0;
    if (used < 12 || memcmp(buffer, "HTTP/1.1 ", 9) ||
        buffer[9] < '1' || buffer[9] > '5' || buffer[10] < '0' || buffer[10] > '9' ||
        buffer[11] < '0' || buffer[11] > '9' || (buffer[12] != ' ' && buffer[12] != '\r')) {
        fail(w, "upgrade-status"); goto end;
    }
    w->failure->http_status = (buffer[9] - '0') * 100 + (buffer[10] - '0') * 10 + buffer[11] - '0';
    char *line = strstr(buffer, "\r\n");
    if (!line) { fail(w, "upgrade-lines"); goto end; }
    line += 2;
    while (*line != '\r') {
        char *next = strstr(line, "\r\n"), *colon = strchr(line, ':');
        if (!next || !colon || colon >= next || colon == line || *line == ' ' || *line == '\t') {
            fail(w, "upgrade-header"); goto end;
        }
        *next = 0; *colon++ = 0;
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *tail = next;
        while (tail > colon && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (equal_case(line, "Upgrade")) {
            if (upgrade || !equal_case(colon, "websocket")) { fail(w, "upgrade-token"); goto end; }
            upgrade = true;
        } else if (equal_case(line, "Connection")) connection = connection || token(colon, "upgrade");
        else if (equal_case(line, "Sec-WebSocket-Accept")) {
            if (accepted || strcmp(colon, accept)) { fail(w, "upgrade-accept"); goto end; }
            accepted = true;
        } else if (equal_case(line, "Sec-WebSocket-Extensions") || equal_case(line, "Sec-WebSocket-Protocol")) {
            fail(w, "upgrade-extension"); goto end;
        } else if (equal_case(line, "X-Tt-Logid") || equal_case(line, "X-Request-Id"))
            sp_safe_id(w->failure->request_id, sizeof(w->failure->request_id), colon, strlen(colon));
        line = next + 2;
    }
    if (w->failure->http_status != 101) {
        int status = w->failure->http_status;
        sp_fail(w->failure, status == 401 || status == 403 ? EPD_SPEECH_AUTH :
                status == 402 ? EPD_SPEECH_QUOTA : status == 429 ? EPD_SPEECH_BUSY :
                EPD_SPEECH_CONNECT, "upgrade-http", status); goto end;
    }
    if (!upgrade || !connection || !accepted) { fail(w, "upgrade-missing"); goto end; }
    ok = w->upgraded = true;
end:
    sp_wipe(buffer, 8192); speech_free(buffer); return ok;
}
bool sp_ws_send(sp_ws *w, unsigned opcode, const void *p, size_t n, sp_guard *g)
{
    unsigned char header[14], mask[4], chunk[1024];
    size_t h = 2, offset = 0;
    if (w->reply_size) {
        if (!write_all(w, w->reply + w->reply_sent, w->reply_size - w->reply_sent, g)) return false;
        w->reply_sent = w->reply_size = 0;
    }
    if (w->close_received) w->closed = true;
    if (w->closed || (w->close_sent && opcode != 8)) return fail(w, "ws-send-closed");
    if ((!p && n) || n > SP_MESSAGE_MAX || (opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) ||
        (opcode >= 8 && n > 125)) return fail(w, "ws-send-size");
    if (!sp_check(g)) return false;
    if (!sp_transport_random(w->transport, mask, sizeof(mask))) return fail(w, "ws-mask-random");
    header[0] = (unsigned char)(0x80u | opcode);
    if (n < 126) header[1] = (unsigned char)(0x80u | n);
    else if (n <= 65535) {
        header[1] = 0xfe; header[h++] = (unsigned char)(n >> 8); header[h++] = (unsigned char)n;
    } else {
        header[1] = 0xff;
        for (int i = 7; i >= 0; --i) header[h++] = (unsigned char)((uint64_t)n >> (i * 8));
    }
    memcpy(header + h, mask, 4); h += 4;
    if (!write_all(w, header, h, g)) return false;
    while (offset < n) {
        size_t size = n - offset;
        if (size > sizeof(chunk)) size = sizeof(chunk);
        for (size_t i = 0; i < size; ++i) chunk[i] = ((const unsigned char *)p)[offset + i] ^ mask[(offset + i) & 3];
        if (!write_all(w, chunk, size, g)) { sp_wipe(chunk, sizeof(chunk)); return false; }
        offset += size;
    }
    sp_wipe(chunk, sizeof(chunk));
    if (opcode == 8) w->close_sent = true;
    return true;
}
static bool frame_header(sp_ws *w)
{
    unsigned length = w->header[1] & 127u;
    w->opcode = w->header[0] & 15u; w->fin = (w->header[0] & 128u) != 0;
    if ((w->header[0] & 0x70u) || (w->header[1] & 128u)) return fail(w, "ws-rsv-mask");
    if (w->opcode != 0 && w->opcode != 1 && w->opcode != 2 && w->opcode != 8 && w->opcode != 9 && w->opcode != 10)
        return fail(w, "ws-opcode");
    if (w->opcode >= 8 && (!w->fin || length > 125)) return fail(w, "ws-control-size");
    w->remaining = length;
    if (length >= 126) {
        w->remaining = 0;
        for (size_t i = 2; i < w->hneed; ++i) w->remaining = (w->remaining << 8) | w->header[i];
        if ((length == 126 && w->remaining < 126) || (length == 127 &&
            (w->remaining <= 65535 || (w->header[2] & 128u)))) return fail(w, "ws-length-encoding");
    }
    if (w->opcode < 8) {
        if (w->opcode == 0) { if (!w->message_opcode) return fail(w, "ws-orphan-continuation"); }
        else {
            if (w->message_opcode) return fail(w, "ws-nested-message");
            w->message_opcode = w->opcode; w->message_bytes = 0; w->first = true;
            memset(&w->utf, 0, sizeof(w->utf));
        }
        uint64_t maximum = w->message_opcode == 1 ? SP_JSON_MAX : SP_MESSAGE_MAX;
        if (w->remaining > maximum - w->message_bytes) return fail(w, "ws-message-size");
        w->message_bytes += w->remaining;
    }
    w->control_used = 0; w->frame = true; return true;
}
static bool close_code(unsigned code)
{
    return (code >= 3000 && code <= 4999) ||
        (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006);
}
int sp_ws_poll(sp_ws *w, uint32_t wait_ms, sp_guard *g, sp_ws_data_fn fn, void *user)
{
    uint32_t start = speech_now_ms();
    unsigned slices = 0;
    unsigned char chunk[1024];
    if (w->closed) return -1;
    for (;;) {
        if (!sp_check(g) || !sp_transport_route(w->transport, g)) return -1;
        uint32_t elapsed = speech_now_ms() - start;
        if (slices && elapsed && elapsed >= wait_ms) return 0;
        if (slices++ >= 128) return 0;
        if (w->reply_size) {
            int sent = flush_reply(w);
            if (sent < 0) return -1;
            if (!sent) {
                if (speech_now_ms() - start >= wait_ms) return 0;
                uint32_t left = wait_ms - (speech_now_ms() - start);
                sp_delay(left < 10 ? left : 10);
                continue;
            }
            if (w->close_received) { w->closed = true; return -1; }
            return 1;
        }
        int r;
        if (!w->frame) {
            r = read_some(w, w->header + w->hused, w->hneed - w->hused);
            if (r > 0) {
                w->hused += (size_t)r;
                if (w->hused < w->hneed) continue;
                if (w->hneed == 2) {
                    unsigned n = w->header[1] & 127u;
                    w->hneed = n == 127 ? 10 : n == 126 ? 4 : 2;
                    if (w->hused < w->hneed) continue;
                }
                if (!frame_header(w)) return -1;
                continue;
            }
        } else if (w->remaining) {
            size_t want = w->remaining < sizeof(chunk) ? (size_t)w->remaining : sizeof(chunk);
            r = read_some(w, chunk, want);
            if (r > 0) {
                w->remaining -= (size_t)r;
                if (w->opcode >= 8) {
                    memcpy(w->control + w->control_used, chunk, (size_t)r); w->control_used += (size_t)r;
                } else {
                    bool end = !w->remaining && w->fin;
                    if (w->message_opcode == 1 && (!sp_utf8_feed(&w->utf, chunk, (size_t)r) || (end && w->utf.need))) {
                        fail(w, "ws-utf8"); return -1;
                    }
                    if (fn && !fn(user, w->message_opcode, chunk, (size_t)r, w->first, end)) {
                        sp_fail(w->failure, EPD_SPEECH_CANCELLED, "ws-consumer", 0); return -1;
                    }
                    w->first = false;
                }
                if (w->remaining) continue;
                r = 1;
            }
        } else r = 1;
        if (w->frame && !w->remaining && r > 0) {
            unsigned op = w->opcode;
            w->frame = false; w->hneed = 2; w->hused = 0;
            if (op >= 8) {
                if (op == 9) {
                    if (!queue_reply(w, 10, w->control, w->control_used)) return -1;
                    continue;
                }
                if (op == 8) {
                    size_t n = w->control_used;
                    if (n == 1 || (n >= 2 && (!close_code((w->control[0] << 8) | w->control[1]) ||
                        !sp_utf8_text((const char *)w->control + 2, n - 2, NULL)))) {
                        fail(w, "ws-close-payload"); return -1;
                    }
                    w->close_received = true;
                    if (!w->close_sent) {
                        if (!queue_reply(w, 8, w->control, n)) return -1;
                        continue;
                    }
                    w->closed = true; return -1;
                }
                return 1;
            }
            if (w->fin) {
                /* Empty final continuation still terminates the message. */
                if ((w->header[1] & 127u) == 0) {
                    if (w->message_opcode == 1 && w->utf.need) { fail(w, "ws-utf8"); return -1; }
                    if (fn && !fn(user, w->message_opcode, NULL, 0, w->first, true)) {
                        sp_fail(w->failure, EPD_SPEECH_CANCELLED, "ws-consumer", 0); return -1;
                    }
                }
                w->message_opcode = 0; return 1;
            }
            continue;
        }
        if (r != SP_IO_AGAIN) {
            sp_fail(w->failure, EPD_SPEECH_CONNECT, "ws-eof", r); return -1;
        }
        if (speech_now_ms() - start >= wait_ms) return 0;
        uint32_t left = wait_ms - (speech_now_ms() - start);
        sp_delay(left < 10 ? left : 10);
    }
}
void sp_ws_close(sp_ws *w, sp_guard *g)
{
    const unsigned char normal[2] = {3, 232};
    if (!w || !w->upgraded || w->closed || !sp_check(g)) return;
    if (!w->close_sent && !sp_ws_send(w, 8, normal, sizeof(normal), g)) return;
    while (!w->closed && sp_check(g))
        if (sp_ws_poll(w, 20, g, NULL, NULL) < 0) break;
}
void sp_ws_free(sp_ws *w)
{
    if (w) { sp_wipe(w, sizeof(*w)); speech_free(w); }
}
