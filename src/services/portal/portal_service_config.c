/* SPDX-License-Identifier: Apache-2.0 */
#include "portal_service_config.h"
#include "portal_protocol.h"
#include "epd_memory.h"
#include "ai_settings.h"
#include "ai_json.h"
#include "speech_settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void portal_service_config_wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}

static void string_field(portal_json_t *j, const char *name, const char *value)
{
    portal_json_string(j, name); portal_json_raw(j, ":"); portal_json_string(j, value);
}
static void number_field(portal_json_t *j, const char *name, uint32_t value)
{
    portal_json_string(j, name); portal_json_raw(j, ":"); portal_json_number(j, value);
}
static void bool_field(portal_json_t *j, const char *name, bool value)
{
    portal_json_string(j, name); portal_json_raw(j, value ? ":true" : ":false");
}
static void ai_profile_json(portal_json_t *j, const ai_profile_t *p, bool has_secret)
{
    const epd_ai_profile_t *v = &p->public;
    portal_json_raw(j, "{"); string_field(j, "id", v->id);
    portal_json_raw(j, ","); string_field(j, "name", v->name);
    portal_json_raw(j, ","); number_field(j, "protocol", v->protocol);
    portal_json_raw(j, ","); bool_field(j, "enabled", v->enabled);
    portal_json_raw(j, ","); bool_field(j, "is_default", v->is_default);
    portal_json_raw(j, ","); bool_field(j, "has_secret", has_secret);
    portal_json_raw(j, ","); string_field(j, "endpoint", p->endpoint);
    portal_json_raw(j, ","); number_field(j, "auth", p->auth);
    portal_json_raw(j, ","); number_field(j, "features", v->features);
    portal_json_raw(j, ",\"models\":[");
    for (unsigned i = 0; i < v->model_count; ++i)
    {
        const epd_ai_model_t *m = &v->models[i];
        portal_json_raw(j, i ? ",{" : "{"); string_field(j, "id", m->id);
        portal_json_raw(j, ","); number_field(j, "capabilities", m->capabilities);
        portal_json_raw(j, ","); number_field(j, "max_output_tokens", m->max_output_tokens);
        portal_json_raw(j, ","); number_field(j, "token_field", m->token_field);
        portal_json_raw(j, "}");
    }
    portal_json_raw(j, "],\"headers\":[");
    for (unsigned i = 0; i < p->header_count; ++i)
    {
        portal_json_raw(j, i ? ",{" : "{"); string_field(j, "name", p->headers[i].name);
        portal_json_raw(j, ","); string_field(j, "value", p->headers[i].value);
        portal_json_raw(j, "}");
    }
    portal_json_raw(j, "],"); number_field(j, "connect_ms", p->connect_ms);
    portal_json_raw(j, ","); number_field(j, "send_idle_ms", p->send_idle_ms);
    portal_json_raw(j, ","); number_field(j, "first_event_ms", p->first_event_ms);
    portal_json_raw(j, ","); number_field(j, "read_idle_ms", p->read_idle_ms);
    portal_json_raw(j, ","); number_field(j, "total_ms", p->total_ms);
    portal_json_raw(j, "}");
}
static int ai_read(char **output)
{
    /* Includes worst-case JSON escaping for every bounded profile and grant. */
    const size_t capacity = 192u * 1024u;
    ai_settings_snapshot_t *s = ai_alloc(sizeof(*s));
    ai_profile_t *p = ai_alloc(sizeof(*p));
    char *text = epd_app_alloc(capacity, EPD_APP_PSRAM);
    int status = 503;
    if (!s || !p || !text) goto done;
    if (ai_settings_snapshot(s) != EPD_AI_OK || s->stopping) goto done;
    uint32_t generation = s->generation;
    bool initialized = s->initialized;
    portal_json_t j; portal_json_init(&j, text, capacity);
    portal_json_raw(&j, "{"); bool_field(&j, "initialized", initialized);
    portal_json_raw(&j, ","); bool_field(&j, "busy", s->busy);
    portal_json_raw(&j, ","); string_field(&j, "default_id", s->default_id);
    portal_json_raw(&j, ",\"profiles\":[");
    for (size_t i = 0; i < s->profile_count; ++i)
    {
        bool has_secret = false;
        if (ai_settings_profile_get(s->profiles[i].id, p, &has_secret) != EPD_AI_OK) goto done;
        if (i) portal_json_raw(&j, ",");
        ai_profile_json(&j, p, has_secret);
    }
    portal_json_raw(&j, "],\"grants\":[");
    for (size_t i = 0; i < s->grant_count; ++i)
    {
        portal_json_raw(&j, i ? ",{" : "{"); string_field(&j, "app", s->grants[i].app);
        portal_json_raw(&j, ","); string_field(&j, "profile", s->grants[i].profile);
        portal_json_raw(&j, "}");
    }
    portal_json_raw(&j, "]}");
    if (j.failed) goto done;
    /* Profiles are copied separately; retry rather than mix two revisions. */
    if (ai_settings_snapshot(s) != EPD_AI_OK || s->generation != generation ||
        s->initialized != initialized || s->stopping) goto done;
    *output = text; text = NULL; status = 200;
done:
    if (p) { portal_service_config_wipe(p, sizeof(*p)); ai_free(p); }
    ai_free(s); epd_app_free(text); return status;
}
static int speech_read(char **output)
{
    speech_settings_snapshot_t s;
    if (speech_settings_snapshot(&s) != EPD_SPEECH_OK || s.stopping) return 503;
    const size_t capacity = 8192;
    char *text = epd_app_alloc(capacity, EPD_APP_PSRAM);
    if (!text) return 503;
    portal_json_t j; portal_json_init(&j, text, capacity);
    portal_json_raw(&j, "{"); bool_field(&j, "initialized", s.initialized);
    portal_json_raw(&j, ","); bool_field(&j, "busy", s.busy);
    portal_json_raw(&j, ","); string_field(&j, "asr", speech_provider_id(s.asr));
    portal_json_raw(&j, ","); string_field(&j, "tts", speech_provider_id(s.tts));
    portal_json_raw(&j, ",\"providers\":[");
    static const char *names[] = {"", "腾讯云", "阿里云", "豆包"};
    for (speech_provider_t provider = SPEECH_TENCENT; provider < SPEECH_PROVIDER_COUNT; ++provider)
    {
        const speech_settings_account_t *a = &s.accounts[provider];
        portal_json_raw(&j, provider == SPEECH_TENCENT ? "{" : ",{");
        string_field(&j, "id", speech_provider_id(provider));
        portal_json_raw(&j, ","); string_field(&j, "name", names[provider]);
        portal_json_raw(&j, ","); bool_field(&j, "configured", a->configured);
        portal_json_raw(&j, ","); bool_field(&j, "enabled", a->enabled);
        portal_json_raw(&j, ","); string_field(&j, "voice", a->voice);
        char rate[32]; snprintf(rate, sizeof(rate), ",\"rate\":%.2f", (double)a->rate);
        portal_json_raw(&j, rate);
        portal_json_raw(&j, ","); bool_field(&j, "asr", speech_provider_supports(provider, EPD_SPEECH_ASR));
        portal_json_raw(&j, ","); bool_field(&j, "tts", speech_provider_supports(provider, EPD_SPEECH_TTS));
        size_t count = 0; const speech_voice_t *voices = speech_settings_voices(provider, &count);
        portal_json_raw(&j, ",\"voices\":[");
        for (size_t i = 0; i < count; ++i)
        {
            portal_json_raw(&j, i ? ",{" : "{"); string_field(&j, "id", voices[i].id);
            portal_json_raw(&j, ","); string_field(&j, "name", voices[i].name_zh);
            portal_json_raw(&j, "}");
        }
        portal_json_raw(&j, "]}");
    }
    portal_json_raw(&j, "]}");
    if (j.failed) { epd_app_free(text); return 503; }
    *output = text; return 200;
}
int portal_service_config_read(bool speech, char **output)
{
    if (!output) return 400;
    *output = NULL;
    return speech ? speech_read(output) : ai_read(output);
}

static const ai_cjson *field(const ai_cjson *object, const char *name)
{ return ai_cjson_GetObjectItemCaseSensitive(object, name); }
static bool read_string(const ai_cjson *object, const char *name, char *out, size_t capacity, bool required)
{
    const ai_cjson *v = field(object, name);
    if (!v) return !required;
    if (!ai_cjson_IsString(v) || strlen(v->valuestring) >= capacity) return false;
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1); return true;
}
static bool read_bool(const ai_cjson *object, const char *name, bool *out)
{
    const ai_cjson *v = field(object, name);
    if (!ai_cjson_IsBool(v)) return false;
    *out = ai_cjson_IsTrue(v); return true;
}
static bool read_uint(const ai_cjson *object, const char *name, uint32_t max, uint32_t *out)
{
    const ai_cjson *v = field(object, name);
    if (!ai_cjson_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < 0 || v->valuedouble > max) return false;
    uint32_t n = (uint32_t)v->valuedouble;
    if ((double)n != v->valuedouble) return false;
    *out = n; return true;
}
static bool parse_profile(const ai_cjson *root, ai_settings_job_t *job)
{
    const ai_cjson *v = field(root, "profile");
    ai_profile_t *p = &job->profile;
    uint32_t number;
    if (!ai_cjson_IsObject(v) || !read_bool(root, "keep_secret", &job->keep_secret)) return false;
    p->public.struct_size = sizeof(p->public); p->public.version = EPD_AI_VERSION;
    if (!read_string(v,"id",p->public.id,sizeof(p->public.id),true) || !*p->public.id ||
        !read_string(v,"name",p->public.name,sizeof(p->public.name),true) ||
        !read_string(v,"endpoint",p->endpoint,sizeof(p->endpoint),true) ||
        !read_string(v,"secret",p->secret,sizeof(p->secret),false) ||
        !read_bool(v,"enabled",&p->public.enabled) || !read_bool(v,"is_default",&p->public.is_default) ||
        !read_uint(v,"protocol",2,&number)) return false;
    p->public.protocol = (epd_ai_protocol_t)number;
    if (!read_uint(v,"auth",1,&number)) return false;
    p->auth = (ai_auth_t)number;
    if (!read_uint(v,"features",7,&p->public.features) ||
        !read_uint(v,"connect_ms",900000,&p->connect_ms) ||
        !read_uint(v,"send_idle_ms",900000,&p->send_idle_ms) ||
        !read_uint(v,"first_event_ms",900000,&p->first_event_ms) ||
        !read_uint(v,"read_idle_ms",900000,&p->read_idle_ms) ||
        !read_uint(v,"total_ms",900000,&p->total_ms)) return false;
    if (job->keep_secret && *p->secret) return false;
    const ai_cjson *models = field(v, "models"), *headers = field(v, "headers");
    if (!ai_cjson_IsArray(models) || !ai_cjson_IsArray(headers)) return false;
    int count = ai_cjson_GetArraySize(models);
    if (count < 1 || count > EPD_AI_MAX_MODELS) return false;
    p->public.model_count = (uint8_t)count;
    for (int i = 0; i < count; ++i)
    {
        const ai_cjson *m = ai_cjson_GetArrayItem(models, i); epd_ai_model_t *out = &p->public.models[i];
        if (!ai_cjson_IsObject(m) || !read_string(m,"id",out->id,sizeof(out->id),true) ||
            !read_uint(m,"capabilities",127,&out->capabilities) ||
            !read_uint(m,"max_output_tokens",UINT32_MAX,&out->max_output_tokens) ||
            !read_uint(m,"token_field",1,&number)) return false;
        out->token_field = (epd_ai_token_field_t)number;
    }
    count = ai_cjson_GetArraySize(headers);
    if (count > AI_EXTRA_HEADERS) return false;
    p->header_count = (uint8_t)count;
    for (int i = 0; i < count; ++i)
    {
        const ai_cjson *h = ai_cjson_GetArrayItem(headers, i);
        if (!ai_cjson_IsObject(h) || !read_string(h,"name",p->headers[i].name,sizeof(p->headers[i].name),true) ||
            !read_string(h,"value",p->headers[i].value,sizeof(p->headers[i].value),true)) return false;
    }
    job->operation = AI_SETTINGS_SAVE_PROFILE; return true;
}
static bool parse_ai(const ai_cjson *root, ai_settings_job_t *job)
{
    char operation[16] = {0};
    if (!read_string(root,"operation",operation,sizeof(operation),true)) return false;
    if (!strcmp(operation,"save")) return parse_profile(root,job);
    if (!read_string(root,"profile_id",job->profile_id,sizeof(job->profile_id),true)) return false;
    if (!strcmp(operation,"default")) job->operation = AI_SETTINGS_DEFAULT;
    else if (!strcmp(operation,"remove")) job->operation = AI_SETTINGS_REMOVE;
    else if (!strcmp(operation,"grant")) job->operation = AI_SETTINGS_GRANT;
    else if (!strcmp(operation,"revoke")) job->operation = AI_SETTINGS_REVOKE;
    else return false;
    if (job->operation != AI_SETTINGS_DEFAULT && !*job->profile_id) return false;
    if (job->operation == AI_SETTINGS_GRANT || job->operation == AI_SETTINGS_REVOKE)
        return read_string(root,"app_id",job->app_id,sizeof(job->app_id),true) && *job->app_id;
    return true;
}
static bool parse_provider(const ai_cjson *root, speech_provider_t *out)
{
    char id[16] = {0};
    if (!read_string(root,"provider",id,sizeof(id),true)) return false;
    for (speech_provider_t p = SPEECH_NONE; p < SPEECH_PROVIDER_COUNT; ++p)
        if (!strcmp(id,speech_provider_id(p))) { *out = p; return true; }
    return false;
}
static bool parse_speech(const ai_cjson *root, speech_settings_job_t *job)
{
    char operation[16] = {0};
    if (!read_string(root,"operation",operation,sizeof(operation),true) ||
        !parse_provider(root,&job->provider)) return false;
    if (!strcmp(operation,"default"))
    {
        char kind[8] = {0};
        if (!read_string(root,"kind",kind,sizeof(kind),true)) return false;
        if (!strcmp(kind,"asr")) job->kind = EPD_SPEECH_ASR;
        else if (!strcmp(kind,"tts")) job->kind = EPD_SPEECH_TTS;
        else return false;
        if (job->provider != SPEECH_NONE && !speech_provider_supports(job->provider, job->kind)) return false;
        job->operation = SPEECH_SETTINGS_DEFAULT; return true;
    }
    if (job->provider == SPEECH_NONE) return false;
    if (!strcmp(operation,"remove")) { job->operation = SPEECH_SETTINGS_REMOVE; return true; }
    if (strcmp(operation,"save")) return false;
    speech_account_t *a = &job->account; a->provider = job->provider;
    if (!read_bool(root,"keep_credentials",&job->keep_credentials) || !read_bool(root,"enabled",&a->enabled) ||
        !read_string(root,"voice",a->voice,sizeof(a->voice),false) ||
        !read_string(root,"app_id",a->app_id,sizeof(a->app_id),false) ||
        !read_string(root,"secret_id",a->secret_id,sizeof(a->secret_id),false) ||
        !read_string(root,"secret",a->secret,sizeof(a->secret),false) ||
        !read_string(root,"workspace",a->workspace,sizeof(a->workspace),false)) return false;
    const ai_cjson *rate = field(root,"rate");
    double min = a->provider == SPEECH_TENCENT ? .6 : .5;
    if (rate && (!ai_cjson_IsNumber(rate) || !isfinite(rate->valuedouble) ||
        rate->valuedouble < 0 || rate->valuedouble > 2)) return false;
    a->rate = rate ? (float)rate->valuedouble : 0;
    if ((a->voice[0] || a->rate != 0) && a->rate < min) return false;
    job->operation = SPEECH_SETTINGS_SAVE_ACCOUNT; return true;
}
static void wipe_json(ai_cjson *value)
{
    for (; value; value = value->next)
    {
        if (value->valuestring) portal_service_config_wipe(value->valuestring, strlen(value->valuestring));
        if (value->child) wipe_json(value->child);
    }
}
static const char *error_message(bool speech, int error)
{
    if (speech)
    {
        switch ((epd_speech_error_t)error) {
        case EPD_SPEECH_INVALID: return "语音配置无效，请检查凭据、音色和语速。";
        case EPD_SPEECH_NOT_CONFIGURED: return "请先保存并启用对应的语音服务。";
        case EPD_SPEECH_UNSUPPORTED: return "所选服务不支持这项语音功能。";
        case EPD_SPEECH_STORAGE: return "语音配置保存失败，请检查内部存储空间。";
        case EPD_SPEECH_NOT_FOUND: return "语音配置任务不存在，请刷新后确认配置。";
        default: return "语音配置暂不可用，请稍后重试。";
        }
    }
    switch ((epd_ai_error_t)error) {
    case EPD_AI_INVALID: return "AI 配置无效，请检查地址、模型及高级参数。";
    case EPD_AI_NOT_FOUND: return "AI 配置或任务不存在，请刷新后确认。";
    case EPD_AI_STORAGE: return "AI 配置保存失败，请检查内部存储空间。";
    case EPD_AI_LIMIT: return "AI 配置已达到数量或容量上限。";
    case EPD_AI_UNAUTHORIZED: return "请先启用 AI 服务，再为应用授权。";
    default: return "AI 配置暂不可用，请稍后重试。";
    }
}
static int error_response(bool speech, int error, bool completed, char *output, size_t capacity)
{
    portal_json_t j; portal_json_init(&j,output,capacity);
    portal_json_raw(&j,completed ? "{\"state\":\"failed\"," : "{\"error\":\"configuration_failed\",");
    number_field(&j,"code",error); portal_json_raw(&j,",");
    string_field(&j,"message",error_message(speech,error)); portal_json_raw(&j,"}");
    if (j.failed) { if (capacity) *output = 0; return 500; }
    if (completed) return 200;
    if (error == (speech ? EPD_SPEECH_INVALID : EPD_AI_INVALID)) return 400;
    if (error == (speech ? EPD_SPEECH_NOT_FOUND : EPD_AI_NOT_FOUND)) return 404;
    if (error == (speech ? EPD_SPEECH_BUSY : EPD_AI_BUSY)) return 409;
    return 503;
}
int portal_service_config_submit(bool speech, const char *text, size_t length, char *output, size_t capacity)
{
    int invalid = speech ? EPD_SPEECH_INVALID : EPD_AI_INVALID;
    int unavailable = speech ? EPD_SPEECH_NO_MEMORY : EPD_AI_NO_MEMORY;
    if (!text || !length || length > PORTAL_SERVICE_CONFIG_MAX_BYTES)
        return error_response(speech,invalid,false,output,capacity);
    size_t bytes = speech ? sizeof(speech_settings_job_t) : sizeof(ai_settings_job_t);
    void *job = ai_calloc(1,bytes);
    if (!job) return error_response(speech,unavailable,false,output,capacity);
    int error; bool valid = false;
    if (!ai_json_begin()) { ai_free(job); return error_response(speech,unavailable,false,output,capacity); }
    epd_ai_error_t parse_error;
    ai_cjson *root = ai_json_parse(text,length,&parse_error);
    if (ai_cjson_IsObject(root)) valid = speech ? parse_speech(root,job) : parse_ai(root,job);
    wipe_json(root); ai_cjson_Delete(root); ai_json_end();
    uint32_t ticket = 0;
    if (!valid) error = parse_error == EPD_AI_NO_MEMORY ? unavailable : invalid;
    else error = speech ? (int)speech_settings_submit(job,&ticket) : (int)ai_settings_submit(job,&ticket);
    portal_service_config_wipe(job,bytes); ai_free(job);
    if (error) return error_response(speech,error,false,output,capacity);
    int n = snprintf(output,capacity,"{\"ticket\":%lu}",(unsigned long)ticket);
    return n >= 0 && (size_t)n < capacity ? 202 : 500;
}
int portal_service_config_poll(bool speech, uint32_t ticket, char *output, size_t capacity)
{
    int error;
    if (speech)
    {
        speech_settings_result_t result;
        error = speech_settings_poll(ticket,&result);
        if (error == EPD_SPEECH_OK && result.error) return error_response(true,result.error,true,output,capacity);
    }
    else
    {
        ai_settings_result_t result;
        error = ai_settings_poll(ticket,&result);
        if (error == EPD_AI_OK && result.error) return error_response(false,result.error,true,output,capacity);
    }
    const char *state;
    if (!error) state = "done";
    else if (error == (speech ? EPD_SPEECH_BUSY : EPD_AI_BUSY)) state = "pending";
    else return error_response(speech,error,false,output,capacity);
    int n = snprintf(output,capacity,"{\"state\":\"%s\"}",state);
    return n >= 0 && (size_t)n < capacity ? 200 : 500;
}
