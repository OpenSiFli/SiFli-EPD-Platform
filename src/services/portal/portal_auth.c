#include "storage_internal.h"
#include "portal_auth.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "sleep_guard.h"
#include "boards/board_service.h"
#include "bf0_hal.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <dfs_posix.h>
#include <rthw.h>
#include <errno.h>
#include <string.h>

#define AUTH_PATH STORAGE_FLASH_ROOT "/system/portal_accounts.bin"
#define AUTH_MAGIC 0x50414343u
#define AUTH_VERSION 1u
#define AUTH_ITERATIONS 1000u
#define SESSION_COUNT 16
#define SESSION_IDLE_SECONDS 1800u
#define SESSION_ABSOLUTE_SECONDS 28800u
#define RATE_IP_COUNT 16
#define RATE_SECONDS 60u
#define RATE_GLOBAL 10u
#define RATE_IP 5u

typedef struct
{
    uint32_t id, iterations;
    char username[PORTAL_AUTH_USERNAME_MAX + 1];
    unsigned char salt[16], hash[32];
} account_t;
typedef struct { uint32_t count, next_id; account_t accounts[PORTAL_AUTH_MAX_ACCOUNTS]; } record_t;
typedef struct
{
    uint32_t id;
    rt_tick_t created, used;
    char token[PORTAL_AUTH_TOKEN_LENGTH + 1], csrf[PORTAL_AUTH_TOKEN_LENGTH + 1];
} session_t;
typedef struct { bool active; uint32_t peer; rt_tick_t start; unsigned count; } rate_t;
typedef enum { JOB_ADD, JOB_PASSWORD, JOB_DELETE } job_kind_t;
typedef struct
{
    job_kind_t kind;
    uint32_t id;
    char username[PORTAL_AUTH_USERNAME_MAX + 1], password[PORTAL_AUTH_PASSWORD_MAX + 1];
} job_input_t;

static struct rt_mutex lock;
static struct rt_mutex rng_lock;
static bool rng_ready, launching, storage_blocked;
static sleep_guard_owner_t init_hold = {.id = "portal-auth-init"};
static sleep_guard_owner_t mutation_hold = {.id = "portal-auth-save"};
static struct rt_semaphore wake;
static bool starting, initialized, mutating;
static portal_auth_error_t store_error = PORTAL_AUTH_UNAVAILABLE;
L2_RET_BSS_SECT_BEGIN(portal_accounts)
static record_t record L2_RET_BSS_SECT(portal_accounts);
static session_t sessions[SESSION_COUNT] L2_RET_BSS_SECT(portal_accounts);
L2_RET_BSS_SECT_END

/* These retained buffers are not cleared by C startup. Never reuse a login
 * session from the previous boot; account records are loaded from storage. */
static int portal_auth_buffers_init(void)
{
    memset(&record, 0, sizeof(record));
    memset(sessions, 0, sizeof(sessions));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(portal_auth_buffers_init);

static rate_t global_rate, ip_rates[RATE_IP_COUNT];
static portal_auth_snapshot_t published;
static portal_auth_job_t job_result;
static job_input_t job_input;
static uint32_t ticket_serial, session_epoch, setup_epoch;
static bool setup_open;
static rt_tick_t setup_started;
static char setup_code[PORTAL_AUTH_CODE_LENGTH + 1];

static void erase(void *data, size_t length)
{ volatile unsigned char *p = data; while (length--) *p++ = 0; }
static bool equal(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b; unsigned diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}
static rt_tick_t ticks(unsigned seconds)
{ return (rt_tick_t)seconds * RT_TICK_PER_SECOND; }
static bool elapsed(rt_tick_t start, unsigned seconds)
{ return (rt_tick_t)(rt_tick_get() - start) >= ticks(seconds); }
static bool take(bool wait)
{ return initialized && rt_mutex_take(&lock, wait ? RT_WAITING_FOREVER : 0) == RT_EOK; }
static void release(void) { rt_mutex_release(&lock); }
static bool username_valid(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s); if (n < 3 || n > PORTAL_AUTH_USERNAME_MAX) return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '-')) return false;
    return true;
}
static bool password_valid(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s); if (n < 8 || n > PORTAL_AUTH_PASSWORD_MAX) return false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        unsigned c = *p++, need, minimum;
        if (c < 0x80) { if (c < 32 || c == 127) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; minimum = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; minimum = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; minimum = 0x10000; c &= 7; }
        else return false;
        while (need--) { if ((*p & 0xc0) != 0x80) return false; c = (c << 6) | (*p++ & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
/* All hardware RNG calls are serialized by lock, independently of hashing. */
static bool hardware_random(void *out, size_t length)
{
    RNG_HandleTypeDef rng = {0}; rng.Instance = TRNG;
    if (HAL_RNG_Init(&rng) != HAL_OK) return false;
    unsigned char *p = out; bool ok = true;
    while (length && ok)
    {
        uint32_t seed = 0, value = 0;
        ok = HAL_RNG_Generate(&rng, &seed, 1) == HAL_OK && HAL_RNG_Generate(&rng, &value, 0) == HAL_OK;
        size_t n = length < sizeof(value) ? length : sizeof(value);
        if (ok) { memcpy(p, &value, n); p += n; length -= n; }
        erase(&seed, sizeof(seed)); erase(&value, sizeof(value));
    }
    HAL_RNG_DeInit(&rng); return ok;
}
bool portal_auth_random_bytes(void *out, size_t length)
{
    if (!out || !length) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!rng_ready) rng_ready = rt_mutex_init(&rng_lock, "authrng", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = rng_ready; rt_hw_interrupt_enable(level);
    if (!ready || rt_mutex_take(&rng_lock, RT_WAITING_FOREVER) != RT_EOK) return false;
    bool ok = hardware_random(out, length); rt_mutex_release(&rng_lock);
    if (!ok) erase(out, length);
    return ok;
}
static bool random_bytes(void *out, size_t length) { return portal_auth_random_bytes(out, length); }
static bool blocked(void)
{ return storage_blocked || board_power_pending() || system_reset_busy() || storage_changing() || storage_release_requested(); }
void portal_auth_storage_block(bool value)
{ rt_base_t level = rt_hw_interrupt_disable(); storage_blocked = value; rt_hw_interrupt_enable(level); }
bool portal_auth_storage_idle(void)
{
    rt_base_t level = rt_hw_interrupt_disable(); bool idle = !starting && !launching && !mutating;
    rt_hw_interrupt_enable(level); return idle;
}
static bool reserve_mutation(void)
{
    /* The handoff caller sets storage_blocked and reads idle without taking
     * the state mutex, so acceptance and the busy flag must be atomic to it. */
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = !mutating && !blocked();
    if (accepted) mutating = true;
    rt_hw_interrupt_enable(level);
    if (!accepted) return false;
    if (sleep_guard_acquire(&mutation_hold)) return true;
    level = rt_hw_interrupt_disable(); mutating = false; rt_hw_interrupt_enable(level); return false;
}
static bool random_hex(char out[PORTAL_AUTH_TOKEN_LENGTH + 1])
{
    unsigned char bytes[32]; static const char digits[] = "0123456789abcdef";
    bool ok = random_bytes(bytes, sizeof(bytes));
    if (ok) for (unsigned i = 0; i < sizeof(bytes); ++i)
    { out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15]; }
    out[PORTAL_AUTH_TOKEN_LENGTH] = 0; erase(bytes, sizeof(bytes)); return ok;
}
static bool derive(const char *password, const account_t *account, unsigned char out[32])
{
    mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = md && mbedtls_md_setup(&ctx, md, 1) == 0;
    if (ok) ok = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)password, strlen(password),
                account->salt, sizeof(account->salt), account->iterations, 32, out) == 0;
    mbedtls_md_free(&ctx); return ok;
}
static int account_id(uint32_t id)
{ for (unsigned i = 0; i < record.count; ++i) if (record.accounts[i].id == id) return (int)i; return -1; }
static int account_name(const char *name)
{ for (unsigned i = 0; i < record.count; ++i) if (!strcmp(record.accounts[i].username, name)) return (int)i; return -1; }
static void close_setup(void)
{ setup_open = false; ++setup_epoch; erase(setup_code, sizeof(setup_code)); }
static void expire(void)
{
    if (setup_open && elapsed(setup_started, PORTAL_AUTH_SETUP_SECONDS)) close_setup();
    for (unsigned i = 0; i < SESSION_COUNT; ++i)
        if (sessions[i].id && (elapsed(sessions[i].created, SESSION_ABSOLUTE_SECONDS) || elapsed(sessions[i].used, SESSION_IDLE_SECONDS)))
            erase(&sessions[i], sizeof(sessions[i]));
}
static void publish(void)
{
    portal_auth_snapshot_t next = {0};
    next.initialized = initialized; next.error = store_error; next.configured = record.count != 0;
    next.count = record.count; next.setup_open = setup_open; next.busy = mutating;
    if (setup_open)
    {
        next.setup_remaining_seconds = PORTAL_AUTH_SETUP_SECONDS - (rt_tick_t)(rt_tick_get() - setup_started) / RT_TICK_PER_SECOND;
        memcpy(next.setup_code, setup_code, sizeof(next.setup_code));
    }
    for (unsigned i = 0; i < record.count; ++i)
    { next.accounts[i].id = record.accounts[i].id; strcpy(next.accounts[i].username, record.accounts[i].username); }
    rt_base_t level = rt_hw_interrupt_disable();
    next.revision = published.revision;
    if (memcmp(&next, &published, sizeof(next))) ++next.revision;
    published = next; rt_hw_interrupt_enable(level);
}
void portal_auth_snapshot(portal_auth_snapshot_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable(); *out = published;
    rt_tick_t started = setup_started; rt_hw_interrupt_enable(level);
    if (!out->initialized) out->error = PORTAL_AUTH_UNAVAILABLE;
    if (out->setup_open)
    {
        unsigned age = (rt_tick_t)(rt_tick_get() - started) / RT_TICK_PER_SECOND;
        out->setup_remaining_seconds = age < PORTAL_AUTH_SETUP_SECONDS ? PORTAL_AUTH_SETUP_SECONDS - age : 0;
        if (!out->setup_remaining_seconds) { out->setup_open = false; erase(out->setup_code, sizeof(out->setup_code)); }
    }
}
static bool record_valid(void)
{
    if (record.count > PORTAL_AUTH_MAX_ACCOUNTS || !record.next_id) return false;
    for (unsigned i = 0; i < record.count; ++i)
    {
        account_t *a = &record.accounts[i];
        if (!a->id || a->id >= record.next_id || !a->iterations ||
            !memchr(a->username, 0, sizeof(a->username)) || !username_valid(a->username)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (a->id == record.accounts[j].id || !strcmp(a->username, record.accounts[j].username)) return false;
    }
    return true;
}
static void revoke(uint32_t id)
{ for (unsigned i = 0; i < SESSION_COUNT; ++i) if (sessions[i].id == id) erase(&sessions[i], sizeof(sessions[i])); }
static portal_auth_error_t save(const record_t *next)
{
    storage_lock();
    /* A handoff may start while waiting for storage. Before any file has been
     * opened, cancellation has a known unchanged outcome and is retryable. */
    portal_auth_error_t error = blocked() || !system_reset_write_allowed() ? PORTAL_AUTH_BUSY :
        storage_record_save(AUTH_PATH, AUTH_MAGIC, AUTH_VERSION, next, sizeof(*next)) ? PORTAL_AUTH_OK : PORTAL_AUTH_STORAGE_ERROR;
    storage_unlock(); return error;
}
static void finish_save(bool ok, const record_t *next, uint32_t revoked)
{
    if (ok) { record = *next; if (revoked) revoke(revoked); }
    else
    {
        /* A failed commit can have an uncertain durable outcome. Stop using
         * the old credentials until storage is read again after reboot. */
        store_error = PORTAL_AUTH_STORAGE_ERROR; erase(sessions, sizeof(sessions)); close_setup();
    }
}
static void account_worker(void *unused)
{
    (void)unused;
    for (;;)
    {
        rt_sem_take(&wake, ticks(1));
        take(true); expire(); publish();
        if (!mutating || !job_result.ticket || job_result.done) { release(); continue; }
        job_input_t input = job_input; erase(&job_input, sizeof(job_input));
        record_t next = record;
        int index = input.kind == JOB_ADD ? (int)next.count : account_id(input.id);
        portal_auth_error_t error = blocked() ? PORTAL_AUTH_BUSY : store_error;
        if (error == PORTAL_AUTH_OK && input.kind == JOB_ADD)
        {
            if (!next.count) error = PORTAL_AUTH_SETUP_CLOSED;
            else if (next.count >= PORTAL_AUTH_MAX_ACCOUNTS) error = PORTAL_AUTH_ACCOUNT_LIMIT;
            else if (account_name(input.username) >= 0) error = PORTAL_AUTH_ACCOUNT_EXISTS;
            else if (next.next_id == UINT32_MAX) error = PORTAL_AUTH_UNAVAILABLE;
            else { memset(&next.accounts[index], 0, sizeof(account_t)); next.accounts[index].id = next.next_id++; strcpy(next.accounts[index].username, input.username); ++next.count; }
        }
        else if (error == PORTAL_AUTH_OK && index < 0) error = PORTAL_AUTH_NOT_FOUND;
        if (error == PORTAL_AUTH_OK && input.kind != JOB_DELETE)
        {
            next.accounts[index].iterations = AUTH_ITERATIONS;
            if (!random_bytes(next.accounts[index].salt, sizeof(next.accounts[index].salt))) error = PORTAL_AUTH_CRYPTO_ERROR;
        }
        release();
        if (error == PORTAL_AUTH_OK && input.kind != JOB_DELETE && !derive(input.password, &next.accounts[index], next.accounts[index].hash)) error = PORTAL_AUTH_CRYPTO_ERROR;
        erase(input.password, sizeof(input.password));
        if (error == PORTAL_AUTH_OK && input.kind == JOB_DELETE)
        {
            memmove(&next.accounts[index], &next.accounts[index+1], (next.count - (unsigned)index - 1) * sizeof(account_t));
            memset(&next.accounts[--next.count], 0, sizeof(account_t));
        }
        if (error == PORTAL_AUTH_OK && blocked()) error = PORTAL_AUTH_BUSY;
        bool attempted = error == PORTAL_AUTH_OK;
        if (attempted) error = save(&next);
        take(true);
        if (attempted && error != PORTAL_AUTH_BUSY) finish_save(error == PORTAL_AUTH_OK, &next, input.kind == JOB_ADD ? 0 : input.id);
        sleep_guard_release(&mutation_hold); mutating = false;
        rt_base_t level = rt_hw_interrupt_disable(); job_result.done = true; job_result.error = error; rt_hw_interrupt_enable(level);
        publish(); release(); erase(&input, sizeof(input)); erase(&next, sizeof(next));
    }
}
bool portal_auth_init_failed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool failed = published.initialized && published.error != PORTAL_AUTH_OK;
    rt_hw_interrupt_enable(level);
    return failed;
}
rt_err_t portal_auth_init(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (initialized) { rt_hw_interrupt_enable(level); return store_error == PORTAL_AUTH_OK ? RT_EOK : -RT_EIO; }
    if (starting) { rt_hw_interrupt_enable(level); return -RT_EBUSY; }
    if (blocked()) { rt_hw_interrupt_enable(level); return -RT_EBUSY; }
    starting = true; rt_hw_interrupt_enable(level);
    if (!sleep_guard_acquire(&init_hold)) { starting = false; return -RT_EBUSY; }
    if (rt_mutex_init(&lock, "webauth", RT_IPC_FLAG_PRIO) != RT_EOK) goto failed;
    if (rt_sem_init(&wake, "authjob", 0, RT_IPC_FLAG_PRIO) != RT_EOK) { rt_mutex_detach(&lock); goto failed; }
    storage_lock();
    struct stat st;
    if (!storage_available(STORAGE_FLASH) || !storage_file_recover(AUTH_PATH)) store_error = PORTAL_AUTH_STORAGE_ERROR;
    else if (stat(AUTH_PATH, &st) == 0)
        store_error = S_ISREG(st.st_mode) && storage_record_load(AUTH_PATH, AUTH_MAGIC, AUTH_VERSION, &record, sizeof(record)) && record_valid() ? PORTAL_AUTH_OK : PORTAL_AUTH_STORE_CORRUPT;
    else if (errno == ENOENT || errno == -ENOENT)
    {
        /* A leftover replacement is evidence of interrupted provisioning,
         * not evidence that the device has never had credentials. */
        int temporary = stat(AUTH_PATH ".tmp", &st), temporary_errno = errno;
        int backup = stat(AUTH_PATH ".old", &st), backup_errno = errno;
        if (temporary == 0 || backup == 0) store_error = PORTAL_AUTH_STORE_CORRUPT;
        else if ((temporary_errno != ENOENT && temporary_errno != -ENOENT) || (backup_errno != ENOENT && backup_errno != -ENOENT)) store_error = PORTAL_AUTH_STORAGE_ERROR;
        else { memset(&record, 0, sizeof(record)); record.next_id = 1; store_error = PORTAL_AUTH_OK; }
    }
    else store_error = PORTAL_AUTH_STORAGE_ERROR;
    storage_unlock();
    if (store_error != PORTAL_AUTH_OK) erase(&record, sizeof(record));
    rt_thread_t worker = epd_thread_create_psram("authjob", account_worker, NULL, 6144, RT_THREAD_PRIORITY_MAX - 4, 10);
    if (!worker) { rt_sem_detach(&wake); rt_mutex_detach(&lock); goto failed; }
    initialized = true; publish();
    if (rt_thread_startup(worker) != RT_EOK)
    { initialized = false; rt_thread_delete(worker); rt_sem_detach(&wake); rt_mutex_detach(&lock); goto failed; }
    sleep_guard_release(&init_hold); starting = false; return store_error == PORTAL_AUTH_OK ? RT_EOK : -RT_EIO;
failed:
    sleep_guard_release(&init_hold); starting = false; store_error = PORTAL_AUTH_UNAVAILABLE; publish(); return -RT_ENOMEM;
}
static void start_worker(void *unused)
{
    (void)unused; portal_auth_init();
    rt_base_t level = rt_hw_interrupt_disable(); launching = false; rt_hw_interrupt_enable(level);
}
void portal_auth_start(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (initialized || starting || launching || blocked()) { rt_hw_interrupt_enable(level); return; }
    launching = true; rt_hw_interrupt_enable(level);
    rt_thread_t thread = epd_thread_create_psram("authinit", start_worker, NULL, 4096, RT_THREAD_PRIORITY_MAX - 4, 10);
    if (thread && rt_thread_startup(thread) == RT_EOK) return;
    if (thread) rt_thread_delete(thread);
    level = rt_hw_interrupt_disable(); launching = false; rt_hw_interrupt_enable(level);
}
static portal_auth_error_t submit(job_kind_t kind, uint32_t id, const char *username, const char *password, uint32_t *ticket)
{
    if (!ticket || (kind == JOB_ADD && !username_valid(username)) || (kind != JOB_DELETE && !password_valid(password))) return PORTAL_AUTH_INVALID;
    if (!initialized) return PORTAL_AUTH_UNAVAILABLE;
    if (!take(false)) return PORTAL_AUTH_BUSY;
    portal_auth_error_t error = store_error;
    if (error == PORTAL_AUTH_OK && (mutating || blocked() || !system_reset_write_allowed())) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK && kind == JOB_ADD)
    {
        if (!record.count) error = PORTAL_AUTH_SETUP_CLOSED;
        else if (record.count >= PORTAL_AUTH_MAX_ACCOUNTS) error = PORTAL_AUTH_ACCOUNT_LIMIT;
        else if (account_name(username) >= 0) error = PORTAL_AUTH_ACCOUNT_EXISTS;
    }
    if (error == PORTAL_AUTH_OK && kind != JOB_ADD && account_id(id) < 0) error = PORTAL_AUTH_NOT_FOUND;
    if (error == PORTAL_AUTH_OK && !reserve_mutation()) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK)
    {
        memset(&job_input, 0, sizeof(job_input)); job_input.kind = kind; job_input.id = id;
        if (username) strcpy(job_input.username, username);
        if (password) strcpy(job_input.password, password);
        if (!++ticket_serial) ++ticket_serial;
        rt_base_t level = rt_hw_interrupt_disable(); job_result = (portal_auth_job_t){ticket_serial, false, PORTAL_AUTH_OK}; rt_hw_interrupt_enable(level);
        *ticket = ticket_serial; mutating = true; publish(); rt_sem_release(&wake);
    }
    release(); return error;
}
portal_auth_error_t portal_auth_add_async(const char *u, const char *p, uint32_t *t) { return submit(JOB_ADD, 0, u, p, t); }
portal_auth_error_t portal_auth_password_async(uint32_t id, const char *p, uint32_t *t) { return submit(JOB_PASSWORD, id, NULL, p, t); }
portal_auth_error_t portal_auth_delete_async(uint32_t id, uint32_t *t) { return submit(JOB_DELETE, id, NULL, NULL, t); }
bool portal_auth_result(uint32_t ticket, portal_auth_job_t *out)
{
    if (!out || !ticket) return false;
    rt_base_t level = rt_hw_interrupt_disable(); *out = job_result; rt_hw_interrupt_enable(level);
    return out->ticket == ticket;
}
portal_auth_error_t portal_auth_bootstrap_open(void)
{
    if (!initialized) return PORTAL_AUTH_UNAVAILABLE;
    if (!take(false)) return PORTAL_AUTH_BUSY;
    portal_auth_error_t error = store_error;
    if (error == PORTAL_AUTH_OK && (record.count || mutating || blocked())) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK)
    {
        unsigned char bytes[PORTAL_AUTH_CODE_LENGTH];
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        close_setup();
        if (!random_bytes(bytes, sizeof(bytes))) error = PORTAL_AUTH_CRYPTO_ERROR;
        else
        {
            for (unsigned i = 0; i < sizeof(bytes); ++i) setup_code[i] = alphabet[bytes[i] & 31];
            setup_started = rt_tick_get(); setup_open = true;
        }
        erase(bytes, sizeof(bytes)); publish();
    }
    release(); return error;
}
void portal_auth_bootstrap_close(void)
{ if (take(true)) { close_setup(); publish(); release(); } }
void portal_auth_invalidate_sessions(void)
{ if (take(true)) { ++session_epoch; erase(sessions, sizeof(sessions)); close_setup(); publish(); release(); } }
static portal_auth_error_t rate_limit(uint32_t peer, unsigned *retry_after)
{
    rt_tick_t now = rt_tick_get();
    if (!global_rate.active || elapsed(global_rate.start, RATE_SECONDS)) global_rate = (rate_t){true, 0, now, 0};
    rate_t *slot = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < RATE_IP_COUNT; ++i)
    {
        if (ip_rates[i].active && elapsed(ip_rates[i].start, RATE_SECONDS)) ip_rates[i].active = false;
        if (ip_rates[i].active && ip_rates[i].peer == peer) slot = &ip_rates[i];
        else if (!ip_rates[i].active) free_slot = &ip_rates[i];
    }
    if (!slot && free_slot) { slot = free_slot; *slot = (rate_t){true, peer, now, 0}; }
    unsigned wait = 0;
    if (global_rate.count >= RATE_GLOBAL) wait = RATE_SECONDS - (rt_tick_t)(now - global_rate.start) / RT_TICK_PER_SECOND;
    if (slot && slot->count >= RATE_IP)
    { unsigned local = RATE_SECONDS - (rt_tick_t)(now - slot->start) / RT_TICK_PER_SECOND; if (local > wait) wait = local; }
    if (!slot) wait = RATE_SECONDS;
    if (wait) { if (retry_after) *retry_after = wait; return PORTAL_AUTH_RATE_LIMITED; }
    ++global_rate.count; ++slot->count; return PORTAL_AUTH_OK;
}
static portal_auth_error_t new_session(uint32_t id, portal_auth_session_t *out)
{
    expire(); int index = -1;
    for (unsigned i = 0; i < SESSION_COUNT; ++i) if (!sessions[i].id) { index = (int)i; break; }
    if (index < 0) return PORTAL_AUTH_BUSY;
    int account = account_id(id); if (account < 0) return PORTAL_AUTH_UNAUTHENTICATED;
    session_t next = {0};
    if (!random_hex(next.token) || !random_hex(next.csrf)) { erase(&next, sizeof(next)); return PORTAL_AUTH_CRYPTO_ERROR; }
    for (unsigned i = 0; i < SESSION_COUNT; ++i)
        if (sessions[i].id && equal(sessions[i].token, next.token, PORTAL_AUTH_TOKEN_LENGTH)) { erase(&next, sizeof(next)); return PORTAL_AUTH_CRYPTO_ERROR; }
    next.id = id; next.created = next.used = rt_tick_get(); sessions[index] = next;
    strcpy(out->username, record.accounts[account].username); strcpy(out->token, next.token); strcpy(out->csrf, next.csrf);
    erase(&next, sizeof(next)); return PORTAL_AUTH_OK;
}
portal_auth_error_t portal_auth_login(uint32_t peer, const char *username, const char *password, portal_auth_session_t *out, unsigned *retry_after)
{
    if (retry_after) *retry_after = 0;
    if (!out) return PORTAL_AUTH_INVALID;
    memset(out, 0, sizeof(*out)); if (!take(true)) return PORTAL_AUTH_UNAVAILABLE;
    portal_auth_error_t error = store_error;
    if (error == PORTAL_AUTH_OK && blocked()) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK) error = rate_limit(peer, retry_after);
    if (error == PORTAL_AUTH_OK && (!username_valid(username) || !password_valid(password))) error = PORTAL_AUTH_BAD_CREDENTIALS;
    int index = error == PORTAL_AUTH_OK ? account_name(username) : -1;
    account_t account = {0}; account.iterations = AUTH_ITERATIONS;
    if (index >= 0)
    {
        account = record.accounts[index];
        if (account.iterations != AUTH_ITERATIONS) error = PORTAL_AUTH_BAD_CREDENTIALS;
    }
    uint32_t epoch = session_epoch; release();
    unsigned char derived[32] = {0};
    if (error == PORTAL_AUTH_OK && !derive(password, &account, derived)) error = PORTAL_AUTH_CRYPTO_ERROR;
    if (error == PORTAL_AUTH_OK && (!equal(derived, account.hash, sizeof(derived)) || index < 0)) error = PORTAL_AUTH_BAD_CREDENTIALS;
    take(true);
    int current = account_id(account.id);
    if (error == PORTAL_AUTH_OK && (store_error != PORTAL_AUTH_OK || epoch != session_epoch || current < 0 || !equal(&record.accounts[current], &account, sizeof(account)))) error = PORTAL_AUTH_UNAUTHENTICATED;
    if (error == PORTAL_AUTH_OK) error = new_session(account.id, out);
    release(); erase(derived, sizeof(derived)); erase(&account, sizeof(account)); return error;
}
portal_auth_error_t portal_auth_setup(uint32_t peer, const char *username, const char *password, const char *code, portal_auth_session_t *out, unsigned *retry_after)
{
    if (retry_after) *retry_after = 0;
    if (!out) return PORTAL_AUTH_INVALID;
    memset(out, 0, sizeof(*out)); if (!take(true)) return PORTAL_AUTH_UNAVAILABLE;
    expire(); portal_auth_error_t error = store_error;
    if (error == PORTAL_AUTH_OK) error = rate_limit(peer, retry_after);
    if (error == PORTAL_AUTH_OK && (record.count || !setup_open)) error = PORTAL_AUTH_SETUP_CLOSED;
    if (error == PORTAL_AUTH_OK && (mutating || blocked())) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK && (!code || strlen(code) != PORTAL_AUTH_CODE_LENGTH || !equal(code, setup_code, PORTAL_AUTH_CODE_LENGTH))) error = PORTAL_AUTH_BAD_CREDENTIALS;
    if (error == PORTAL_AUTH_OK && (!username_valid(username) || !password_valid(password))) error = PORTAL_AUTH_INVALID;
    record_t next = record; uint32_t epoch = session_epoch;
    if (error == PORTAL_AUTH_OK && next.next_id == UINT32_MAX) error = PORTAL_AUTH_UNAVAILABLE;
    if (error == PORTAL_AUTH_OK && !reserve_mutation()) error = PORTAL_AUTH_BUSY;
    if (error == PORTAL_AUTH_OK)
    {
        close_setup(); mutating = true;
        account_t *a = &next.accounts[0]; memset(a, 0, sizeof(*a));
        a->id = next.next_id++; a->iterations = AUTH_ITERATIONS; strcpy(a->username, username); next.count = 1;
        if (!random_bytes(a->salt, sizeof(a->salt))) error = PORTAL_AUTH_CRYPTO_ERROR;
    }
    uint32_t generation = setup_epoch; bool reserved = mutating && !record.count && next.count == 1;
    publish(); release();
    if (error == PORTAL_AUTH_OK && !derive(password, &next.accounts[0], next.accounts[0].hash)) error = PORTAL_AUTH_CRYPTO_ERROR;
    if (reserved)
    {
        take(true);
        if (error == PORTAL_AUTH_OK && (epoch != session_epoch || generation != setup_epoch || elapsed(setup_started, PORTAL_AUTH_SETUP_SECONDS))) error = PORTAL_AUTH_SETUP_CLOSED;
        if (error == PORTAL_AUTH_OK && blocked()) error = PORTAL_AUTH_BUSY;
        release();
        bool attempted = error == PORTAL_AUTH_OK;
        if (attempted) error = save(&next);
        take(true);
        if (attempted && error != PORTAL_AUTH_BUSY)
        {
            finish_save(error == PORTAL_AUTH_OK, &next, 0);
            if (error == PORTAL_AUTH_OK) error = epoch == session_epoch ? new_session(next.accounts[0].id, out) : PORTAL_AUTH_UNAUTHENTICATED;
        }
        sleep_guard_release(&mutation_hold); mutating = false; publish(); release();
    }
    erase(&next, sizeof(next)); return error;
}
portal_auth_error_t portal_auth_session(const char *token, const char *csrf, bool mutation, portal_auth_session_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!token || strlen(token) != PORTAL_AUTH_TOKEN_LENGTH || !take(true)) return PORTAL_AUTH_UNAUTHENTICATED;
    expire(); portal_auth_error_t error = PORTAL_AUTH_UNAUTHENTICATED;
    if (store_error == PORTAL_AUTH_OK) for (unsigned i = 0; i < SESSION_COUNT; ++i)
    {
        session_t *s = &sessions[i];
        if (!s->id || !equal(token, s->token, PORTAL_AUTH_TOKEN_LENGTH)) continue;
        int account = account_id(s->id); if (account < 0) break;
        if (mutation && (!csrf || strlen(csrf) != PORTAL_AUTH_TOKEN_LENGTH || !equal(csrf, s->csrf, PORTAL_AUTH_TOKEN_LENGTH))) { error = PORTAL_AUTH_CSRF_INVALID; break; }
        s->used = rt_tick_get(); error = PORTAL_AUTH_OK;
        if (out) { strcpy(out->username, record.accounts[account].username); strcpy(out->token, s->token); strcpy(out->csrf, s->csrf); }
        break;
    }
    release(); return error;
}
void portal_auth_logout(const char *token)
{
    if (!token || strlen(token) != PORTAL_AUTH_TOKEN_LENGTH || !take(true)) return;
    for (unsigned i = 0; i < SESSION_COUNT; ++i)
        if (sessions[i].id && equal(token, sessions[i].token, PORTAL_AUTH_TOKEN_LENGTH)) erase(&sessions[i], sizeof(sessions[i]));
    release();
}
const char *portal_auth_error_code(portal_auth_error_t error)
{
    static const char *const codes[] = {"ok", "busy", "invalid_request", "auth_unavailable", "auth_store_corrupt", "storage_error", "crypto_error", "account_exists", "account_limit", "account_not_found", "setup_closed", "invalid_credentials", "rate_limited", "unauthenticated", "csrf_invalid"};
    return (unsigned)error < sizeof(codes)/sizeof(codes[0]) ? codes[error] : "auth_unavailable";
}
