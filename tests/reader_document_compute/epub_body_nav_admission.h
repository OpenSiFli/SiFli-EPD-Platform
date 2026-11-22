/* SPDX-License-Identifier: Apache-2.0 */
static void admission_test(document_t *d, const char *name)
{
    bool navigation = strstr(name, "-nav") != NULL;
    bool cancel = strstr(name, "-cancel") != NULL;
    bool busy = strstr(name, "-busy") != NULL;
    bool partial = strstr(name, "-partial") != NULL;
    bool queued_nav = strstr(name, "queued-nav") != NULL;
    bool limit = strstr(name, "-limit") != NULL;
    bool automatic = strstr(name, "-auto") != NULL;
    bool busy_body = strstr(name, "busy-body") != NULL;
    if (navigation || (busy && !busy_body)) body_finish(d);
    if (navigation && !busy && !queued_nav) {
        at_navigation(d, 25); document_navigation_release(d);
    }
    if (busy && !busy_body) {
        pipe_open_status = EPD_COMPUTE_BUSY; pipe_open_partial = partial;
        if (automatic) { document_prefetch_enable(d, true); d->adjacent_pending = false; }
        else assert(document_navigation_request(d) == 0);
        for (unsigned i = 0; !d->open_pending; ++i) { assert(i < 10); step(); }
        assert(d->open_pending && conversion_document == d && d->status.result == BOOK_OK);
        unsigned attempts = waits_created, opened = pipe_opens;
        for (unsigned i = 0; i < 20; ++i) step();
        assert(waits_created == attempts && pipe_opens == opened && !d->status.error_code);
        assert(conversion_wait() > 0 && (uint32_t)conversion_wait() <= rt_tick_from_millisecond(20) + 1);
        if (partial) {
            pipe_shutdown_failures = 2;
            for (unsigned i = 0; i < 2; ++i) {
                host_ticks = d->open_retry; step();
                assert(d->open_pending && conversion_document == d && pipe_opens == opened);
                assert(d->status.result == BOOK_OK);
            }
        }
        if (limit) {
            for (unsigned i = 0; d->open_pending; ++i) {
                assert(i < 10); host_ticks = d->open_retry; step();
            }
            assert(waits_created - attempts <= 8 && !conversion_document);
            assert(d->nav_deferred && d->nav_state == DOCUMENT_NAV_PENDING && d->status.result == BOOK_OK);
            attempts = waits_created;
            for (unsigned i = 0; i < 100; ++i) {
                host_ticks += 1000; step();
                if (!automatic) assert(document_navigation_request(d) == -1);
            }
            assert(waits_created == attempts);
            save_spool(d); assert(d->persistent && committed);
            pipe_open_status = 0; pipe_open_partial = false;
            document_navigation_release(d);
            document_prefetch_enable(d, true); d->adjacent_pending = false;
            for (unsigned i = 0; i < 20; ++i) { host_ticks += 1000; step(); }
            assert(d->nav_deferred && waits_created == attempts);
            document_prefetch_enable(d, false);
            assert(document_navigation_request(d) == 0 && !d->nav_deferred);
            navigation_finish(d);
            puts("PASS BUSY deadline stops automatic OPENs; BODY cache survives and a new explicit NAV request succeeds");
            return;
        }
        pipe_open_status = 0; pipe_open_partial = false; host_ticks = d->open_retry;
        if (cancel) {
            d->cancelled = true; compute_cancel(d); step();
            assert(!conversion_document && !d->open_pending && pipe_opens == opened);
        } else {
            step(); assert(!d->open_pending && d->pipe && conversion_document == d);
            navigation_finish(d);
        }
        printf("PASS %s BUSY is retryable, throttled, CLOSE-before-retry and cancellable\n", name);
        return;
    }

    if (queued_nav) {
        char error[96]; document_anchor_t next = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, d->chapter + 1, 0, 0};
        document_t *active = document_open_at_internal(&d->file, &next, error, sizeof(error), false, false);
        assert(active); document_prefetch_enable(active, true);
        for (unsigned i = 0; !active->pipe; ++i) { assert(i < 100); step(); }
        active->owners = 0; active->retained = true;
        document_prefetch_enable(d, true); d->adjacent_pending = false; d->finish_stage = 0;
        unsigned opens = pipe_opens;
        for (unsigned i = 0; i < 20; ++i) {
            step(); assert(conversion_document == active && !nav_opens && pipe_opens == opens);
        }
        navigation_begin(d); assert(!nav_opens && conversion_document == active);
        active->owners = 1; active->cancelled = true; compute_cancel(active); pipe_shutdown_failures = 2;
        step(); step(); assert(!nav_opens && conversion_document == active);
        step(); assert(!conversion_document && !nav_opens);
        for (unsigned i = 0; !d->pipe; ++i) { assert(i < 100); step(); }
        assert(d->nav_state == DOCUMENT_NAV_ACTIVE && pipe_opens == opens + 1);
        document_prefetch_enable(d, false); navigation_finish(d);
        active->retained = false; document_close(active); pump();
        puts("PASS optional NAV waits for active BODY CLOSE; released slot advances NAV"); return;
    }

    /* A retained paused BODY (or paused NAV) no longer supplies the owner's
     * next chapter. The unclaimed neighbor must wait for the occupied slot. */
    char error[96];
    document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, d->chapter + 1, 0, 0};
    document_t *queued = document_open_at_internal(&d->file, &anchor, error, sizeof(error), false, false);
    assert(queued); queued->owners = 0; queued->retained = true;
    document_t owner = {0};
    owner.owners = 1; owner.claimed = owner.status.complete = owner.prefetch_enabled = owner.demand_controlled = true;
    owner.flow_order = ++flow_sequence;
    owner.following = queued; owner.next = documents; documents = &owner;
    if (!navigation) { d->owners = 0; d->retained = true; }
    unsigned opens = pipe_opens;
    for (unsigned i = 0; i < 20; ++i) {
        step(); assert(conversion_document == d && !queued->claimed && !queued->pipe && pipe_opens == opens);
    }
    convert_begin(queued); navigation_begin(queued);
    assert(conversion_document == d && !queued->converting && pipe_opens == opens);
    d->owners = 1;
    if (cancel) {
        d->cancelled = true; compute_cancel(d); pipe_shutdown_failures = 2;
        step(); step(); assert(conversion_document == d && !queued->claimed);
        step(); assert(!conversion_document && !queued->claimed);
    } else if (navigation) navigation_finish(d);
    else body_finish(d);
    d->finish_stage = 0; d->adjacent_pending = false;
    owner.flow_order = ++flow_sequence;
    committed = false; /* The host cache stub has a single identity. */
    if (busy_body) {
        pipe_open_status = EPD_COMPUTE_BUSY;
        for (unsigned i = 0; !queued->open_pending; ++i) { assert(i < 100); step(); }
        assert(conversion_document == queued && !queued->status.complete && queued->status.result == BOOK_OK);
        assert(!queued->status.error_code && pipe_opens == opens);
        unsigned attempts = waits_created;
        for (unsigned i = 0; i < 20; ++i) step();
        assert(waits_created == attempts);
        if (limit) {
            for (unsigned i = 0; conversion_document == queued; ++i) {
                assert(i < 10); host_ticks = queued->open_retry; step();
            }
            assert(!owner.following && waits_created - attempts <= 8 && pipe_opens == opens);
            assert(d->status.result == BOOK_OK && !d->status.error_code);
            assert(documents == &owner); documents = owner.next;
            pipe_open_status = 0;
            document_t *retry = document_open_at(&d->file, &anchor, error, sizeof(error)); assert(retry);
            document_prefetch_enable(retry, true);
            for (unsigned i = 0; !retry->pipe; ++i) { assert(i < 100); step(); }
            assert(retry->status.result == BOOK_OK && pipe_opens == opens + 1);
            document_close(retry); pump(); d->retained = false;
            puts("PASS optional BODY BUSY expires as cancellation, then a new foreground task opens"); return;
        }
        pipe_open_status = 0; host_ticks += 21;
    }
    for (unsigned i = 0; !queued->pipe; ++i) { assert(i < 100); step(); }
    assert(conversion_document == queued && queued->status.result == BOOK_OK && pipe_opens == opens + 1);
    assert(documents == &owner); documents = owner.next;
    queued->retained = false; queued->owners = 1; document_close(queued); pump();
    d->retained = false;
    printf("PASS %s occupied slot blocks unclaimed neighbor; release advances it\n", name);
}

static void body_cache_test(document_t *d, const char *name)
{
    bool failed = strstr(name, "failed") != NULL;
    bool active = strstr(name, "active") != NULL;
    uint32_t end = d->body_end;
    if (failed) {
        pipe_done_error = BOOK_IO_ERROR;
        document_navigation_request(d);
        for (unsigned i = 0; d->nav_state != DOCUMENT_NAV_FAILED; ++i) { assert(i < 20000); step(); }
        assert(d->status.result == BOOK_OK && d->finish_stage);
        pipe_done_error = 0;
    } else if (active) at_navigation(d, 25);
    save_spool(d);
    struct stat st; assert(!stat(persistent_path, &st) && st.st_size == end);
    assert(d->persistent && !d->observing && d->status.result == BOOK_OK);
    if (active) assert(d->converting && d->nav_state == DOCUMENT_NAV_ACTIVE);
    if (!failed && !active) assert(d->nav_state != DOCUMENT_NAV_DONE);
    /* Stop only the optional session; the committed BODY stays valid. */
    if (d->converting) { navigation_pause_locked(d); step(); }
    document_navigation_release(d);
    printf("PASS %s BODY commits independently of optional NAV\n", name);
}
