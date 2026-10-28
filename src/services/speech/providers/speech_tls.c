/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_wire.h"
#include "mem_section.h"
#include "speech_roots.h"
#include "network.h"
#include "epd_tls_config.h"
#include <tls_client.h>
#include <lwip/sockets.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <errno.h>
#include <string.h>

struct sp_transport {
    MbedTLSSession tls;
    network_snapshot_t route;
    speech_failure_t *failure;
};
/* Requests outlive a cancelled caller until lwIP's callback retires them.
 * A fixed pool bounds this detached state even during repeated DNS failures. */
typedef struct {
    bool occupied, abandoned, done;
    int result;
    ip_addr_t address;
    char host[256];
} sp_dns;
L2_RET_BSS_SECT_BEGIN(speech_dns)
static sp_dns dns_slots[4] L2_RET_BSS_SECT(speech_dns);
L2_RET_BSS_SECT_END

/* The retained section is not in the startup zero table. No DNS request
 * survives a reboot, while sleep/resume must preserve outstanding requests. */
static int speech_dns_slots_init(void)
{
    memset(dns_slots, 0, sizeof(dns_slots));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(speech_dns_slots_init);

static void dns_complete(const char *name, const ip_addr_t *address, void *user)
{
    sp_dns *d = user;
    rt_base_t level = rt_hw_interrupt_disable();
    (void)name;
    if (d->abandoned) memset(d, 0, sizeof(*d));
    else {
        if (address) d->address = *address;
        d->result = address ? ERR_OK : ERR_VAL;
        d->done = true;
    }
    rt_hw_interrupt_enable(level);
}
static void dns_begin(void *user)
{
    sp_dns *d = user;
    ip_addr_t address;
    rt_base_t level = rt_hw_interrupt_disable();
    bool abandoned = d->abandoned;
    if (abandoned) memset(d, 0, sizeof(*d));
    rt_hw_interrupt_enable(level);
    if (abandoned) return;
    err_t r = dns_gethostbyname_addrtype(d->host, &address, dns_complete, d, LWIP_DNS_ADDRTYPE_IPV4);
    if (r != ERR_INPROGRESS) dns_complete(d->host, r == ERR_OK ? &address : NULL, d);
}
static bool resolve(sp_transport *t, const char *host, ip_addr_t *address, sp_guard *g)
{
    sp_dns *d = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < sizeof(dns_slots) / sizeof(dns_slots[0]); ++i) {
        if (!dns_slots[i].occupied) {
            d = &dns_slots[i]; memset(d, 0, sizeof(*d));
            d->occupied = true; memcpy(d->host, host, strlen(host) + 1); break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!d) { sp_fail(g->failure, EPD_SPEECH_BUSY, "dns-capacity", 0); return false; }
    err_t r = tcpip_callback_with_block(dns_begin, d, 0);
    if (r != ERR_OK) {
        level = rt_hw_interrupt_disable(); memset(d, 0, sizeof(*d)); rt_hw_interrupt_enable(level);
        sp_fail(g->failure, EPD_SPEECH_CONNECT, "dns-dispatch", r); return false;
    }
    while (sp_check(g) && sp_transport_route(t, g)) {
        level = rt_hw_interrupt_disable();
        bool done = d->done;
        if (done) { *address = d->address; r = (err_t)d->result; memset(d, 0, sizeof(*d)); }
        rt_hw_interrupt_enable(level);
        if (done) {
            if (r != ERR_OK) sp_fail(g->failure, EPD_SPEECH_CONNECT, "dns-result", r);
            return r == ERR_OK;
        }
        sp_delay(10);
    }
    level = rt_hw_interrupt_disable();
    if (d->done) memset(d, 0, sizeof(*d));
    else d->abandoned = true;
    rt_hw_interrupt_enable(level);
    return false;
}
void sp_delay(uint32_t ms) { rt_thread_mdelay((rt_int32_t)ms); }
uint64_t sp_utc(void) { time_t now = epd_tls_time(NULL); return now > 0 ? (uint64_t)now : 0; }
static int socket_result(int r, int retry, int failure)
{
    if (r >= 0) return r;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) return retry;
    return failure;
}
static int socket_send(void *context, const unsigned char *data, size_t n)
{
    int fd = ((mbedtls_net_context *)context)->fd;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    return socket_result((int)send(fd, data, n, 0), MBEDTLS_ERR_SSL_WANT_WRITE, MBEDTLS_ERR_NET_SEND_FAILED);
}
static int socket_recv(void *context, unsigned char *data, size_t n)
{
    int fd = ((mbedtls_net_context *)context)->fd;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    return socket_result((int)recv(fd, data, n, 0), MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_NET_RECV_FAILED);
}
bool sp_transport_new(sp_transport **out, sp_guard *g)
{
    *out = NULL;
    if (!sp_check(g)) return false;
    sp_transport *t = speech_alloc(sizeof(*t));
    if (!t) { sp_fail(g->failure, EPD_SPEECH_NO_MEMORY, "tls-alloc", 0); return false; }
    memset(t, 0, sizeof(*t)); t->failure = g->failure;
    network_snapshot(&t->route);
    int r = mbedtls_client_init(&t->tls, "epd-speech", 10);
    *out = t;
    if (r) { sp_fail(g->failure, EPD_SPEECH_TLS, "tls-seed", r); return false; }
    return sp_transport_route(t, g);
}
bool sp_transport_route(sp_transport *t, sp_guard *g)
{
    network_snapshot_t now;
    network_snapshot(&now);
    if (!now.ready || now.generation != t->route.generation || now.address != t->route.address ||
        now.source != t->route.source) {
        sp_fail(g->failure, EPD_SPEECH_NO_NETWORK, "network-route", 0); return false;
    }
    return true;
}
bool sp_transport_random(sp_transport *t, void *data, size_t n)
{
    int r = mbedtls_ctr_drbg_random(&t->tls.ctr_drbg, data, n);
    if (r) sp_fail(t->failure, EPD_SPEECH_TLS, "tls-random", r);
    return !r;
}
bool sp_transport_connect(sp_transport *t, const char *host, sp_guard *g)
{
    ip_addr_t ip;
    struct sockaddr_in remote = {0}, local = {0};
    int nonblock = 1, r, socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (!sp_utc()) { sp_fail(g->failure, EPD_SPEECH_TLS, "clock-invalid", 0); return false; }
    if (!host || strlen(host) >= 256) { sp_fail(g->failure, EPD_SPEECH_INVALID, "tls-host", 0); return false; }
    t->tls.host = speech_alloc(strlen(host) + 1);
    if (!t->tls.host) { sp_fail(g->failure, EPD_SPEECH_NO_MEMORY, "tls-host", 0); return false; }
    strcpy(t->tls.host, host);
    r = mbedtls_client_context(&t->tls);
    if (r) { sp_fail(g->failure, EPD_SPEECH_TLS, "tls-context", r); return false; }
    r = mbedtls_x509_crt_parse(&t->tls.cacert, (const unsigned char *)sp_speech_roots, sizeof(sp_speech_roots));
    if (r) { sp_fail(g->failure, EPD_SPEECH_TLS, "tls-speech-roots", r); return false; }
    mbedtls_ssl_conf_dbg(&t->tls.conf, NULL, NULL);
    mbedtls_ssl_conf_min_version(&t->tls.conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    if (t->tls.conf.authmode != MBEDTLS_SSL_VERIFY_REQUIRED || !t->tls.cacert.raw.p) {
        sp_fail(g->failure, EPD_SPEECH_TLS, "tls-trust", 0); return false;
    }
    /* Explicitly set both SNI and the certificate-name verification target. */
    r = mbedtls_ssl_set_hostname(&t->tls.ssl, host);
    if (r) { sp_fail(g->failure, EPD_SPEECH_TLS, "tls-hostname", r); return false; }
    if (!resolve(t, host, &ip, g)) return false;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { sp_fail(g->failure, EPD_SPEECH_CONNECT, "socket", errno); return false; }
    t->tls.server_fd.fd = fd;
    if (ioctlsocket(fd, FIONBIO, &nonblock)) { sp_fail(g->failure, EPD_SPEECH_CONNECT, "socket-nonblock", errno); return false; }
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    int close_timeout = 50;
#else
    struct timeval close_timeout = {0, 50000};
#endif
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &close_timeout, sizeof(close_timeout))) {
        sp_fail(g->failure, EPD_SPEECH_CONNECT, "socket-close-timeout", errno); return false;
    }
    local.sin_family = AF_INET; local.sin_addr.s_addr = t->route.address;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local))) {
        sp_fail(g->failure, EPD_SPEECH_CONNECT, "socket-bind", errno); return false;
    }
    remote.sin_family = AF_INET; remote.sin_port = htons(443);
    remote.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&ip));
    r = connect(fd, (struct sockaddr *)&remote, sizeof(remote));
    if (r && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        sp_fail(g->failure, EPD_SPEECH_CONNECT, "tcp-connect", errno); return false;
    }
    while (r) {
        fd_set writefds;
        struct timeval timeout = {0, 20000};
        if (!sp_check(g) || !sp_transport_route(t, g)) return false;
        FD_ZERO(&writefds); FD_SET(fd, &writefds);
        int selected = select(fd + 1, NULL, &writefds, NULL, &timeout);
        if (selected < 0 && errno == EINTR) continue;
        if (selected < 0) { sp_fail(g->failure, EPD_SPEECH_CONNECT, "tcp-select", errno); return false; }
        if (!selected) continue;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) || socket_error) {
            sp_fail(g->failure, EPD_SPEECH_CONNECT, "tcp-connect", socket_error ? socket_error : errno); return false;
        }
        break;
    }
    mbedtls_ssl_set_bio(&t->tls.ssl, &t->tls.server_fd, socket_send, socket_recv, NULL);
    while (sp_check(g) && sp_transport_route(t, g)) {
        r = mbedtls_ssl_handshake(&t->tls.ssl);
        g->failure->tls_verify_flags = mbedtls_ssl_get_verify_result(&t->tls.ssl);
        if (!r) {
            if (g->failure->tls_verify_flags) { sp_fail(g->failure, EPD_SPEECH_TLS, "tls-verify", 0); return false; }
            return sp_check(g);
        }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            sp_fail(g->failure, EPD_SPEECH_TLS, g->failure->tls_verify_flags ? "tls-verify" : "tls-handshake", r);
            return false;
        }
        sp_delay(10);
    }
    return false;
}
static int io_result(sp_transport *t, int r, const char *stage)
{
    if (r >= 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return SP_IO_AGAIN;
    sp_fail(t->failure, EPD_SPEECH_CONNECT, stage, r); return SP_IO_ERROR;
}
int sp_transport_read(sp_transport *t, void *data, size_t n)
{ return io_result(t, mbedtls_ssl_read(&t->tls.ssl, data, n), "tls-read"); }
int sp_transport_write(sp_transport *t, const void *data, size_t n)
{ return io_result(t, mbedtls_ssl_write(&t->tls.ssl, data, n), "tls-write"); }
void sp_transport_free(sp_transport *t)
{
    if (!t) return;
    /* No SDK fallback, implicit reconnection or blocking close-notify loop. */
    mbedtls_net_free(&t->tls.server_fd);
    mbedtls_ssl_free(&t->tls.ssl);
    mbedtls_ssl_config_free(&t->tls.conf);
    mbedtls_x509_crt_free(&t->tls.cacert);
    mbedtls_ctr_drbg_free(&t->tls.ctr_drbg);
    mbedtls_entropy_free(&t->tls.entropy);
    speech_free(t->tls.host); speech_free(t->tls.buffer); speech_free(t->tls.port);
    sp_wipe(t, sizeof(*t)); speech_free(t);
}
