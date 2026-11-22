/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#define main correctness_main
#include "host.c"
#undef main

static bool measuring;
static uint64_t guard_start, guard_calls, guards, max_calls, total_calls, max_ns, total_ns;
static uint64_t cpu_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
void profile_guard_enter(void)
{ if (measuring) { guard_start = cpu_ns(); guard_calls = 0; } }
void profile_guard_leave(void)
{
    if (!measuring) return;
    uint64_t elapsed = cpu_ns() - guard_start;
    ++guards; total_calls += guard_calls; total_ns += elapsed;
    if (elapsed > max_ns) max_ns = elapsed;
    if (guard_calls > max_calls) max_calls = guard_calls;
}
/* Only the production manager object is compiled with instrumentation. */
void __cyg_profile_func_enter(void *function, void *caller)
{ (void)function; (void)caller; if (measuring && irq_depth) ++guard_calls; }
void __cyg_profile_func_exit(void *function, void *caller)
{ (void)function; (void)caller; }

static epd_memory_owner_t chain(epd_memory_owner_t root, unsigned depth)
{
    epd_memory_owner_t p = root;
    for (unsigned i = 0; i < depth; ++i) {
        p = epd_memory_scope_create_trusted(p, EPD_MEMORY_SCOPE_TASK);
        assert(p != INVALID);
    }
    return p;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup(); assert(epd_memory_init() == EPD_MEMORY_OK);
    if (!strcmp(argv[1], "share62accounts")) {
        epd_memory_credit_t credits[62], prepared;
        epd_memory_association_t changes[62];
        epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
        for (unsigned i = 0; i < 62; ++i) {
            changes[i] = (epd_memory_association_t){epd_memory_owner_create("account"), 0, 100};
            assert(epd_memory_owner_set_limit(changes[i].account, backends[0], 100) == EPD_MEMORY_OK);
            credits[i] = request(changes[i].account, 100, 0, EPD_MEMORY_OK);
        }
        assert(epd_memory_credit_share_prepare(credits, 62, shared, backends[0], 6200, &prepared) == EPD_MEMORY_OK);
        measuring = true;
        assert(epd_memory_credit_share_commit(prepared, changes, 62) == EPD_MEMORY_OK);
        goto report;
    }
    epd_memory_owner_t root = epd_memory_owner_create("profile");
    epd_memory_owner_t leaf = chain(root, !strcmp(argv[1], "heap16") ? 15 : 14);
    if (!strcmp(argv[1], "share64") || !strcmp(argv[1], "share64association")) {
        epd_memory_credit_t credits[64], prepared;
        epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
        for (unsigned i = 0; i < 13; ++i) {
            shared = epd_memory_scope_create_trusted(shared, EPD_MEMORY_SCOPE_SHARED); assert(shared != INVALID);
        }
        for (unsigned i = 0; i < 64; ++i) credits[i] = request(leaf, 100, 0, EPD_MEMORY_OK);
        assert(epd_memory_credit_share_prepare(credits,64,shared,backends[0],6400,&prepared)==EPD_MEMORY_OK);
        measuring = true;
        if (!strcmp(argv[1], "share64association")) {
            epd_memory_association_t change = {root, 0, 6400};
            assert(epd_memory_credit_share_commit(prepared, &change, 1) == EPD_MEMORY_OK);
        } else assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    } else if (!strcmp(argv[1], "close111") || !strcmp(argv[1], "poll111") || !strcmp(argv[1], "heap111")) {
        epd_memory_credit_t held = request(leaf, 32000, 0, EPD_MEMORY_OK);
        for (unsigned i = 0; i < 111; ++i) request(leaf, 1000, 1000, EPD_MEMORY_WAITING);
        if (!strcmp(argv[1], "close111")) {
            measuring = true; assert(epd_memory_owner_close_trusted(root));
        } else if (!strcmp(argv[1], "poll111")) {
            epd_memory_credit_end(held);
            measuring = true; epd_memory_poll(epd_memory_now_ms());
        } else {
            epd_memory_owner_t previous = epd_memory_owner_enter_trusted(leaf);
            measuring = true;
            for (unsigned i = 0; i < 100; ++i) {
                void *p = rt_memheap_alloc(heaps[0], 32);
                if (p) rt_memheap_free(p);
            }
            measuring = false; epd_memory_owner_leave(previous);
        }
    } else {
        assert(!strcmp(argv[1], "heap0") || !strcmp(argv[1], "heap16"));
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(!strcmp(argv[1],"heap0") ? 0 : leaf);
        measuring = true;
        for (unsigned i = 0; i < 1000; ++i) {
            void *p = rt_memheap_alloc(heaps[0],32); assert(p); rt_memheap_free(p);
        }
        measuring = false; epd_memory_owner_leave(previous);
    }
report:
    measuring = false;
    printf("{\"case\":\"%s\",\"guards\":%llu,\"max_core_calls\":%llu,\"total_core_calls\":%llu,"
           "\"host_max_cpu_ns\":%llu,\"host_total_cpu_ns\":%llu}\n",argv[1],
           (unsigned long long)guards,(unsigned long long)max_calls,(unsigned long long)total_calls,
           (unsigned long long)max_ns,(unsigned long long)total_ns);
    return 0;
}
