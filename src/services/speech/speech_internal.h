/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_INTERNAL_H
#define SPEECH_INTERNAL_H
#include "epd_speech.h"
#include <rtthread.h>
typedef enum { SPEECH_NONE, SPEECH_TENCENT, SPEECH_ALI, SPEECH_DOUBAO, SPEECH_PROVIDER_COUNT } speech_provider_t;
#define SPEECH_SECRET_SIZE 512u
typedef struct {
    speech_provider_t provider;
    bool enabled;
    char app_id[64], secret_id[128], secret[SPEECH_SECRET_SIZE], workspace[128];
    char voice[128];
    float rate;
} speech_account_t;
typedef struct {
    uint32_t version, revision;
    speech_provider_t asr, tts;
    speech_account_t accounts[SPEECH_PROVIDER_COUNT];
} speech_config_t;
typedef struct {
    epd_speech_error_t error;
    int detail, http_status;
    uint32_t tls_verify_flags;
    char stage[32], code[96], request_id[128];
} speech_failure_t;
typedef bool (*speech_cancel_fn)(void *user);

/* Platform lifecycle hooks. None waits on GUI callbacks. */
void speech_process(void);
void speech_owner_stop(epd_service_t *owner);
void speech_stop_all(void);
bool speech_idle(void);
bool speech_flush(void);
void speech_config_changed(const speech_config_t *previous, const speech_config_t *next);
/* Short configuration snapshots; initialization/storage are on the settings worker. */
epd_speech_error_t speech_config_snapshot(speech_config_t *out);
epd_speech_error_t speech_config_account(epd_speech_kind_t kind, speech_account_t *out);

/* Shared PSRAM allocation, monotonic milliseconds and bounded cancellation. */
void *speech_alloc(size_t size);
void speech_free(void *ptr);
uint32_t speech_now_ms(void);

#endif
