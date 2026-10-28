/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_wire.h"
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <string.h>

void sp_wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}
void sp_safe_id(char *out, size_t capacity, const char *p, size_t n)
{
    size_t i;
    if (!capacity) return;
    out[0] = 0;
    if (n >= capacity) return;
    for (i = 0; i < n; ++i) {
        unsigned c = (unsigned char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return;
    }
    memcpy(out, p, n); out[n] = 0;
}
epd_speech_error_t sp_fail(speech_failure_t *f, epd_speech_error_t error,
                           const char *stage, int detail)
{
    if (!f->error) {
        f->error = error; f->detail = detail;
        sp_safe_id(f->stage, sizeof(f->stage), stage, strlen(stage));
    }
    return f->error;
}
bool sp_check(sp_guard *g)
{
    if (g->failure->error) return false;
    if (g->cancel && g->cancel(g->user))
        return sp_fail(g->failure, EPD_SPEECH_CANCELLED, "cancel", 0) == EPD_SPEECH_OK;
    if ((int32_t)(speech_now_ms() - g->deadline) >= 0)
        return sp_fail(g->failure, EPD_SPEECH_TIMEOUT, "deadline", 0) == EPD_SPEECH_OK;
    return true;
}
bool sp_sha1(const void *p, size_t n, unsigned char digest[20])
{
    return mbedtls_sha1_ret(p, n, digest) == 0;
}
bool sp_hmac_sha1(const char *key, const char *p, unsigned char digest[20])
{
    unsigned char block[64] = {0}, inner[20];
    size_t n = strlen(key);
    mbedtls_sha1_context context;
    int ret;
    if (n > sizeof(block)) { if (!sp_sha1(key, n, block)) return false; }
    else memcpy(block, key, n);
    for (unsigned i = 0; i < sizeof(block); ++i) block[i] ^= 0x36;
    mbedtls_sha1_init(&context);
    ret = mbedtls_sha1_starts_ret(&context);
    if (!ret) ret = mbedtls_sha1_update_ret(&context, block, sizeof(block));
    if (!ret) ret = mbedtls_sha1_update_ret(&context, (const unsigned char *)p, strlen(p));
    if (!ret) ret = mbedtls_sha1_finish_ret(&context, inner);
    for (unsigned i = 0; i < sizeof(block); ++i) block[i] ^= 0x36 ^ 0x5c;
    if (!ret) ret = mbedtls_sha1_starts_ret(&context);
    if (!ret) ret = mbedtls_sha1_update_ret(&context, block, sizeof(block));
    if (!ret) ret = mbedtls_sha1_update_ret(&context, inner, sizeof(inner));
    if (!ret) ret = mbedtls_sha1_finish_ret(&context, digest);
    mbedtls_sha1_free(&context);
    sp_wipe(block, sizeof(block)); sp_wipe(inner, sizeof(inner));
    return !ret;
}
bool sp_base64(const void *p, size_t n, char *out, size_t capacity)
{
    size_t used = 0;
    if (mbedtls_base64_encode((unsigned char *)out, capacity, &used, p, n) || used >= capacity) return false;
    out[used] = 0; return true;
}
