#ifndef EPD_PORTAL_AUTH_H
#define EPD_PORTAL_AUTH_H
#include <rtthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTAL_AUTH_MAX_ACCOUNTS 5
#define PORTAL_AUTH_USERNAME_MAX 32
#define PORTAL_AUTH_PASSWORD_MAX 64
#define PORTAL_AUTH_CODE_LENGTH 8
#define PORTAL_AUTH_TOKEN_LENGTH 64
#define PORTAL_AUTH_SETUP_SECONDS 300

typedef enum
{
    PORTAL_AUTH_OK = 0, PORTAL_AUTH_BUSY, PORTAL_AUTH_INVALID,
    PORTAL_AUTH_UNAVAILABLE, PORTAL_AUTH_STORE_CORRUPT, PORTAL_AUTH_STORAGE_ERROR,
    PORTAL_AUTH_CRYPTO_ERROR, PORTAL_AUTH_ACCOUNT_EXISTS, PORTAL_AUTH_ACCOUNT_LIMIT,
    PORTAL_AUTH_NOT_FOUND, PORTAL_AUTH_SETUP_CLOSED, PORTAL_AUTH_BAD_CREDENTIALS,
    PORTAL_AUTH_RATE_LIMITED, PORTAL_AUTH_UNAUTHENTICATED, PORTAL_AUTH_CSRF_INVALID
} portal_auth_error_t;

typedef struct { uint32_t id; char username[PORTAL_AUTH_USERNAME_MAX + 1]; } portal_auth_account_t;
typedef struct
{
    bool initialized, configured, setup_open, busy;
    portal_auth_error_t error;
    uint32_t revision, setup_remaining_seconds;
    unsigned count;
    portal_auth_account_t accounts[PORTAL_AUTH_MAX_ACCOUNTS];
    /* Device UI only; never serialize this field into HTTP responses. */
    char setup_code[PORTAL_AUTH_CODE_LENGTH + 1];
} portal_auth_snapshot_t;
typedef struct { uint32_t ticket; bool done; portal_auth_error_t error; } portal_auth_job_t;
typedef struct
{
    char username[PORTAL_AUTH_USERNAME_MAX + 1];
    char token[PORTAL_AUTH_TOKEN_LENGTH + 1];
    char csrf[PORTAL_AUTH_TOKEN_LENGTH + 1];
} portal_auth_session_t;

/* Call on a service worker. Idempotent; reads persistent storage, starts the
 * account worker. A damaged record remains locked until local factory reset. */
rt_err_t portal_auth_init(void);
/* Cached store failures require successful reinitialization after repair. */
bool portal_auth_init_failed(void);
/* UI-safe, idempotent asynchronous initialization, also usable offline. */
void portal_auth_start(void);
/* Shared hardware RNG serialization, available before auth initialization. */
bool portal_auth_random_bytes(void *out, size_t length);
/* Block new work before power/reset/storage handoff, then wait for idle.
 * Unblock when the handoff has ended. Both calls are nonblocking. */
void portal_auth_storage_block(bool blocked);
bool portal_auth_storage_idle(void);
/* Copies published memory without waiting on hashing or storage. */
void portal_auth_snapshot(portal_auth_snapshot_t *out);
/* Device UI only. The first account always requires browser setup. Username:
 * 3..32 ASCII letters/digits/_.-; password: 8..64 valid UTF-8 bytes. Inputs are
 * copied. One outstanding job; no caller-owned pointer survives submission. */
portal_auth_error_t portal_auth_add_async(const char *username, const char *password, uint32_t *ticket);
portal_auth_error_t portal_auth_password_async(uint32_t id, const char *password, uint32_t *ticket);
portal_auth_error_t portal_auth_delete_async(uint32_t id, uint32_t *ticket);
/* Nonblocking read. Latest result remains available until the next accepted
 * job; closing a UI page neither cancels the job nor requires cleanup. */
bool portal_auth_result(uint32_t ticket, portal_auth_job_t *out);
portal_auth_error_t portal_auth_bootstrap_open(void);
void portal_auth_bootstrap_close(void);
void portal_auth_invalidate_sessions(void);

/* HTTP worker only; peer is IPv4 in network byte order. These may hash/write.
 * retry_after is seconds and is set when RATE_LIMITED is returned. */
portal_auth_error_t portal_auth_setup(uint32_t peer, const char *username, const char *password,
                                    const char *code, portal_auth_session_t *out, unsigned *retry_after);
portal_auth_error_t portal_auth_login(uint32_t peer, const char *username, const char *password,
                                    portal_auth_session_t *out, unsigned *retry_after);
portal_auth_error_t portal_auth_session(const char *token, const char *csrf, bool mutation,
                                      portal_auth_session_t *out);
void portal_auth_logout(const char *token);
const char *portal_auth_error_code(portal_auth_error_t error);
#endif
