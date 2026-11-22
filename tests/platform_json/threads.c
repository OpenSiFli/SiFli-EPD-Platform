/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_json.h"
#include "epd_memory.h"
#include <cJSON.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JSON_SHARED_ERRNO_CONTROL
static rt_err_t local_errno;
#else
static _Thread_local rt_err_t local_errno;
#endif
static _Thread_local size_t calls, fail_at;
static _Thread_local int hold_oom;
static atomic_size_t live;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int waiting, finished;

rt_err_t rt_get_errno(void) { return local_errno; }
void rt_set_errno(rt_err_t value)
{
    local_errno = value;
    if (value == ENOMEM && hold_oom) {
        pthread_mutex_lock(&lock);
        waiting = 1;
        pthread_cond_broadcast(&condition);
        while (!finished) pthread_cond_wait(&condition, &lock);
        pthread_mutex_unlock(&lock);
    }
}
void *rt_memset(void *p, int value, rt_ubase_t size) { return memset(p, value, size); }
void *rt_memcpy(void *d, const void *s, rt_ubase_t size) { return memcpy(d, s, size); }
void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    assert(region == EPD_APP_PSRAM);
    ++calls;
    if (calls == fail_at) return NULL;
    void *p = malloc(size);
    assert(p);
    atomic_fetch_add(&live, 1);
    /* Successful lower-level operations need not clear an older errno. */
    local_errno = ENOMEM;
    return p;
}
void rt_free(void *p)
{
    if (p) { assert(atomic_fetch_sub(&live, 1)); free(p); }
}

static const char document[] = "{\"id\":\"probe\",\"version\":1,\"items\":[true,null,\"value\"]}";

static void bounded_input(void)
{
    const char bounded[] = {'[', '1', ',', '2', ']'};
    const char *valid[] = {"{}", "null", "123", "true", "\"hello\"", "{} \t\r\n"};
    const char *invalid[] = {"", " ", "{", "{\"a\":}", "{}x", "{} {}", "nullx", "{}\v"};
    cJSON *json;
    assert(epd_json_parse(bounded, sizeof(bounded), &json) == EPD_JSON_OK);
    assert(cJSON_GetArraySize(json) == 2); epd_json_delete(json);
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        rt_set_errno(73);
        assert(epd_json_parse(valid[i], strlen(valid[i]), &json) == EPD_JSON_OK);
        assert(json && rt_get_errno() == 73); epd_json_delete(json);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        json = (cJSON *)1; rt_set_errno(ENOMEM);
        assert(epd_json_parse(invalid[i], strlen(invalid[i]), &json) == EPD_JSON_FORMAT_ERROR);
        assert(!json && rt_get_errno() == ENOMEM && !atomic_load(&live));
    }
    const char embedded[] = {'{', '}', 0, 'x'};
    assert(epd_json_parse(embedded, sizeof(embedded), &json) == EPD_JSON_FORMAT_ERROR);
    assert(!json);
    assert(epd_json_parse(NULL, 2, &json) == EPD_JSON_FORMAT_ERROR && !json);
    assert(epd_json_parse("{}", SIZE_MAX, &json) == EPD_JSON_FORMAT_ERROR && !json);
    assert(epd_json_parse("{}", 2, NULL) == EPD_JSON_FORMAT_ERROR);
    epd_json_delete(NULL);
    assert(!atomic_load(&live));
    puts("PASS JSON bounded input and errno restoration");
}

static void nth_allocation(void)
{
    cJSON *json;
    calls = 0;
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    size_t count = calls;
    epd_json_delete(json);
    for (size_t n = 1; n <= count; ++n) {
        calls = 0; fail_at = n; rt_set_errno(74);
        assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_NO_MEMORY);
        assert(!json && calls == n && rt_get_errno() == 74 && !atomic_load(&live));
        fail_at = 0;
        assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
        epd_json_delete(json);
        assert(rt_get_errno() == 74 && !atomic_load(&live));
    }
    printf("PASS JSON Nth allocation failures 1..%zu and subsequent recovery\n", count);
}

static void *failing_thread(void *unused)
{
    (void)unused;
    local_errno = 81; calls = 0; fail_at = 3; hold_oom = 1;
    cJSON *json = NULL;
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_NO_MEMORY);
    assert(!json && local_errno == 81);
    return NULL;
}

static void interleaved_threads(void)
{
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, failing_thread, NULL));
    pthread_mutex_lock(&lock);
    while (!waiting) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    local_errno = 82;
    cJSON *json = NULL;
    assert(epd_json_parse("{", 1, &json) == EPD_JSON_FORMAT_ERROR && !json);
    assert(epd_json_parse(document, sizeof(document) - 1, &json) == EPD_JSON_OK);
    assert(local_errno == 82); epd_json_delete(json);
    pthread_mutex_lock(&lock);
    finished = 1; pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    assert(!pthread_join(thread, NULL));
    assert(local_errno == 82 && !atomic_load(&live));
    puts("PASS JSON two-thread OOM/format/success isolation");
}

int main(int argc, char **argv)
{
    epd_json_init(); epd_json_init();
    if (argc == 2 && !strcmp(argv[1], "--interleave")) interleaved_threads();
    else { bounded_input(); nth_allocation(); interleaved_threads(); }
    return 0;
}
