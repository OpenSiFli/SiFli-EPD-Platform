#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include "epd_http_stream.h"

#define RT_NULL NULL
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_EINVAL 22
#define RT_ENOMEM 12
#define RT_EOK 0
#define ERR_OK 0
#define MBEDTLS_SSL_VERIFY_REQUIRED 2
#define MBEDTLS_SSL_HANDSHAKE_OVER 16
#define MBEDTLS_ERR_SSL_CONN_EOF -0x7280
#define MBEDTLS_ERR_SSL_WANT_READ -10
#define MBEDTLS_ERR_SSL_WANT_WRITE -11
#define MBEDTLS_ERR_NET_CONN_RESET -12
#define MBEDTLS_ERR_NET_SEND_FAILED -13
#define MBEDTLS_ERR_NET_RECV_FAILED -14
#define MBEDTLS_ERR_NET_INVALID_CONTEXT -15
typedef uint32_t rt_tick_t;
typedef uint32_t epd_memory_owner_t;
typedef void epd_activity_work_t;
typedef struct { int fd; } mbedtls_net_context;
typedef struct { int state; size_t out_left; } ssl_t;
typedef struct {
    mbedtls_net_context server_fd;
    ssl_t ssl;
    struct { int authmode; } conf;
    char *host;
} MbedTLSSession;
typedef struct { uint32_t addr; } ip_addr_t;
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->addr)
typedef enum { NETWORK_WIFI, NETWORK_PAN, NETWORK_WIFI_AP } network_source_t;
typedef struct { bool ready; unsigned generation; network_source_t source; uint32_t address; } network_snapshot_t;
typedef struct { bool ready; uint32_t address, netmask; unsigned char mac[6]; } network_interface_snapshot_t;
#include "reader_actual.inc"

static rt_tick_t clock_ticks;
static const char *scenario;
static unsigned allocations, output_count, send_count, handshake_count;
static unsigned tls_step_count;
static bool stopped, changed;
static uint32_t verify;
static char output[1024];
static rt_tick_t rt_tick_get(void) { return clock_ticks; }
static rt_tick_t ticks(unsigned ms) { return (rt_tick_t)((uint64_t)ms * RT_TICK_PER_SECOND / 1000); }
static void rt_thread_mdelay(unsigned ms) { clock_ticks += ticks(ms); }
static void step(const char *name, unsigned ms)
{
    char key[64]; clock_ticks += ticks(ms);
    snprintf(key, sizeof(key), "timeout_%s", name);
    if (!strcmp(scenario, key)) clock_ticks += ticks(30000);
    snprintf(key, sizeof(key), "cancel_%s", name);
    if (!strcmp(scenario, key)) stopped = true;
    if (!strcmp(scenario, "network_dns") && !strcmp(name, "dns")) changed = true;
}
static int rt_kprintf(const char *format, ...)
{
    char line[sizeof(output)];
    va_list ap; va_start(ap, format);
    int n = vsnprintf(line, sizeof(line), format, ap); va_end(ap);
    assert(n > 0 && (size_t)n < sizeof(line));
    if (strncmp(line, "http_tls_", 9) == 0) {
        assert(!strstr(line, "SECRET") && !strstr(line, "secret-host"));
        if (strncmp(line, "http_tls_step ", 14) == 0) ++tls_step_count;
        return n;
    }
    memcpy(output, line, (size_t)n + 1);
    ++output_count;
    return n;
}
static bool cancelled(void *unused) { (void)unused; return stopped; }
static void network_snapshot(network_snapshot_t *state)
{ *state = (network_snapshot_t){true, changed ? 2u : 1u, NETWORK_WIFI, 0x0100007fu}; }
static int network_interface_snapshot(network_source_t source, network_interface_snapshot_t *state)
{ (void)source; memset(state, 0, sizeof(*state)); return -1; }
#define EPD_APP_PSRAM 1
static void *epd_app_alloc(size_t bytes, int region)
{
    assert(region == EPD_APP_PSRAM);
    if (!strcmp(scenario, "alloc_fail")) return NULL;
    void *p = calloc(1, bytes); assert(p); ++allocations; return p;
}
static void epd_app_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
static void *rt_calloc(size_t n, size_t size) { return epd_app_alloc(n * size, EPD_APP_PSRAM); }
static char *rt_strdup(const char *s) { char *p = epd_app_alloc(strlen(s) + 1, EPD_APP_PSRAM); if (p) strcpy(p, s); return p; }
static int mbedtls_client_init(MbedTLSSession *tls, const char *seed, size_t length)
{ (void)tls; (void)seed; (void)length; step("init", 10); return !strcmp(scenario, "fail_init") ? -80 : 0; }
static int mbedtls_client_context(MbedTLSSession *tls)
{ step("ca", 20); tls->conf.authmode = MBEDTLS_SSL_VERIFY_REQUIRED; return !strcmp(scenario, "fail_ca") ? -81 : 0; }
static int netconn_gethostbyname(const char *host, ip_addr_t *ip)
{ assert(host); step("dns", 30); ip->addr = 0x08080808; return !strcmp(scenario, "fail_dns") ? -82 : 0; }
static int test_socket(int a, int b, int c) { (void)a; (void)b; (void)c; return 3; }
static int test_ioctl(int fd, unsigned long request, int *value) { (void)fd; (void)request; (void)value; return 0; }
static int test_setsockopt(int fd, int a, int b, const void *p, socklen_t n) { (void)fd; (void)a; (void)b; (void)p; (void)n; return 0; }
static int test_bind(int fd, const struct sockaddr *p, socklen_t n) { (void)fd; (void)p; (void)n; return 0; }
static int test_connect(int fd, const struct sockaddr *p, socklen_t n) { (void)fd; (void)p; (void)n; errno = EINPROGRESS; return -1; }
static int test_select(int n, fd_set *a, fd_set *b, fd_set *c, struct timeval *t)
{ (void)n; (void)a; (void)b; (void)c; (void)t; step("tcp", 40); return stopped || !strcmp(scenario, "timeout_tcp") ? 0 : 1; }
static int test_getsockopt(int fd, int a, int b, void *p, socklen_t *n)
{ (void)fd; (void)a; (void)b; (void)n; *(int *)p = !strcmp(scenario, "fail_tcp") ? 83 : 0; return 0; }
static int test_send(int fd, const void *p, size_t n, int flags) { (void)fd; (void)p; (void)flags; return (int)n; }
static int test_recv(int fd, void *p, size_t n, int flags) { (void)fd; (void)p; (void)n; (void)flags; errno = EAGAIN; return -1; }
static void mbedtls_ssl_set_bio(ssl_t *ssl, void *ctx, int (*send_fn)(void *, const unsigned char *, size_t),
                              int (*recv_fn)(void *, unsigned char *, size_t), void *timeout)
{ (void)ssl; (void)ctx; (void)send_fn; (void)recv_fn; (void)timeout; }
static int mbedtls_ssl_handshake(ssl_t *ssl)
{
    ssl->state = 20 + (int)++handshake_count; step("hs", 50);
    if (!strcmp(scenario, "verify_fail")) verify = 7;
    if (!strcmp(scenario, "fail_hs")) return -84;
    if (!strcmp(scenario, "eof_hs")) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (!strcmp(scenario, "retry") && handshake_count < 4) return MBEDTLS_ERR_SSL_WANT_READ;
    if (!strcmp(scenario, "timeout_hs") || stopped) return MBEDTLS_ERR_SSL_WANT_READ;
    return 0;
}
static int mbedtls_ssl_handshake_step(ssl_t *ssl)
{
    if (!ssl->state) {
        ssl->state = 2;
        return 0;
    }
    int result = mbedtls_ssl_handshake(ssl);
    if (!result) ssl->state = MBEDTLS_SSL_HANDSHAKE_OVER;
    return result;
}
static uint32_t mbedtls_ssl_get_verify_result(ssl_t *ssl) { (void)ssl; return verify; }
static void mbedtls_client_close(MbedTLSSession *tls)
{ epd_app_free(tls->host); epd_app_free(tls); clock_ticks += ticks(70); }
#define socket test_socket
#define ioctlsocket test_ioctl
#define setsockopt test_setsockopt
#define bind test_bind
#define connect test_connect
#define select test_select
#define getsockopt test_getsockopt
#define send test_send
#define recv test_recv
static bool request_valid(http_reader_t *r) { (void)r; return true; }
static bool send_request(http_reader_t *r)
{ ++send_count; r->response->stage = "request-write"; step("send", 10); return !expired(r); }
static bool sent_callback(http_reader_t *r) { (void)r; return true; }
static bool receive_body(http_reader_t *r, char *line, char **body, size_t *used, bool *gzip)
{ (void)line; (void)body; (void)used; (void)gzip; r->response->stage = "body-read"; r->response->status = 200; return true; }
#include "core_actual.inc"

static unsigned long field(const char *name)
{
    const char *p = strstr(output, name); assert(p); return strtoul(p + strlen(name), NULL, 10);
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    if (!strcmp(scenario, "wrap")) clock_ticks = UINT32_MAX - ticks(40);
    epd_http_request_t request = {.host="secret-host", .path="/private", .cancelled=cancelled,
        .body="SECRET-BODY", .body_size=11};
    epd_http_header_t header = {"Authorization", "SECRET-TOKEN"}; request.headers = &header; request.header_count = 1;
    epd_http_stream_request_t stream = {.connect_timeout_ms=30000, .send_idle_timeout_ms=1000};
    epd_http_response_t response = {0}; http_reader_t reader = {0};
    reader.request = &request; reader.response = &response; reader.stream = &stream;
    if (!strcmp(scenario, "invalid")) reader.request = NULL;
    char *body = NULL; size_t used = 0; bool gzip = false;
    bool ok = epd_http_core_perform(&reader, &body, &used, &gzip);
    bool success = !strcmp(scenario,"success") || !strcmp(scenario,"wrap") || !strcmp(scenario,"retry");
    assert(ok == success); assert(!allocations);
    const char *stage = success ? "body-read" :
        !strncmp(scenario,"timeout_",8) ? (!strcmp(scenario,"timeout_send") ? "send-timeout" : "connect-timeout") :
        !strncmp(scenario,"cancel_",7) ? "cancelled" :
        !strcmp(scenario,"fail_init") ? "tls-init" : !strcmp(scenario,"fail_ca") ? "tls-context" :
        !strcmp(scenario,"fail_dns") ? "dns" : !strcmp(scenario,"fail_tcp") ? "tcp-connect" :
        (!strcmp(scenario,"fail_hs") || !strcmp(scenario,"eof_hs")) ? "tls-handshake" : !strcmp(scenario,"verify_fail") ? "tls-verify" :
        !strcmp(scenario,"network_dns") ? "network-changed" : !strcmp(scenario,"invalid") ? "request-invalid" : "headers-alloc";
    assert(response.stage && !strcmp(response.stage, stage));
    if (!strncmp(scenario,"timeout_",8)) assert(response.detail == -RT_ETIMEOUT);
    if (!strncmp(scenario,"cancel_",7)) assert(response.detail == -RT_ERROR);
    if (!success && strcmp(scenario,"timeout_send")) assert(!send_count);
#if EPD_INPUT_TEST_ENABLED && !defined(EXPECT_OLD)
    assert(output_count == 1);
    assert(!strstr(output,"SECRET") && !strstr(output,"secret-host") && !strstr(output,"Authorization"));
    assert(strchr(output,'\n') == output + strlen(output) - 1);
    assert(field("phase=") == (success ? 2u : !strcmp(scenario,"timeout_send") ? 1u : 0u));
    const char *origin = strstr(scenario,"init") ? "tls-init" : strstr(scenario,"ca") && !strstr(scenario,"cancel_") ? "tls-context" :
        !strcmp(scenario,"cancel_ca") ? "tls-context" : strstr(scenario,"dns") ? "dns" : strstr(scenario,"tcp") ? "tcp-connect" :
        !strcmp(scenario,"invalid") || !strcmp(scenario,"alloc_fail") ? "prepare" : "tls-handshake";
    char expected[64]; snprintf(expected,sizeof(expected),"conn=%s ",origin); assert(strstr(output,expected));
    if (success) {
        assert(strstr(output,"seen=1f "));
        assert(field("init_ms=") == 10 && field("ca_ms=") == 20 && field("dns_ms=") == 30 && field("tcp_ms=") == 40);
        assert(field("hs_ms=") == (!strcmp(scenario,"retry") ? 230u : 50u));
        assert(field("total_ms=") == (!strcmp(scenario,"retry") ? 410u : 230u));
    }
    if (!strcmp(scenario,"fail_hs")) assert(strstr(output,"last_ret=-84 "));
    if (!strcmp(scenario,"eof_hs"))
        assert(response.detail == MBEDTLS_ERR_SSL_CONN_EOF && strstr(output,"last_ret=-29312 "));
    const char *state = strstr(output, "ssl_state=");
#if defined(BUILD_DLMODULE)
    assert(state && strtol(state + strlen("ssl_state="), NULL, 10) ==
           (handshake_count ? 20L + handshake_count : -1L));
    assert(tls_step_count == handshake_count);
#else
    bool handshake_finished = success || !strcmp(scenario, "timeout_send") || !strcmp(scenario, "verify_fail");
    assert(state && strtol(state + strlen("ssl_state="), NULL, 10) ==
           (handshake_finished ? MBEDTLS_SSL_HANDSHAKE_OVER : handshake_count ? 20L + handshake_count : -1L));
    assert(tls_step_count == (handshake_count ? handshake_count + 1u : 0u));
#endif
    if (!strcmp(scenario,"timeout_dns")) assert(strstr(output,"last_ret=0 ") && field("dns_ms=") == 30030);
    if (!strcmp(scenario,"verify_fail")) assert(strstr(output,"verify=7\n"));
    fputs(output, stdout);
#else
    assert(!output_count && !tls_step_count);
#endif
    printf("PASS %s\n",scenario); return 0;
}
