#ifndef REAL_FONT_PIPE
static void persistent_fd_counts_worker(void)
{
    remote_font_t font; source_fixture(&font, 1);
    operation_timing_test = true;
    for (unsigned i = 0; i < 100; ++i) assert(source_request(&font, i * 4096u, 1024) == CRF_OK);
    face_t *face = font.primary;
    assert(source_opens == 1 && source_stats == 1 && source_seeks == 100 && !source_closes);
    assert(face->file_io[SOURCE_IO_OPEN].calls == 1 && face->file_io[SOURCE_IO_FSTAT].calls == 1);
    assert(fd_pool.held == 1 && open_fds == 1 && !io_depth);
    assert(source_request(&font, 13, EPD_COMPUTE_PIPE_BYTES) == CRF_OK);
    assert(source_opens == 1 && source_stats == 1 && source_seeks == 101);
    assert(close_source(face) && source_closes == 1 && !fd_pool.held);
    assert(face->file_io[SOURCE_IO_CLOSE].calls == 1 && face->file_io[SOURCE_IO_CLOSE].ticks == 17);
    source_fixture_release(&font);
}
static void persistent_fd_counts(void) { source_case_run(persistent_fd_counts_worker); }

static void persistent_fd_close_failure_worker(void)
{
    remote_font_t font; source_fixture(&font, 1); assert(source_request(&font, 0, 1024) == CRF_OK);
    source_fault = 6;
    assert(!close_source(font.primary) && !fd_released(font.primary) && open_fds == 1);
    assert(!close_source(font.primary));
    LONG attempts = source_closes;
    for (unsigned i = 0; i < 20; ++i) assert(!close_source(font.primary));
    assert(source_closes == attempts && fd_pool.held == 1);
    source_fault = 0; source_fd_retire(&font.primary->fd_state, 7);
    assert(close_source(font.primary) && !open_fds && !fd_pool.held);
    source_fixture_release(&font);
}
static void persistent_fd_close_failure(void) { source_case_run(persistent_fd_close_failure_worker); }

static void persistent_fd_release_inflight(void)
{
    hold_io = 1;
    lv_font_t *font = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    fixture_handoff = true;
    assert(!ui_reader_font_remote_release_storage(23));
    assert(fd_pool.held == 1);
    for (unsigned i = 0; i < 30; ++i) assert(!ui_reader_font_remote_release_storage(23));
    SetEvent(io_continue); idle();
    assert(ui_reader_font_remote_release_storage(23) && !fd_pool.held);
    destroy(font); fixture_handoff = false; hold_io = 0;
}

static void wait_fd_state(unsigned closes, unsigned held)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    EnterCriticalSection(&cache_lock.cs);
    while (source_closes < (LONG)closes || fd_pool.held != held) {
        ULONGLONG now = GetTickCount64(); assert(now < deadline);
        assert(SleepConditionVariableCS(&transition, &cache_lock.cs, (DWORD)(deadline - now)));
    }
    LeaveCriticalSection(&cache_lock.cs);
}

static void persistent_fd_release_retry(void)
{
    lv_font_t *font = create(1); ready_font(font);
    ResetEvent(io_entered); ResetEvent(io_continue);
    fixture_handoff = true; source_fault = 6; hold_close = 1;
    assert(!ui_reader_font_remote_release_storage(23));
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    /* The worker is blocked inside close. Every UI poll must still return. */
    for (unsigned i = 0; i < 30; ++i) assert(!ui_reader_font_remote_release_storage(23));
    SetEvent(io_continue);
    wait_fd_state(2, 1); idle();
    for (unsigned i = 0; i < 30; ++i) assert(!ui_reader_font_remote_release_storage(23));
    assert(source_closes == 2 && open_fds == 1);
    source_fault = 0;
    assert(!ui_reader_font_remote_release_storage(24));
    wait_fd_state(3, 0); idle();
    assert(ui_reader_font_remote_release_storage(24) && source_closes == 3 && !open_fds);
    destroy(font); fixture_handoff = false;
    assert(!sources && !allocations && !leases);
}

static void wait_closed_pipes(void)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    EnterCriticalSection(&cache_lock.cs);
    while (resident_sources || sources) {
        ULONGLONG now = GetTickCount64(); assert(now < deadline);
        assert(SleepConditionVariableCS(&transition, &cache_lock.cs, (DWORD)(deadline - now)));
    }
    LeaveCriticalSection(&cache_lock.cs);
    idle();
}

static void persistent_fd_sleep_handoff(void)
{
    lv_font_t *a = create(1), *b = create(2); ready_font(a); ready_font(b);
    assert(open_fds == 2 && fd_pool.held == 2 && resident_sources == 2);
    assert(sleep_guard_prepare() && sleep_guard_commit());
    LONG notified_before = power_notifications, reads = raw_reads;
    fixture_handoff = true;
    assert(!ui_reader_font_remote_release_storage(23));
    wait_fd_state(2, 0); idle();
    assert(ui_reader_font_remote_release_storage(23));
    assert(!open_fds && !shutdown_calls && resident_sources == 2);
    assert(power_notifications > notified_before);
    for (face_t *s = sources; s; s = s->next) assert(s->closing && !s->pipe);
    /* A completed storage handoff may unmount immediately, while the ACPU
     * allocation remains retained. Old shells cannot initiate another read. */
    fixture_mounted = false; ++fixture_session;
    assert(ui_reader_font_remote_prepare(a, "A", 1, true) == -1);
    assert(ui_reader_font_remote_prepare(b, "B", 1, true) == -1);
    assert(raw_reads == reads);
    sleep_guard_cancel(); ui_reader_font_remote_resume_cleanup(); wait_closed_pipes();
    assert(!sources && !fd_pool.held && shutdown_calls == 2);
    destroy(a); destroy(b); assert(!fonts && !allocations && !leases);
}

static void persistent_fd_closing_visible(void)
{
    lv_font_t *font = create(1); ready_font(font);
    ResetEvent(io_entered); ResetEvent(io_continue); hold_shutdown = 1;
    ui_reader_font_remote_cancel(font);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    lock(); assert(sources && !sources->pipe && sources->closing && resident_sources == 1); unlock();
    assert(!open_fds && !fd_pool.held);
    assert(sleep_guard_prepare() && !sleep_guard_commit());
    fixture_handoff = true;
    assert(ui_reader_font_remote_release_storage(23));
    SetEvent(io_continue); idle(); sleep_guard_cancel();
    destroy(font); assert(!sources && !allocations && !leases);
}

static void persistent_fd_sleep_batch(void)
{
    assert(sleep_guard_prepare() && sleep_guard_commit());
    lv_font_t *font = create(1); idle();
    assert(remote(font)->sleep_parked && !remote_opens && !raw_reads && !open_fds);
    assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 0);
    idle(); assert(!remote_opens && !raw_reads);
    sleep_guard_cancel(); ui_reader_font_remote_resume_cleanup(); ready_font(font);
    assert(!remote(font)->sleep_parked);
    assert(ui_reader_font_remote_ready(font, 'A', 0, true) == 1);
    assert(ui_reader_font_remote_ready(font, 'B', 0, true) == 1);
    destroy(font); assert(!sources && !allocations && !leases);
}

static void persistent_fd_batch_commit(void)
{
    hold_io = 1; lv_font_t *font = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    sleep_guard_snapshot_t snapshot; sleep_guard_snapshot(&snapshot);
    assert(snapshot.held_count && snapshot.held_count == snapshot.draining_count);
    assert(sleep_guard_prepare() && !sleep_guard_commit());
    SetEvent(io_continue); ready_font(font); hold_io = 0;
    assert(sleep_guard_commit());
    ui_reader_font_remote_cancel(font); idle();
    assert(!open_fds && !fd_pool.held && sources->closing && !shutdown_calls);
    sleep_guard_cancel(); ui_reader_font_remote_resume_cleanup(); wait_closed_pipes();
    destroy(font); assert(!sources && !allocations && !leases);
}

static void persistent_fd_sleep_failed_close(void)
{
    lv_font_t *font = create(1); ready_font(font);
    assert(sleep_guard_prepare() && sleep_guard_commit());
    fixture_handoff = true; source_fault = 6;
    assert(!ui_reader_font_remote_release_storage(23));
    wait_fd_state(2, 1); idle();
    assert(!shutdown_calls && sources->closing);
    for (unsigned i = 0; i < 20; ++i) assert(!ui_reader_font_remote_release_storage(23));
    assert(source_closes == 2);
    /* A stable reinsert generates a new ID even while the old mount drains. */
    source_fault = 0;
    assert(!ui_reader_font_remote_release_storage(24));
    wait_fd_state(3, 0); idle();
    assert(ui_reader_font_remote_release_storage(24) && !shutdown_calls);
    sleep_guard_cancel(); ui_reader_font_remote_resume_cleanup(); wait_closed_pipes();
    destroy(font); assert(!sources && !allocations && !leases);
}
#endif
