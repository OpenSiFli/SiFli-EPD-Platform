/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_internal.h"
#include "ai_json.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <rthw.h>
#include <string.h>

#define AI_CONFIG_PATH STORAGE_FLASH_ROOT "/config/ai-services.json"
#define ALL_CAPS (EPD_AI_CAP_TEXT | EPD_AI_CAP_STREAM | EPD_AI_CAP_TOOLS | \
                  EPD_AI_CAP_JSON | EPD_AI_CAP_SCHEMA | EPD_AI_CAP_NATIVE | EPD_AI_CAP_WEB_SEARCH)
#define ALL_FEATURES (EPD_AI_FEATURE_OPENAI | EPD_AI_FEATURE_DEEPSEEK | \
                      EPD_AI_FEATURE_ENCRYPTED_REASONING)

static struct rt_mutex config_mutex;
static bool mutex_ready, writing;
static ai_config_t *published;

static bool config_lock(bool wait)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!mutex_ready)
        mutex_ready = rt_mutex_init(&config_mutex, "ai_cfg", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = mutex_ready;
    rt_hw_interrupt_enable(level);
    return ready && rt_mutex_take(&config_mutex, wait ? RT_WAITING_FOREVER : 0) == RT_EOK;
}

static bool lock_config(void) { return config_lock(true); }

static void unlock_config(void) { rt_mutex_release(&config_mutex); }

/* The writer reservation serializes transactions without holding a mutex
 * during validation, JSON processing, or storage operations. */
static epd_ai_error_t reserve_writer(void)
{
    if (!lock_config()) return EPD_AI_NO_MEMORY;
    epd_ai_error_t error = writing ? EPD_AI_BUSY : EPD_AI_OK;
    if (error == EPD_AI_OK) writing = true;
    unlock_config();
    return error;
}

static void release_writer(void)
{
    if (lock_config()) { writing = false; unlock_config(); }
}

static bool bounded_text(const char *s, size_t cap, bool empty)
{
    size_t length = 0;
    while (length < cap && s[length]) ++length;
    if (length == cap || (!empty && !length)) return false;
    const char *end = s + length;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return ai_json_utf8(s, (size_t)(end - s)) != 0;
}

static bool identifier(const char *s, size_t cap)
{
    if (!bounded_text(s, cap, false)) return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.'))
            return false;
    return true;
}

static bool equal_fold(const char *a, const char *b)
{
    while (*a && *b)
    {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

static int profile_index(const ai_config_t *c, const char *id)
{
    for (size_t i = 0; i < c->profile_count; ++i)
        if (!strcmp(c->profiles[i].public.id, id)) return (int)i;
    return -1;
}

static bool granted(const ai_config_t *c, const char *app, const char *id)
{
    for (size_t i = 0; i < c->grant_count; ++i)
        if (!strcmp(c->grants[i].app, app) && !strcmp(c->grants[i].profile, id)) return true;
    return false;
}

static epd_ai_error_t validate(ai_config_t *c)
{
    if (c->version != EPD_AI_VERSION) return EPD_AI_UNSUPPORTED;
    if (c->profile_count > EPD_AI_MAX_PROFILES || c->grant_count > AI_MAX_GRANTS)
        return EPD_AI_LIMIT;
    if (!bounded_text(c->default_id, sizeof(c->default_id), true)) return EPD_AI_INVALID;
    for (size_t i = 0; i < c->profile_count; ++i)
    {
        ai_profile_t *p = &c->profiles[i];
        epd_ai_profile_t *v = &p->public;
        if (v->struct_size != sizeof(*v) || v->version != EPD_AI_VERSION ||
            !identifier(v->id, sizeof(v->id)) ||
            !bounded_text(v->name, sizeof(v->name), false) ||
            !bounded_text(p->endpoint, sizeof(p->endpoint), false) ||
            !bounded_text(p->secret, sizeof(p->secret), true) ||
            (unsigned)v->protocol > EPD_AI_MESSAGES || (unsigned)p->auth > AI_AUTH_API_KEY ||
            (v->features & ~ALL_FEATURES)) return EPD_AI_INVALID;
        if ((v->features & EPD_AI_FEATURE_DEEPSEEK) && v->protocol == EPD_AI_MESSAGES)
            return EPD_AI_INVALID;
        if ((v->features & EPD_AI_FEATURE_DEEPSEEK) && (v->features & EPD_AI_FEATURE_OPENAI))
            return EPD_AI_INVALID;
        if ((v->features & EPD_AI_FEATURE_ENCRYPTED_REASONING) && v->protocol != EPD_AI_RESPONSES)
            return EPD_AI_INVALID;
        ai_endpoint_t endpoint;
        epd_ai_error_t error = ai_endpoint_parse(p->endpoint, &endpoint);
        if (error != EPD_AI_OK) return error;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(v->id, c->profiles[j].public.id)) return EPD_AI_INVALID;
        if (!v->model_count || v->model_count > EPD_AI_MAX_MODELS || p->header_count > AI_EXTRA_HEADERS)
            return EPD_AI_LIMIT;
        for (size_t j = 0; j < v->model_count; ++j)
        {
            epd_ai_model_t *m = &v->models[j];
            if (!bounded_text(m->id, sizeof(m->id), false) || !m->max_output_tokens ||
                !(m->capabilities & EPD_AI_CAP_TEXT) || (m->capabilities & ~ALL_CAPS) ||
                (unsigned)m->token_field > EPD_AI_MAX_COMPLETION_TOKENS) return EPD_AI_INVALID;
            if ((m->capabilities & EPD_AI_CAP_WEB_SEARCH) &&
                (ai_model_search_support(v, m) != AI_SEARCH_SUPPORTED ||
                 !(m->capabilities & EPD_AI_CAP_NATIVE)))
                return EPD_AI_UNSUPPORTED;
            for (size_t k = 0; k < j; ++k)
                if (!strcmp(m->id, v->models[k].id)) return EPD_AI_INVALID;
        }
        bool version_header = false;
        for (size_t j = 0; j < p->header_count; ++j)
        {
            ai_header_t *h = &p->headers[j];
            if (!bounded_text(h->name, sizeof(h->name), false) ||
                !bounded_text(h->value, sizeof(h->value), false)) return EPD_AI_INVALID;
            bool anthropic_version = equal_fold(h->name, "anthropic-version");
            bool anthropic_beta = equal_fold(h->name, "anthropic-beta");
            bool openai = equal_fold(h->name, "OpenAI-Organization") ||
                          equal_fold(h->name, "OpenAI-Project") || equal_fold(h->name, "OpenAI-Beta");
            if (!anthropic_version && !anthropic_beta && !openai) return EPD_AI_INVALID;
            if ((anthropic_version || anthropic_beta) && v->protocol != EPD_AI_MESSAGES)
                return EPD_AI_INVALID;
            if (openai && v->protocol == EPD_AI_MESSAGES) return EPD_AI_INVALID;
            if (anthropic_version)
            {
                if (strlen(h->value) != 10 || h->value[4] != '-' || h->value[7] != '-')
                    return EPD_AI_INVALID;
                for (size_t k = 0; k < 10; ++k)
                    if (k != 4 && k != 7 && (h->value[k] < '0' || h->value[k] > '9'))
                        return EPD_AI_INVALID;
                unsigned month = (unsigned)(h->value[5] - '0') * 10u + (unsigned)(h->value[6] - '0');
                unsigned day = (unsigned)(h->value[8] - '0') * 10u + (unsigned)(h->value[9] - '0');
                if (!month || month > 12 || !day || day > 31) return EPD_AI_INVALID;
            }
            version_header |= anthropic_version;
            for (size_t k = 0; k < j; ++k)
                if (equal_fold(h->name, p->headers[k].name)) return EPD_AI_INVALID;
        }
        if (v->protocol == EPD_AI_MESSAGES && !version_header) return EPD_AI_INVALID;
        if (!p->connect_ms) p->connect_ms = 30000;
        if (!p->send_idle_ms) p->send_idle_ms = 30000;
        if (!p->first_event_ms) p->first_event_ms = 90000;
        if (!p->read_idle_ms) p->read_idle_ms = 60000;
        if (!p->total_ms) p->total_ms = 300000;
        if (p->total_ms > 900000 || p->connect_ms > p->total_ms ||
            p->send_idle_ms > p->total_ms || p->first_event_ms > p->total_ms ||
            p->read_idle_ms > p->total_ms) return EPD_AI_INVALID;
        v->is_default = !strcmp(c->default_id, v->id);
    }
    if (*c->default_id && profile_index(c, c->default_id) < 0) return EPD_AI_INVALID;
    for (size_t i = 0; i < c->grant_count; ++i)
    {
        if (!identifier(c->grants[i].app, sizeof(c->grants[i].app)) ||
            !identifier(c->grants[i].profile, sizeof(c->grants[i].profile)) ||
            profile_index(c, c->grants[i].profile) < 0) return EPD_AI_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(c->grants[i].app, c->grants[j].app) &&
                !strcmp(c->grants[i].profile, c->grants[j].profile)) return EPD_AI_INVALID;
    }
    return EPD_AI_OK;
}

static const ai_cjson *field(const ai_cjson *o, const char *key)
{ return ai_cjson_GetObjectItemCaseSensitive(o, key); }

static bool keys(const ai_cjson *o, const char *allowed)
{
    if (!ai_cjson_IsObject(o)) return false;
    for (const ai_cjson *n = o->child; n; n = n->next)
    {
        if (!n->string) return false;
        size_t len = strlen(n->string);
        const char *p = allowed;
        bool found = false;
        while (*p)
        {
            const char *end = strchr(p, '|');
            size_t size = end ? (size_t)(end - p) : strlen(p);
            if (size == len && !memcmp(n->string, p, len)) { found = true; break; }
            p += size;
            if (*p) ++p;
        }
        if (!found) return false;
    }
    return true;
}

static bool number(const ai_cjson *o, const char *key, uint32_t *out, uint32_t fallback)
{
    const ai_cjson *n = field(o, key);
    if (!n) { *out = fallback; return true; }
    if (!ai_cjson_IsNumber(n) || !(n->valuedouble >= 0 && n->valuedouble <= UINT32_MAX)) return false;
    *out = (uint32_t)n->valuedouble;
    return n->valuedouble == *out;
}

static bool string(const ai_cjson *o, const char *key, char *out, size_t size, bool optional)
{
    const ai_cjson *n = field(o, key);
    if (!n) { *out = 0; return optional; }
    if (!ai_cjson_IsString(n) || !n->valuestring || strlen(n->valuestring) >= size) return false;
    strcpy(out, n->valuestring);
    return true;
}

static bool decode_profile(const ai_cjson *o, ai_profile_t *p)
{
    epd_ai_profile_t *v = &p->public;
    uint32_t protocol, auth;
    if (!keys(o, "id|name|protocol|identity_revision|features|enabled|endpoint|secret|auth|models|headers|connect_ms|send_idle_ms|first_event_ms|read_idle_ms|total_ms") ||
        !string(o, "id", v->id, sizeof(v->id), false) ||
        !string(o, "name", v->name, sizeof(v->name), false) ||
        !string(o, "endpoint", p->endpoint, sizeof(p->endpoint), false) ||
        !string(o, "secret", p->secret, sizeof(p->secret), true) ||
        !number(o, "protocol", &protocol, UINT32_MAX) || protocol > EPD_AI_MESSAGES ||
        !number(o, "auth", &auth, AI_AUTH_BEARER) || auth > AI_AUTH_API_KEY ||
        !number(o, "features", &v->features, 0) ||
        !number(o, "identity_revision", &v->identity_revision, 1) ||
        !number(o, "connect_ms", &p->connect_ms, 30000) ||
        !number(o, "send_idle_ms", &p->send_idle_ms, 30000) ||
        !number(o, "first_event_ms", &p->first_event_ms, 90000) ||
        !number(o, "read_idle_ms", &p->read_idle_ms, 60000) ||
        !number(o, "total_ms", &p->total_ms, 300000)) return false;
    const ai_cjson *enabled = field(o, "enabled");
    if (!ai_cjson_IsBool(enabled)) return false;
    v->enabled = ai_cjson_IsTrue(enabled) != 0;
    v->struct_size = sizeof(*v); v->version = EPD_AI_VERSION;
    v->protocol = (epd_ai_protocol_t)protocol; p->auth = (ai_auth_t)auth;
    const ai_cjson *models = field(o, "models"), *headers = field(o, "headers");
    if (!ai_cjson_IsArray(models) || ai_cjson_GetArraySize(models) > (int)EPD_AI_MAX_MODELS ||
        (headers && (!ai_cjson_IsArray(headers) || ai_cjson_GetArraySize(headers) > (int)AI_EXTRA_HEADERS)))
        return false;
    for (const ai_cjson *m = models->child; m; m = m->next)
    {
        epd_ai_model_t *model = &v->models[v->model_count++];
        uint32_t token;
        if (!keys(m, "id|capabilities|max_output_tokens|token_field") ||
            !string(m, "id", model->id, sizeof(model->id), false) ||
            !number(m, "capabilities", &model->capabilities, EPD_AI_CAP_TEXT) ||
            !number(m, "max_output_tokens", &model->max_output_tokens, 0) ||
            !number(m, "token_field", &token, EPD_AI_MAX_TOKENS) || token > EPD_AI_MAX_COMPLETION_TOKENS)
            return false;
        model->token_field = (epd_ai_token_field_t)token;
    }
    for (const ai_cjson *h = headers ? headers->child : NULL; h; h = h->next)
    {
        ai_header_t *header = &p->headers[p->header_count++];
        if (!keys(h, "name|value") || !string(h, "name", header->name, sizeof(header->name), false) ||
            !string(h, "value", header->value, sizeof(header->value), false)) return false;
    }
    return true;
}

static epd_ai_error_t decode(const char *text, size_t size, ai_config_t *c)
{
    if (!ai_json_begin()) return EPD_AI_NO_MEMORY;
    epd_ai_error_t error = EPD_AI_INVALID;
    ai_cjson *root = ai_json_parse(text, size, &error);
    if (!root) { ai_json_end(); return error; }
    error = EPD_AI_INVALID;
    const ai_cjson *profiles = field(root, "profiles"), *grants = field(root, "grants");
    if (!keys(root, "version|generation|default_id|profiles|grants") ||
        !number(root, "version", &c->version, 0) ||
        !number(root, "generation", &c->generation, 1) ||
        !string(root, "default_id", c->default_id, sizeof(c->default_id), true)) goto done;
    if (c->version != EPD_AI_VERSION) { error = EPD_AI_UNSUPPORTED; goto done; }
    if (!ai_cjson_IsArray(profiles) || (grants && !ai_cjson_IsArray(grants))) goto done;
    if (ai_cjson_GetArraySize(profiles) > (int)EPD_AI_MAX_PROFILES ||
        (grants && ai_cjson_GetArraySize(grants) > (int)AI_MAX_GRANTS)) { error = EPD_AI_LIMIT; goto done; }
    for (const ai_cjson *p = profiles->child; p; p = p->next)
        if (!decode_profile(p, &c->profiles[c->profile_count++])) goto done;
    for (const ai_cjson *g = grants ? grants->child : NULL; g; g = g->next)
    {
        ai_grant_t *grant = &c->grants[c->grant_count++];
        if (!keys(g, "app|profile") || !string(g, "app", grant->app, sizeof(grant->app), false) ||
            !string(g, "profile", grant->profile, sizeof(grant->profile), false)) goto done;
    }
    error = EPD_AI_OK;
done:
    ai_cjson_Delete(root);
    ai_json_end();
    return error == EPD_AI_OK ? validate(c) : error;
}

static bool add(ai_cjson *o, const char *key, ai_cjson *n)
{
    if (!n) return false;
    if (key ? ai_cjson_AddItemToObject(o, key, n) : ai_cjson_AddItemToArray(o, n)) return true;
    ai_cjson_Delete(n);
    return false;
}
#define NUM(o, k, n) add((o), (k), ai_cjson_CreateNumber((double)(n)))
#define STR(o, k, s) add((o), (k), ai_cjson_CreateString(s))

static bool encode_profile(ai_cjson *o, const ai_profile_t *p, bool redacted)
{
    const epd_ai_profile_t *v = &p->public;
    if (!STR(o, "id", v->id) || !STR(o, "name", v->name) || !NUM(o, "protocol", v->protocol) ||
        !NUM(o, "identity_revision", v->identity_revision) || !NUM(o, "features", v->features) ||
        !add(o, "enabled", ai_cjson_CreateBool(v->enabled)) || !STR(o, "endpoint", p->endpoint) ||
        !STR(o, "secret", redacted ? "" : p->secret) || !NUM(o, "auth", p->auth) ||
        !NUM(o, "connect_ms", p->connect_ms) || !NUM(o, "send_idle_ms", p->send_idle_ms) ||
        !NUM(o, "first_event_ms", p->first_event_ms) || !NUM(o, "read_idle_ms", p->read_idle_ms) ||
        !NUM(o, "total_ms", p->total_ms)) return false;
    ai_cjson *models = ai_cjson_CreateArray();
    if (!add(o, "models", models)) return false;
    for (size_t i = 0; i < v->model_count; ++i)
    {
        ai_cjson *m = ai_cjson_CreateObject();
        if (!add(models, NULL, m)) return false;
        if (!STR(m, "id", v->models[i].id) || !NUM(m, "capabilities", v->models[i].capabilities) ||
            !NUM(m, "max_output_tokens", v->models[i].max_output_tokens) ||
            !NUM(m, "token_field", v->models[i].token_field)) return false;
    }
    ai_cjson *headers = ai_cjson_CreateArray();
    if (!add(o, "headers", headers)) return false;
    for (size_t i = 0; i < p->header_count; ++i)
    {
        ai_cjson *h = ai_cjson_CreateObject();
        if (!add(headers, NULL, h)) return false;
        if (!STR(h, "name", p->headers[i].name) || !STR(h, "value", p->headers[i].value)) return false;
    }
    return true;
}

static epd_ai_error_t encode(const ai_config_t *c, bool redacted, char **text, size_t *size)
{
    *text = NULL; *size = 0;
    if (!ai_json_begin()) return EPD_AI_NO_MEMORY;
    epd_ai_error_t error = EPD_AI_NO_MEMORY;
    ai_cjson *root = ai_cjson_CreateObject();
    if (!root || !NUM(root, "version", c->version) || !NUM(root, "generation", c->generation) ||
        !STR(root, "default_id", c->default_id)) goto done;
    ai_cjson *profiles = ai_cjson_CreateArray();
    if (!add(root, "profiles", profiles)) goto done;
    for (size_t i = 0; i < c->profile_count; ++i)
    {
        ai_cjson *p = ai_cjson_CreateObject();
        if (!add(profiles, NULL, p) || !encode_profile(p, &c->profiles[i], redacted)) goto done;
    }
    ai_cjson *grants = ai_cjson_CreateArray();
    if (!add(root, "grants", grants)) goto done;
    for (size_t i = 0; !redacted && i < c->grant_count; ++i)
    {
        ai_cjson *g = ai_cjson_CreateObject();
        if (!add(grants, NULL, g) || !STR(g, "app", c->grants[i].app) ||
            !STR(g, "profile", c->grants[i].profile)) goto done;
    }
    *text = ai_json_print(root, &error);
    if (*text)
    {
        *size = strlen(*text);
        if (*size > AI_CONFIG_LIMIT) { ai_free(*text); *text = NULL; error = EPD_AI_LIMIT; }
        else error = EPD_AI_OK;
    }
done:
    ai_cjson_Delete(root);
    ai_json_end();
    return error;
}

static bool transfer_path(const char *path)
{
    if (!path || !bounded_text(path, STORAGE_PATH_MAX, false) ||
        strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT))) return false;
    const char *start = path + sizeof(STORAGE_SD_ROOT);
    for (const char *p = start; ; ++p)
    {
        if (*p == '\\' || *p == ':') return false;
        if (!*p || *p == '/')
        {
            size_t n = (size_t)(p - start);
            if (!n || (n == 1 && *start == '.') || (n == 2 && start[0] == '.' && start[1] == '.'))
                return false;
            start = p + 1;
        }
        if (!*p) return true;
    }
}

static epd_ai_error_t read_document(const char *path, bool missing_ok, ai_config_t *c, uint32_t session)
{
    epd_ai_error_t error = EPD_AI_STORAGE;
    char *data = NULL;
    int fd = -1;
    size_t size = 0, used = 0;
    storage_lock();
    if (!missing_ok && !storage_session_valid(STORAGE_SD, session))
    { error = EPD_AI_CANCELLED; goto done; }
    if (!storage_path_available(path) || !storage_file_recover(path)) goto done;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        if (missing_ok && (errno == ENOENT || errno == -ENOENT))
        { c->version = EPD_AI_VERSION; error = EPD_AI_OK; }
        goto done;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) goto done;
    if ((uint64_t)st.st_size > AI_CONFIG_LIMIT) { error = EPD_AI_LIMIT; goto done; }
    size = (size_t)st.st_size;
    data = ai_alloc(size + 1);
    if (!data) { error = EPD_AI_NO_MEMORY; goto done; }
    while (used < size)
    {
        int n = read(fd, data + used, size - used);
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    char tail;
    if (read(fd, &tail, 1) != 0 || !storage_path_available(path)) goto done;
    if (!missing_ok && !storage_session_valid(STORAGE_SD, session))
    { error = EPD_AI_CANCELLED; goto done; }
    data[size] = 0;
    error = EPD_AI_OK;
done:
    if (fd >= 0 && close(fd) != 0) error = EPD_AI_STORAGE;
    storage_unlock();
    if (error == EPD_AI_OK && data) error = decode(data, size, c);
    ai_free(data);
    return error;
}

epd_ai_error_t ai_config_init(void)
{
    epd_ai_error_t error = reserve_writer();
    if (error != EPD_AI_OK) return error;
    if (published) { release_writer(); return EPD_AI_OK; }
    ai_config_t *next = ai_calloc(1, sizeof(*next));
    if (!next) { release_writer(); return EPD_AI_NO_MEMORY; }
    error = read_document(AI_CONFIG_PATH, true, next, 0);
    if (error == EPD_AI_OK)
    {
        for (size_t i = 0; i < next->profile_count; ++i)
            if (!next->profiles[i].public.identity_revision ||
                next->profiles[i].public.identity_revision > next->generation) error = EPD_AI_INVALID;
    }
    if (error == EPD_AI_OK && lock_config())
    { published = next; next = NULL; unlock_config(); }
    else if (error == EPD_AI_OK) error = EPD_AI_NO_MEMORY;
    ai_free(next);
    release_writer();
    return error;
}

static epd_ai_error_t snapshot(ai_config_t **out, bool wait)
{
    if (!out) return EPD_AI_INVALID;
    *out = NULL;
    ai_config_t *copy = ai_alloc(sizeof(*copy));
    if (!copy) return EPD_AI_NO_MEMORY;
    if (!config_lock(wait)) { ai_free(copy); return EPD_AI_BUSY; }
    bool ready = published != NULL;
    if (ready) memcpy(copy, published, sizeof(*copy));
    unlock_config();
    if (!ready) { ai_free(copy); return EPD_AI_STORAGE; }
    *out = copy;
    return EPD_AI_OK;
}

epd_ai_error_t ai_config_snapshot(ai_config_t **out) { return snapshot(out, false); }

epd_ai_error_t ai_config_acquire(const char *app, const char *id, ai_profile_t **out)
{
    if (!out || !app || !id || !*id) return EPD_AI_INVALID;
    *out = NULL;
    ai_profile_t *copy = ai_alloc(sizeof(*copy));
    if (!copy) return EPD_AI_NO_MEMORY;
    if (!config_lock(false)) { ai_free(copy); return EPD_AI_BUSY; }
    epd_ai_error_t error = EPD_AI_STORAGE;
    if (published)
    {
        int index = profile_index(published, id);
        error = !granted(published, app, id) ? EPD_AI_UNAUTHORIZED :
                index < 0 ? EPD_AI_NOT_FOUND :
                !published->profiles[index].public.enabled ? EPD_AI_UNAUTHORIZED : EPD_AI_OK;
        if (error == EPD_AI_OK) memcpy(copy, &published->profiles[index], sizeof(*copy));
    }
    unlock_config();
    if (error != EPD_AI_OK) ai_free(copy); else *out = copy;
    return error;
}

bool ai_config_current(const char *app, const ai_profile_t *profile)
{
    if (!app || !profile || !lock_config()) return false;
    int index = published ? profile_index(published, profile->public.id) : -1;
    bool current = index >= 0 && published->profiles[index].public.enabled &&
        published->profiles[index].public.identity_revision == profile->public.identity_revision &&
        granted(published, app, profile->public.id);
    unlock_config();
    return current;
}

epd_ai_error_t ai_config_list(const char *app, epd_ai_profile_t *out, size_t capacity, size_t *count)
{
    if (!app || !count || (capacity && !out)) return EPD_AI_INVALID;
    *count = 0;
    if (!config_lock(false)) return EPD_AI_BUSY;
    if (!published) { unlock_config(); return EPD_AI_STORAGE; }
    for (size_t i = 0; i < published->profile_count; ++i)
        if (granted(published, app, published->profiles[i].public.id))
        {
            if (*count < capacity) out[*count] = published->profiles[i].public;
            ++*count;
        }
    unlock_config();
    return capacity && *count > capacity ? EPD_AI_LIMIT : EPD_AI_OK;
}

static bool identity_equal(const ai_profile_t *a, const ai_profile_t *b)
{
    if (a->public.protocol != b->public.protocol || a->auth != b->auth ||
        a->public.enabled != b->public.enabled ||
        a->public.features != b->public.features || strcmp(a->endpoint, b->endpoint) ||
        strcmp(a->secret, b->secret) || a->header_count != b->header_count ||
        a->public.model_count != b->public.model_count) return false;
    for (size_t i = 0; i < a->header_count; ++i)
        if (!equal_fold(a->headers[i].name, b->headers[i].name) ||
            strcmp(a->headers[i].value, b->headers[i].value)) return false;
    for (size_t i = 0; i < a->public.model_count; ++i)
    {
        const epd_ai_model_t *x = &a->public.models[i], *y = &b->public.models[i];
        if (strcmp(x->id, y->id) || x->capabilities != y->capabilities ||
            x->max_output_tokens != y->max_output_tokens || x->token_field != y->token_field) return false;
    }
    return true;
}

/* Caller owns the writer reservation and next. Success transfers next into
 * published; all allocations, encoding and validation precede file commit. */
static epd_ai_error_t commit_owned(ai_config_t *next, bool importing, uint32_t session)
{
    if (!published && !importing) return EPD_AI_STORAGE;
    uint32_t generation = published ? published->generation : 0;
    if (next->generation != generation) return EPD_AI_BUSY;
    if (generation == UINT32_MAX) return EPD_AI_LIMIT;
    epd_ai_error_t error = validate(next);
    if (error != EPD_AI_OK) return error;
    next->generation = generation + 1;
    for (size_t i = 0; i < next->profile_count; ++i)
    {
        int old = published ? profile_index(published, next->profiles[i].public.id) : -1;
        next->profiles[i].public.identity_revision = old >= 0 &&
            identity_equal(&published->profiles[old], &next->profiles[i]) ?
            published->profiles[old].public.identity_revision : next->generation;
    }
    char *text = NULL; size_t size = 0;
    error = encode(next, false, &text, &size);
    if (error != EPD_AI_OK) return error;
    /* Keep replacement media from being mounted between the session check and
     * the durable commit. storage_file_replace uses the same recursive lock. */
    if (importing) storage_lock();
    bool current = !importing || storage_session_valid(STORAGE_SD, session);
    bool saved = current && storage_file_replace(AI_CONFIG_PATH, text, size);
    if (importing) storage_unlock();
    ai_free(text);
    if (!current) return EPD_AI_CANCELLED;
    if (!saved) return EPD_AI_STORAGE;
    /* An initialized RT-Thread mutex with an infinite wait cannot fail under
     * normal execution; no allocation or validation remains after replace. */
    rt_mutex_take(&config_mutex, RT_WAITING_FOREVER);
    ai_config_t *old = published;
    published = next;
    if (old) ai_config_changed(old, next);
    unlock_config();
    ai_free(old);
    return EPD_AI_OK;
}

epd_ai_error_t ai_config_commit(const ai_config_t *candidate)
{
    if (!candidate) return EPD_AI_INVALID;
    epd_ai_error_t error = reserve_writer();
    if (error != EPD_AI_OK) return error;
    ai_config_t *next = ai_alloc(sizeof(*next));
    if (next) { memcpy(next, candidate, sizeof(*next)); error = commit_owned(next, false, 0); }
    else error = EPD_AI_NO_MEMORY;
    if (error != EPD_AI_OK) ai_free(next);
    release_writer();
    return error;
}

epd_ai_error_t ai_config_import_prepare(const char *path, uint32_t session, ai_config_t **out)
{
    if (!out) return EPD_AI_INVALID;
    *out = NULL;
    if (!transfer_path(path)) return EPD_AI_INVALID;
    epd_ai_error_t error = reserve_writer();
    if (error != EPD_AI_OK) return error;
    ai_config_t *next = ai_calloc(1, sizeof(*next));
    if (!next) error = EPD_AI_NO_MEMORY;
    else
    {
        error = read_document(path, false, next, session);
        if (error == EPD_AI_OK)
        { next->generation = published ? published->generation : 0; *out = next; }
    }
    if (error != EPD_AI_OK) ai_free(next);
    release_writer();
    return error;
}

epd_ai_error_t ai_config_import_commit(const ai_config_t *candidate, uint32_t session)
{
    if (!candidate) return EPD_AI_INVALID;
    epd_ai_error_t error = reserve_writer();
    if (error != EPD_AI_OK) return error;
    ai_config_t *next = ai_alloc(sizeof(*next));
    if (next) { memcpy(next, candidate, sizeof(*next)); error = commit_owned(next, true, session); }
    else error = EPD_AI_NO_MEMORY;
    if (error != EPD_AI_OK) ai_free(next);
    release_writer();
    return error;
}

epd_ai_error_t ai_config_export(const char *path, uint32_t session)
{
    if (!transfer_path(path)) return EPD_AI_INVALID;
    ai_config_t *copy = NULL;
    epd_ai_error_t error = ai_config_snapshot(&copy);
    if (error != EPD_AI_OK) return error;
    char *text = NULL; size_t size = 0;
    error = encode(copy, true, &text, &size);
    ai_free(copy);
    if (error == EPD_AI_OK)
    {
        storage_lock();
        if (!storage_session_valid(STORAGE_SD, session)) error = EPD_AI_CANCELLED;
        else if (!storage_file_replace(path, text, size)) error = EPD_AI_STORAGE;
        storage_unlock();
    }
    ai_free(text);
    return error;
}

epd_ai_error_t ai_config_revoke_app(const char *app)
{
    if (!app || !identifier(app, 32)) return EPD_AI_INVALID;
    epd_ai_error_t error = reserve_writer();
    if (error != EPD_AI_OK) return error;
    ai_config_t *next = NULL;
    error = snapshot(&next, true);
    bool changed = false;
    if (error == EPD_AI_OK)
    {
        size_t kept = 0;
        for (size_t i = 0; i < next->grant_count; ++i)
            if (strcmp(next->grants[i].app, app)) next->grants[kept++] = next->grants[i];
        changed = kept != next->grant_count;
        next->grant_count = kept;
        if (changed) error = commit_owned(next, false, 0);
    }
    if (!changed || error != EPD_AI_OK) ai_free(next);
    release_writer();
    return error;
}
