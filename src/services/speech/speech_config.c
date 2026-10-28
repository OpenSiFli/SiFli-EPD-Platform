/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_settings.h"
#include "ai/ai_json.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include <dfs_posix.h>
#include <rthw.h>
#include <errno.h>
#include <math.h>
#include <string.h>

static struct rt_mutex mutex;
static bool mutex_ready, writing;
static speech_config_t *published;

/* Bilingual presets: cloud.tencent.com/document/product/1073/92668,
 * help.aliyun.com/zh/model-studio/cosyvoice-voice-list and
 * docs.volcengine.com/docs/DoubaoVoice/Tonelist-1?lang=zh. */
static const speech_voice_t tencent_voices[] = {
    {"501001", "智兰", "Zhilan"}, {"501003", "智宇", "Zhiyu"}
};
static const speech_voice_t ali_voices[] = {
    {"longanyang", "龙安洋", "Long Anyang"}, {"longanhuan", "龙安欢", "Long Anhuan"}
};
static const speech_voice_t doubao_voices[] = {
    {"zh_female_vv_uranus_bigtts", "Vivi 2.0", "Vivi 2.0"},
    {"zh_male_m191_uranus_bigtts", "云舟 2.0", "Yunzhou 2.0"}
};

void speech_config_wipe(void *data, size_t bytes)
{
    volatile unsigned char *p = data;
    while (bytes--) *p++ = 0;
}

const char *speech_provider_id(speech_provider_t p)
{
    static const char *const names[] = {"none", "tencent", "ali", "doubao"};
    return (unsigned)p < SPEECH_PROVIDER_COUNT ? names[p] : NULL;
}

bool speech_provider_supports(speech_provider_t p, epd_speech_kind_t kind)
{
    return (kind == EPD_SPEECH_ASR && (p == SPEECH_TENCENT || p == SPEECH_ALI)) ||
           (kind == EPD_SPEECH_TTS && p > SPEECH_NONE && p < SPEECH_PROVIDER_COUNT);
}

const speech_voice_t *speech_settings_voices(speech_provider_t p, size_t *count)
{
    if (!count) return NULL;
    *count = 2;
    switch (p) {
    case SPEECH_TENCENT: return tencent_voices;
    case SPEECH_ALI: return ali_voices;
    case SPEECH_DOUBAO: return doubao_voices;
    default: *count = 0; return NULL;
    }
}

void speech_config_defaults(speech_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = SPEECH_CONFIG_VERSION;
}

static bool lock(bool wait)
{
    return mutex_ready && rt_mutex_take(&mutex, wait ? RT_WAITING_FOREVER : 0) == RT_EOK;
}
static void unlock(void) { rt_mutex_release(&mutex); }

static epd_speech_error_t reserve(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!mutex_ready) mutex_ready = rt_mutex_init(&mutex, "sp_cfg", RT_IPC_FLAG_PRIO) == RT_EOK;
    rt_hw_interrupt_enable(level);
    if (!lock(true)) return EPD_SPEECH_NO_MEMORY;
    epd_speech_error_t error = writing ? EPD_SPEECH_BUSY : EPD_SPEECH_OK;
    if (!error) writing = true;
    unlock();
    return error;
}
static void release(void) { lock(true); writing = false; unlock(); }

static bool token(const char *text, size_t cap, bool required)
{
    const char *end = memchr(text, 0, cap);
    if (!end || (required && end == text)) return false;
    for (const char *p = text; p < end; ++p)
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || *p == '"' || *p == '\\') return false;
    return true;
}

static bool account_valid(const speech_account_t *a, speech_provider_t expected)
{
    if (!token(a->app_id, sizeof(a->app_id), false) ||
        !token(a->secret_id, sizeof(a->secret_id), false) ||
        !token(a->workspace, sizeof(a->workspace), false) ||
        !token(a->secret, sizeof(a->secret), a->provider != SPEECH_NONE) ||
        !token(a->voice, sizeof(a->voice), false)) return false;
    if (a->provider == SPEECH_NONE)
        return !a->enabled && !a->app_id[0] && !a->secret_id[0] && !a->secret[0] &&
               !a->workspace[0] && !a->voice[0] && a->rate == 0;
    float minimum = expected == SPEECH_TENCENT ? 0.6f : 0.5f;
    if (a->provider != expected || !isfinite(a->rate) ||
        (a->rate != 0 && (a->rate < minimum || a->rate > 2.0f))) return false;
    if (expected == SPEECH_TENCENT) {
        if (!a->app_id[0] || !a->secret_id[0] || a->workspace[0]) return false;
        for (const char *p = a->app_id; *p; ++p) if (*p < '0' || *p > '9') return false;
        for (const char *p = a->secret_id; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return false;
    } else if (expected == SPEECH_ALI) {
        size_t n = strlen(a->workspace);
        if (a->app_id[0] || a->secret_id[0] || n > 63 ||
            (n && (a->workspace[0] == '-' || a->workspace[n-1] == '-'))) return false;
        for (const char *p = a->workspace; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    } else if (expected == SPEECH_DOUBAO) {
        if (a->app_id[0] || a->secret_id[0] || a->workspace[0]) return false;
    } else return false;
    if (!a->voice[0]) return true;
    if (a->rate < minimum) return false;
    size_t count;
    const speech_voice_t *voices = speech_settings_voices(expected, &count);
    for (size_t i = 0; i < count; ++i) if (!strcmp(a->voice, voices[i].id)) return true;
    return false;
}

epd_speech_error_t speech_config_validate(const speech_config_t *c)
{
    if (!c) return EPD_SPEECH_INVALID;
    if (c->version != SPEECH_CONFIG_VERSION) return EPD_SPEECH_UNSUPPORTED;
    if ((unsigned)c->asr >= SPEECH_PROVIDER_COUNT || (unsigned)c->tts >= SPEECH_PROVIDER_COUNT ||
        (c->asr && !speech_provider_supports(c->asr, EPD_SPEECH_ASR)) ||
        (c->tts && !speech_provider_supports(c->tts, EPD_SPEECH_TTS))) return EPD_SPEECH_UNSUPPORTED;
    for (unsigned i = 0; i < SPEECH_PROVIDER_COUNT; ++i)
        if (!account_valid(&c->accounts[i], (speech_provider_t)i)) return EPD_SPEECH_INVALID;
    if ((c->asr && (!c->accounts[c->asr].enabled || c->accounts[c->asr].provider != c->asr)) ||
        (c->tts && (!c->accounts[c->tts].enabled || c->accounts[c->tts].provider != c->tts ||
                    !c->accounts[c->tts].voice[0]))) return EPD_SPEECH_NOT_CONFIGURED;
    return EPD_SPEECH_OK;
}

epd_speech_error_t speech_config_snapshot(speech_config_t *out)
{
    if (!out) return EPD_SPEECH_INVALID;
    memset(out, 0, sizeof(*out));
    if (!lock(false)) return EPD_SPEECH_BUSY;
    bool ready = published != NULL;
    if (ready) *out = *published;
    unlock();
    return ready ? EPD_SPEECH_OK : EPD_SPEECH_NOT_CONFIGURED;
}

epd_speech_error_t speech_config_account(epd_speech_kind_t kind, speech_account_t *out)
{
    if (!out) return EPD_SPEECH_INVALID;
    memset(out, 0, sizeof(*out));
    if (kind != EPD_SPEECH_ASR && kind != EPD_SPEECH_TTS) return EPD_SPEECH_INVALID;
    if (!lock(false)) return EPD_SPEECH_BUSY;
    speech_provider_t p = published ? (kind == EPD_SPEECH_ASR ? published->asr : published->tts) : SPEECH_NONE;
    bool ready = speech_provider_supports(p, kind) && published->accounts[p].enabled &&
                 (kind != EPD_SPEECH_TTS || published->accounts[p].voice[0]);
    if (ready) *out = published->accounts[p];
    unlock();
    return ready ? EPD_SPEECH_OK : EPD_SPEECH_NOT_CONFIGURED;
}

epd_speech_error_t speech_config_public(speech_settings_snapshot_t *out)
{
    if (!out) return EPD_SPEECH_INVALID;
    if (!lock(false)) return EPD_SPEECH_BUSY;
    if (published) {
        out->initialized = true; out->revision = published->revision;
        out->asr = published->asr; out->tts = published->tts;
        for (unsigned i = 0; i < SPEECH_PROVIDER_COUNT; ++i) {
            const speech_account_t *a = &published->accounts[i];
            out->accounts[i].configured = a->provider != SPEECH_NONE;
            out->accounts[i].enabled = a->enabled;
            memcpy(out->accounts[i].voice, a->voice, sizeof(a->voice));
            out->accounts[i].rate = a->rate;
        }
    }
    bool ready = published != NULL;
    unlock();
    return ready ? EPD_SPEECH_OK : EPD_SPEECH_NOT_CONFIGURED;
}

static epd_speech_error_t json_error(epd_ai_error_t error)
{ return error == EPD_AI_NO_MEMORY ? EPD_SPEECH_NO_MEMORY : EPD_SPEECH_INVALID; }

static void erase_json(ai_cjson *node)
{
    for (; node; node = node->next) {
        if (node->valuestring) speech_config_wipe(node->valuestring, strlen(node->valuestring));
        if (node->child) erase_json(node->child);
    }
}

static bool fields(const ai_cjson *node, const char *const *allowed, size_t count)
{
    if (!ai_cjson_IsObject(node)) return false;
    for (const ai_cjson *p = node->child; p; p = p->next) {
        size_t i = 0;
        while (i < count && strcmp(p->string, allowed[i])) ++i;
        if (i == count) return false;
    }
    return true;
}
static const ai_cjson *get(const ai_cjson *node, const char *key)
{ return ai_cjson_GetObjectItemCaseSensitive(node, key); }
static bool string_value(const ai_cjson *node, const char *key, char *out, size_t capacity, bool required)
{
    const ai_cjson *v = get(node, key);
    if (!v) return !required;
    if (!ai_cjson_IsString(v) || strlen(v->valuestring) >= capacity) return false;
    strcpy(out, v->valuestring);
    return !required || out[0];
}
static bool provider_value(const ai_cjson *node, speech_provider_t *out)
{
    if (!ai_cjson_IsString(node)) return false;
    for (unsigned i = 0; i < SPEECH_PROVIDER_COUNT; ++i)
        if (!strcmp(node->valuestring, speech_provider_id((speech_provider_t)i))) { *out = (speech_provider_t)i; return true; }
    return false;
}

epd_speech_error_t speech_config_decode(const char *text, size_t bytes, speech_config_t *out)
{
    if (!text || !out || !bytes || bytes > SPEECH_CONFIG_MAX_BYTES) return EPD_SPEECH_INVALID;
    speech_config_defaults(out);
    if (!ai_json_begin()) return EPD_SPEECH_NO_MEMORY;
    epd_ai_error_t parse_error = EPD_AI_OK;
    ai_cjson *root = ai_json_parse(text, bytes, &parse_error);
    epd_speech_error_t error = root ? EPD_SPEECH_INVALID : json_error(parse_error);
    static const char *const root_fields[] = {"version", "asr", "tts", "accounts"};
    static const char *const account_fields[] = {"provider", "enabled", "app_id", "secret_id", "secret", "workspace", "voice", "rate"};
    if (!fields(root, root_fields, 4)) goto done;
    const ai_cjson *v = get(root, "version"), *accounts = get(root, "accounts");
    if (!ai_cjson_IsNumber(v)) goto done;
    if (v->valuedouble != SPEECH_CONFIG_VERSION) { error = EPD_SPEECH_UNSUPPORTED; goto done; }
    if (!provider_value(get(root, "asr"), &out->asr) || !provider_value(get(root, "tts"), &out->tts)) {
        error = EPD_SPEECH_UNSUPPORTED; goto done;
    }
    if (!ai_cjson_IsArray(accounts) || ai_cjson_GetArraySize(accounts) > 3) goto done;
    for (const ai_cjson *item = accounts->child; item; item = item->next) {
        speech_provider_t p;
        if (!fields(item, account_fields, 8) || !provider_value(get(item, "provider"), &p) || p == SPEECH_NONE) goto done;
        speech_account_t *a = &out->accounts[p];
        if (a->provider) goto done;
        a->provider = p;
        const ai_cjson *enabled = get(item, "enabled"), *rate = get(item, "rate");
        if (!ai_cjson_IsBool(enabled) || (rate && (!ai_cjson_IsNumber(rate) || !isfinite(rate->valuedouble) ||
            rate->valuedouble < 0 || rate->valuedouble > 2.0))) goto done;
        a->enabled = ai_cjson_IsTrue(enabled); a->rate = rate ? (float)rate->valuedouble : 0;
        if (!string_value(item, "app_id", a->app_id, sizeof(a->app_id), p == SPEECH_TENCENT) ||
            !string_value(item, "secret_id", a->secret_id, sizeof(a->secret_id), p == SPEECH_TENCENT) ||
            !string_value(item, "secret", a->secret, sizeof(a->secret), true) ||
            !string_value(item, "workspace", a->workspace, sizeof(a->workspace), false) ||
            !string_value(item, "voice", a->voice, sizeof(a->voice), false)) goto done;
    }
    error = speech_config_validate(out);
done:
    erase_json(root); ai_cjson_Delete(root); ai_json_end();
    if (error) speech_config_wipe(out, sizeof(*out));
    return error;
}

static epd_speech_error_t encode(const speech_config_t *c, char **out)
{
    *out = NULL;
    if (!ai_json_begin()) return EPD_SPEECH_NO_MEMORY;
    ai_cjson *root = ai_cjson_CreateObject(), *accounts = NULL;
    bool ok = root && ai_cjson_AddNumberToObject(root, "version", c->version) &&
        ai_cjson_AddStringToObject(root, "asr", speech_provider_id(c->asr)) &&
        ai_cjson_AddStringToObject(root, "tts", speech_provider_id(c->tts));
    if (ok) { accounts = ai_cjson_AddArrayToObject(root, "accounts"); ok = accounts != NULL; }
    for (unsigned i = 1; ok && i < SPEECH_PROVIDER_COUNT; ++i) {
        const speech_account_t *a = &c->accounts[i];
        if (!a->provider) continue;
        ai_cjson *item = ai_cjson_CreateObject();
        if (!item) { ok = false; break; }
        if (!ai_cjson_AddItemToArray(accounts, item)) { ai_cjson_Delete(item); ok = false; break; }
        ok = ai_cjson_AddStringToObject(item, "provider", speech_provider_id(a->provider)) &&
            ai_cjson_AddBoolToObject(item, "enabled", a->enabled) &&
            ai_cjson_AddStringToObject(item, "secret", a->secret) &&
            ai_cjson_AddStringToObject(item, "voice", a->voice) &&
            ai_cjson_AddNumberToObject(item, "rate", a->rate);
        if (ok && a->provider == SPEECH_TENCENT)
            ok = ai_cjson_AddStringToObject(item, "app_id", a->app_id) && ai_cjson_AddStringToObject(item, "secret_id", a->secret_id);
        if (ok && a->provider == SPEECH_ALI) ok = ai_cjson_AddStringToObject(item, "workspace", a->workspace) != NULL;
    }
    epd_ai_error_t error = EPD_AI_NO_MEMORY;
    if (ok) *out = ai_json_print(root, &error);
    erase_json(root); ai_cjson_Delete(root); ai_json_end();
    return *out ? EPD_SPEECH_OK : json_error(error);
}

static bool import_path(const char *path)
{
    if (!path || !memchr(path, 0, SPEECH_SETTINGS_PATH_SIZE)) return false;
    size_t n = strlen(path), root = strlen(STORAGE_SD_ROOT);
    if (n < root + 7 || strncmp(path, STORAGE_SD_ROOT "/", root + 1) || strcmp(path+n-5, ".json")) return false;
    const char *segment = path + root + 1;
    for (const char *p = segment; ; ++p) {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':' || (unsigned char)*p == 127)) return false;
        if (*p == '/' || !*p) {
            size_t size = (size_t)(p-segment);
            if (!size || (size == 1 && segment[0] == '.') || (size == 2 && !strncmp(segment, "..", 2))) return false;
            segment = p+1;
        }
        if (!*p) return true;
    }
}

static epd_speech_error_t read_config(const char *path, bool importing, uint32_t session, speech_config_t *out)
{
    int fd = -1; char *text = NULL; size_t size = 0;
    epd_speech_error_t error = EPD_SPEECH_STORAGE;
    storage_lock();
    if (importing && !storage_session_valid(STORAGE_SD, session)) { error = EPD_SPEECH_CANCELLED; goto done; }
    if (!storage_path_available(path) || (!importing && !storage_file_recover(path))) goto done;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        if (!importing && (errno == ENOENT || errno == -ENOENT)) { speech_config_defaults(out); error = EPD_SPEECH_OK; }
        goto done;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) goto done;
    if (!st.st_size || (uint64_t)st.st_size > SPEECH_CONFIG_MAX_BYTES) { error = EPD_SPEECH_INVALID; goto done; }
    size = (size_t)st.st_size; text = speech_alloc(size + 1);
    if (!text) { error = EPD_SPEECH_NO_MEMORY; goto done; }
    for (size_t used = 0; used < size; ) {
        int n = read(fd, text+used, size-used);
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    char tail;
    if (read(fd, &tail, 1) != 0 || !storage_path_available(path)) goto done;
    if (importing && !storage_session_valid(STORAGE_SD, session)) { error = EPD_SPEECH_CANCELLED; goto done; }
    text[size] = 0; error = EPD_SPEECH_OK;
done:
    if (fd >= 0 && close(fd)) error = EPD_SPEECH_STORAGE;
    storage_unlock();
    if (!error && text) error = speech_config_decode(text, size, out);
    if (text) { speech_config_wipe(text, size+1); speech_free(text); }
    return error;
}

static void publish(speech_config_t *next)
{
    lock(true);
    speech_config_t *old = published;
    published = next;
    unlock();
    if (old) { speech_config_changed(old, next); speech_config_wipe(old, sizeof(*old)); speech_free(old); }
}

epd_speech_error_t speech_config_load(void)
{
    epd_speech_error_t error = reserve();
    if (error) return error;
    speech_config_t *next = speech_alloc(sizeof(*next));
    if (!next) error = EPD_SPEECH_NO_MEMORY;
    else {
        error = read_config(SPEECH_CONFIG_PATH, false, 0, next);
        if (!error && published && published->revision == UINT32_MAX) error = EPD_SPEECH_BUSY;
        if (!error) { next->revision = published ? published->revision + 1 : 1; publish(next); next = NULL; }
        if (next) { speech_config_wipe(next, sizeof(*next)); speech_free(next); }
    }
    release(); return error;
}

epd_speech_error_t speech_config_import_read(const char *path, uint32_t session, speech_config_t *out)
{
    if (!out || !import_path(path) || !session) return EPD_SPEECH_INVALID;
    return read_config(path, true, session, out);
}

epd_speech_error_t speech_config_commit(const speech_config_t *candidate, uint32_t session)
{
    epd_speech_error_t error = speech_config_validate(candidate);
    if (error) return error;
    error = reserve(); if (error) return error;
    speech_config_t *next = speech_alloc(sizeof(*next)); char *text = NULL;
    if (!next) { error = EPD_SPEECH_NO_MEMORY; goto done; }
    *next = *candidate;
    if ((published && next->revision != published->revision) || (!published && next->revision)) { error = EPD_SPEECH_BUSY; goto done; }
    if (next->revision == UINT32_MAX) { error = EPD_SPEECH_BUSY; goto done; }
    ++next->revision;
    error = encode(next, &text); if (error) goto done;
    storage_lock();
    if (session && !storage_session_valid(STORAGE_SD, session)) error = EPD_SPEECH_CANCELLED;
    else if (!storage_file_replace(SPEECH_CONFIG_PATH, text, strlen(text))) error = EPD_SPEECH_STORAGE;
    storage_unlock();
    if (!error) { publish(next); next = NULL; }
done:
    if (text) { speech_config_wipe(text, strlen(text)); ai_free(text); }
    if (next) { speech_config_wipe(next, sizeof(*next)); speech_free(next); }
    release(); return error;
}

epd_speech_error_t speech_config_clear(void)
{
    epd_speech_error_t error = reserve(); if (error) return error;
    speech_config_t *empty = speech_alloc(sizeof(*empty));
    if (!empty) { release(); return EPD_SPEECH_NO_MEMORY; }
    speech_config_defaults(empty);
    empty->revision = published && published->revision != UINT32_MAX ? published->revision + 1 : 1;
    storage_lock();
    if (!storage_path_available(SPEECH_CONFIG_PATH) || !system_reset_write_allowed()) error = EPD_SPEECH_STORAGE;
    /* Remove recoverable copies first, so a partial reset cannot resurrect
     * credentials after the main record was deleted. */
    const char *paths[] = {SPEECH_CONFIG_TEMP_PATH, SPEECH_CONFIG_BACKUP_PATH, SPEECH_CONFIG_PATH};
    for (size_t i = 0; !error && i < 3; ++i)
        if (unlink(paths[i]) && errno != ENOENT && errno != -ENOENT) error = EPD_SPEECH_STORAGE;
    storage_unlock();
    if (!error) { publish(empty); empty = NULL; }
    if (empty) { speech_config_wipe(empty, sizeof(*empty)); speech_free(empty); }
    release(); return error;
}
