/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_SETTINGS_H
#define SPEECH_SETTINGS_H
#include "speech_internal.h"

#define SPEECH_SETTINGS_OWNER "sp_cfg"
#define SPEECH_CONFIG_PATH "/flash/config/speech-services.json"
#define SPEECH_CONFIG_TEMP_PATH SPEECH_CONFIG_PATH ".tmp"
#define SPEECH_CONFIG_BACKUP_PATH SPEECH_CONFIG_PATH ".old"
#define SPEECH_CONFIG_VERSION 1u
#define SPEECH_CONFIG_MAX_BYTES 16384u
#define SPEECH_SETTINGS_PATH_SIZE 512u
#define SPEECH_SETTINGS_QUEUE_SIZE 4u
#define SPEECH_SETTINGS_HISTORY_SIZE 8u

typedef struct {
    const char *id, *name_zh, *name_en;
} speech_voice_t;
bool speech_provider_supports(speech_provider_t provider, epd_speech_kind_t kind);
const char *speech_provider_id(speech_provider_t provider);
const speech_voice_t *speech_settings_voices(speech_provider_t provider, size_t *count);

typedef enum {
    SPEECH_SETTINGS_RELOAD, SPEECH_SETTINGS_SAVE_ACCOUNT, SPEECH_SETTINGS_REMOVE,
    SPEECH_SETTINGS_DEFAULT, SPEECH_SETTINGS_IMPORT_PREPARE, SPEECH_SETTINGS_IMPORT,
    SPEECH_SETTINGS_CLEAR
} speech_settings_operation_t;
typedef struct {
    speech_settings_operation_t operation;
    speech_account_t account;
    bool keep_credentials; /* Preserve empty credential fields; replace supplied fields. */
    speech_provider_t provider;
    epd_speech_kind_t kind;
    char path[SPEECH_SETTINGS_PATH_SIZE];
    uint32_t card_session, candidate_id;
} speech_settings_job_t;
typedef struct {
    uint32_t job_id, candidate_id;
    speech_settings_operation_t operation;
    epd_speech_error_t error;
} speech_settings_result_t;
typedef struct {
    bool configured, enabled;
    char voice[128];
    float rate;
} speech_settings_account_t;
/* This UI view contains no credential fields, including account identifiers. */
typedef struct {
    bool initialized, busy, stopping;
    epd_speech_error_t initialization_error;
    uint32_t revision;
    speech_provider_t asr, tts;
    speech_settings_account_t accounts[SPEECH_PROVIDER_COUNT];
} speech_settings_snapshot_t;

/* UI owner starts/restarts the service; all initial reads and writes run on
 * its worker. snapshot/submit/poll never perform storage or network I/O. */
epd_speech_error_t speech_settings_start(void);
void speech_settings_process(void);
epd_speech_error_t speech_settings_snapshot(speech_settings_snapshot_t *out);
epd_speech_error_t speech_settings_submit(const speech_settings_job_t *job, uint32_t *id);
epd_speech_error_t speech_settings_poll(uint32_t id, speech_settings_result_t *out);
void speech_settings_import_discard(uint32_t candidate_id);
bool speech_settings_flush(void);
void speech_settings_stop(void);
bool speech_settings_idle(void);
/* Reset worker only, after stop + idle; no storage lock may be held.
 * Clears the live snapshot and removes main/.tmp/.old. Errors are retryable.
 * WLAN/Bluetooth-only reset must not call this hook. */
epd_speech_error_t speech_settings_reset_storage(void);

/* Settings/reset worker implementation; never call these from the GUI. */
void speech_config_defaults(speech_config_t *out);
void speech_config_wipe(void *data, size_t bytes);
epd_speech_error_t speech_config_validate(const speech_config_t *config);
epd_speech_error_t speech_config_decode(const char *text, size_t bytes, speech_config_t *out);
epd_speech_error_t speech_config_load(void);
epd_speech_error_t speech_config_commit(const speech_config_t *config, uint32_t card_session);
epd_speech_error_t speech_config_import_read(const char *path, uint32_t session, speech_config_t *out);
epd_speech_error_t speech_config_clear(void);
epd_speech_error_t speech_config_public(speech_settings_snapshot_t *out);
#endif
