static void lifecycle_reset(void)
{
    CHECK(!commands && !queue_allocations && !lock_depth);
    reset(); seed(2);
    accepting = sleep_admit = true;
    ordinary_admit = true; background_cancelled = false;
    cancelled = retire_ready = settled = ui_active = cover_refresh = false;
    has_pending_media = media_dirty = flush_ok = flush_ready = flush_ack_pending = false;
    cover_busy = mutex_busy = false;
    scan_limit = false; library_read_result = MUSIC_MODEL_IO;
    flush_requested = flush_completed = 0;
    tf_release_id = tf_release_ack = 0;
    card_io_available = true;
    stops = ui_requests = last_ui_delay = wakes = view_stops = 0;
    timer_ticks = 0; timer_end = false; scan = current_page = NULL;
    worker = (void *)1;
}

static void selective_tf_release(void)
{
    lifecycle_reset();
    uint32_t flash_id;
    OK(music_queue_append(&store.queue, "/flash/Music/song.mp3", &flash_id));
    command(MUSIC_CMD_OPEN, flash_id, 0, NULL);
    update_track();
    CHECK(session_active && working.playing && !backend_card);
    uint64_t flash_session = audio.session_id;
    scan = (void *)1;
    strcpy(working.directory, "/sd/Music");
    card_io_available = false;
    CHECK(!epd_app_release_tf(worker, 41));
    removable_io_step(worker);
    CHECK(epd_app_release_tf(worker, 41) && !scan);
    CHECK(session_active && working.playing && audio.session_id == flash_session);

    card_io_available = true;
    command(MUSIC_CMD_OPEN, store.queue.entries[0].id, 0, NULL);
    update_track();
    CHECK(session_active && working.playing && backend_card);
    card_io_available = false;
    CHECK(!epd_app_release_tf(worker, 42));
    fail_close = true;
    removable_io_step(worker);
    CHECK(!epd_app_release_tf(worker, 42) && session_active && backend_card);
    fail_close = false;
    removable_io_step(worker);
    CHECK(epd_app_release_tf(worker, 42) && !session_active && !working.playing);
    card_io_available = true;
    puts("PASS TF export: Flash playback survives SD scan cleanup; SD playback must close before acknowledgement");
}

static void stopped_release(void)
{
    lifecycle_reset();
    current_page = (void *)2; music_ui_active(true);
    command(MUSIC_CMD_PLAY, 0, 0, NULL); audio.position_ms = 43000;
    command(MUSIC_CMD_STOP, 0, 0, NULL);
    CHECK(working.state == MUSIC_IDLE && !session_active && !dirty);
    CHECK(settle(worker) == RT_WAITING_FOREVER && retire_ready);
    CHECK(!ui_process(worker, false) && accepting && !stops);
    music_ui_active(false); /* ONPAUSE retains the page. */
    CHECK(music_view_present() && !ui_process(worker, false) && !stops);
    music_ui_active(true); /* ONRESUME remains usable. */
    CHECK(!ui_process(worker, false) && !view_stops && accepting);
    music_ui_active(false);
    current_page = NULL;
    unsigned requests = ui_requests;
    music_ui_active(false); /* ONSTOP must notify even after ONPAUSE. */
    CHECK(ui_requests == requests + 1 && !music_view_present());
    CHECK(!ui_process(worker, false) && cancelled && stops == 1 && !accepting);
    CHECK(!begin_work());
    restored_position(43000);
    CHECK(!ui_process(worker, false) && stops == 1);
    CHECK(ui_process(worker, true) && view_stops == 1);
    puts("PASS release: explicit stop saved, foreground/pause/resume protected, page exit, one cooperative stop");
}

static void startup_ui_yields(void)
{
    lifecycle_reset();
    current_page = (void *)2;
    music_ui_active(true);
    CHECK(cover_refresh && wakes == 1);
    /* The worker has not run yet. Its initial refresh must not cause the UI
     * to refill its own wake event on every foreground service iteration. */
    for (unsigned i = 0; i < 100000; ++i) {
        CHECK(!ui_process(worker, false));
        CHECK(wakes == 1 && ui_requests == 0 && !stops && cover_refresh);
    }
    music_ui_active(false);
    music_ui_active(true);
    CHECK(wakes == 3 && cover_refresh);
    unsigned requests = ui_requests;
    for (unsigned i = 0; i < 100; ++i) CHECK(!ui_process(worker, false));
    CHECK(wakes == 3 && ui_requests == requests);
    puts("PASS startup scheduling: 100000 UI polls before worker start do not replenish UI wake events; resume still wakes worker");
}

static void protected_lifecycle(void)
{
    lifecycle_reset();
    for (music_state_t state = MUSIC_OPENING; state <= MUSIC_ERROR; ++state) {
        command(MUSIC_CMD_STOP, 0, 0, NULL);
        working.state = state; working.playing = state == MUSIC_PLAYING;
        settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    }
    working.state = MUSIC_IDLE; working.playing = false;
    session_active = true; settle(worker); CHECK(!retire_ready);
    session_active = false; play_intent = true; settle(worker); CHECK(!retire_ready);
    play_intent = false;
    const music_command_kind_t transport[] = {MUSIC_CMD_OPEN, MUSIC_CMD_PLAY};
    for (unsigned i = 0; i < sizeof(transport) / sizeof(transport[0]); ++i) {
        command(MUSIC_CMD_STOP, 0, 0, NULL);
        command(transport[i], 0, -1, NULL); CHECK(play_intent);
        settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    }
    command(MUSIC_CMD_STOP, 0, 0, NULL); fail_open = true;
    command(MUSIC_CMD_PLAY, 0, 0, NULL);
    CHECK(!session_active && working.state == MUSIC_ERROR);
    play_intent = false; settle(worker); /* Error state alone still prevents retirement. */
    CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    puts("PASS priority: playing/paused/suspended/opening/seeking/draining/error and pending play intent retained");
}

static void pending_work_and_save(void)
{
    lifecycle_reset();
    command(MUSIC_CMD_PLAY, 0, 0, NULL); audio.position_ms = 98000;
    fail_save = true; command(MUSIC_CMD_STOP, 0, 0, NULL);
    CHECK(dirty && working.result == MUSIC_ERR_SAVE);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(begin_work()); settle(worker);
        CHECK(!retire_ready && !ui_process(worker, false) && !stops && accepting);
    }
    CHECK(store.queue.position_ms == 98000 && store.queue.count == 2);
    restored_position(0);
    fail_save = false; CHECK(save()); restored_position(98000);
    has_pending_media = true; settle(worker); CHECK(!retire_ready);
    has_pending_media = false; scan = (music_library_scan_t *)3;
    settle(worker); CHECK(!retire_ready); scan = NULL;
    cover_busy = true; settle(worker); CHECK(!retire_ready); cover_busy = false;
    settle(worker); CHECK(retire_ready);
    CHECK(!flush() && !retire_ready && flush_requested != flush_completed);
    CHECK(!ui_process(worker, false) && !stops);
    CHECK(save()); flush_completed = flush_requested; flush_ok = flush_ready = true;
    settle(worker); CHECK(retire_ready);
    music_command_t next = {.kind = MUSIC_CMD_FAVORITE, .value = 1};
    strcpy(next.path, "/sd/favorite.flac");
    CHECK(music_submit(&next) && !retire_ready && commands && queue_allocations == 1);
    CHECK(!ui_process(worker, false) && !stops);
    CHECK(begin_work() && take_command(&next)); execute(&next);
    CHECK(!commands && !queue_allocations && !dirty);
    settle(worker); CHECK(!ui_process(worker, false) && stops == 1);
    music_store_state_t saved; music_store_init(&saved); OK(music_store_load(&saved));
    CHECK(music_favorites_contains(&saved.favorites, "/sd/favorite.flac"));
    music_store_free(&saved);
    puts("PASS persistence/work: failed save retains state, retry succeeds, pending media/scan/cover/flush/commands postpone exit");
}

static void worker_preempts_stop(void)
{
    CHECK(!accepting && !cancelled);
    CHECK(!begin_work()); /* UI released guard but has not yet called app_service_stop. */
    music_command_t next = {.kind = MUSIC_CMD_PLAY};
    CHECK(!music_submit(&next) && !queue_allocations && !commands);
}

static void retirement_races(void)
{
    lifecycle_reset(); settle(worker);
    CHECK(retire_ready);
    CHECK(begin_work()); /* Worker wins guard: UI cannot retire in-progress work. */
    CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    settle(worker); CHECK(retire_ready);
    mutex_busy = true;
    CHECK(!ui_process(worker, false) && !stops && last_ui_delay == 10);
    mutex_busy = false;
    music_command_t next = {.kind = MUSIC_CMD_PLAY};
    CHECK(music_submit(&next) && !retire_ready);
    CHECK(!ui_process(worker, false) && !stops);
    CHECK(begin_work() && take_command(&next)); execute(&next); settle(worker);
    CHECK(session_active && !retire_ready && !ui_process(worker, false) && !stops);
    command(MUSIC_CMD_STOP, 0, 0, NULL); settle(worker);
    after_unlock = worker_preempts_stop;
    CHECK(!ui_process(worker, false) && cancelled && stops == 1 && !after_unlock);
    CHECK(!music_submit(&next) && !queue_allocations);
    puts("PASS races: initial idle plus queued PLAY, worker-first retained, UI-first closes admission, busy mutex defers");
}

static void submit_kind(music_command_kind_t kind)
{
    music_command_t next = {.kind = kind};
    CHECK(music_submit(&next));
}

static uint32_t epd_service_wait(epd_service_t *service, rt_int32_t delay)
{
    CHECK(service == worker && !lock_depth && wait_phase < 8);
    if (run_case >= 6) { release_wait(service, delay); return 0; }
    (void)delay;
    if (!run_case) {
        switch (wait_phase++) {
        case 0:
            CHECK(retire_ready && !session_active); submit_kind(MUSIC_CMD_PLAY); break;
        case 1:
            CHECK(session_active && working.playing); audio.position_ms = 67000;
            submit_kind(MUSIC_CMD_PAUSE); music_ui_active(false); current_page = NULL; break;
        case 2:
            CHECK(session_active && working.state == MUSIC_PAUSED && !retire_ready);
            CHECK(!ui_process(service, false) && !stops);
            fail_save = true; submit_kind(MUSIC_CMD_STOP); break;
        case 3:
            CHECK(!session_active && dirty && working.result == MUSIC_ERR_SAVE && !retire_ready);
            CHECK(!ui_process(service, false) && !stops && store.queue.count == 2);
            fail_save = false; ticks += 30 * RT_TICK_PER_SECOND; break;
        case 4:
            CHECK(retire_ready && !dirty && !session_active && !commands);
            CHECK(!ui_process(service, false) && cancelled && stops == 1);
            restored_position(67000); break;
        default: CHECK(false);
        }
    } else if (run_case == 1) {
        switch (wait_phase++) {
        case 0:
            for (unsigned i = 0; i < 3; ++i) submit_kind(MUSIC_CMD_FLUSH);
            submit_kind(MUSIC_CMD_STOP); submit_kind(MUSIC_CMD_PLAY); break;
        case 1:
            CHECK(!session_active && commands && !retire_ready);
            CHECK(!ui_process(service, false) && !stops); break;
        case 2:
            CHECK(session_active && working.playing && !commands && !retire_ready);
            submit_kind(MUSIC_CMD_STOP); break;
        case 3:
            CHECK(retire_ready && !commands && !dirty);
            CHECK(!ui_process(service, false) && cancelled && stops == 1); break;
        default: CHECK(false);
        }
    } else if (run_case == 2) {
        CHECK(wait_phase++ == 0 && retire_ready && !backend_opens);
        CHECK(store.queue.count == 2 && working.position_seconds == 88 && !session_active && !play_intent);
        CHECK(!ui_process(service, false) && !stops); /* Restored queue while foreground. */
        music_ui_active(false);
        CHECK(!ui_process(service, false) && !stops); /* Paused GUI still owns its page. */
        current_page = NULL; music_ui_active(false);
        CHECK(!ui_process(service, false) && stops == 1);
    } else if (run_case == 3) {
        switch (wait_phase++) {
        case 0:
            CHECK(!store.queue.count && dirty && !retire_ready && !backend_opens);
            music_ui_active(false); current_page = NULL;
            fail_save = true; CHECK(!flush());
            CHECK(!ui_process(service, false) && !stops); break;
        case 1:
            CHECK(dirty && working.result == MUSIC_ERR_SAVE && !retire_ready && !backend_opens);
            CHECK(!ui_process(service, false) && !stops);
            fail_save = false; CHECK(!flush()); break;
        case 2:
            CHECK(!dirty && retire_ready && flush_completed == flush_requested && !backend_opens);
            CHECK(!ui_process(service, false) && stops == 1); break;
        default: CHECK(false);
        }
    } else if (run_case == 4) {
        switch (wait_phase++) {
        case 0:
            CHECK(session_active && play_intent && working.playing && backend_opens == 1 && !retire_ready);
            music_ui_active(false); current_page = NULL;
            CHECK(!ui_process(service, false) && !stops);
            submit_kind(MUSIC_CMD_STOP); break;
        case 1:
            CHECK(retire_ready && !commands && !session_active);
            CHECK(!ui_process(service, false) && stops == 1); break;
        default: CHECK(false);
        }
    } else if (run_case == 5) {
        switch (wait_phase++) {
        case 0:
            CHECK(session_active && working.playing && backend_opens == 1);
            CHECK(working.result == MUSIC_ERR_LIMIT && working.detail == MUSIC_MODEL_LIMIT);
            CHECK(!scan && !library.entries && !library.count && !library.capacity);
            CHECK(store.queue.count == 2 && store.queue.current_id == 1 && !retire_ready);
            CHECK(!ui_process(service, false) && !stops);
            submit_kind(MUSIC_CMD_STOP); break;
        case 1:
            CHECK(retire_ready && !session_active && !commands);
            CHECK(!ui_process(service, false) && stops == 1); break;
        default: CHECK(false);
        }
    } else {
        CHECK(false);
    }
    return 0;
}

static void worker_exit_integration(void)
{
    for (run_case = 0; run_case < 2; ++run_case) {
        lifecycle_reset(); music_store_free(&store);
        wait_phase = destroyed = cover_stops = 0;
        epd_service_t *service = worker;
        run(service);
        CHECK(stops == 1 && !accepting && !worker && !backend && !commands && !queue_allocations);
        CHECK(destroyed == 1 && cover_stops == 1 && !store.queue.entries && !working.ready);
        CHECK(ui_process(service, true));
    }
    puts("PASS worker loop: pause/failed-save/retry/cleanup and STOP plus queued PLAY across batch boundary");
}

static void directory_limit_keeps_playback(void)
{
    lifecycle_reset();
    command(MUSIC_CMD_PLAY, 0, 0, NULL);
    library_read_result = MUSIC_MODEL_LIMIT;
    uint32_t selected = store.queue.current_id;
    command(MUSIC_CMD_OPEN, 0, -1, "/sd/too-large.mp3");
    CHECK(working.result == MUSIC_ERR_LIMIT && session_active && play_intent);
    CHECK(store.queue.current_id == selected && store.queue.count == 2 && backend_opens == 1);
    command(MUSIC_CMD_STOP, 0, 0, NULL);
    lifecycle_reset(); music_store_free(&store);
    scan_limit = true; run_case = 5;
    wait_phase = destroyed = cover_stops = 0;
    submit_kind(MUSIC_CMD_PLAY);
    epd_service_t *service = worker;
    run(service);
    CHECK(stops == 1 && !worker && !backend && !library.entries && destroyed == 1);
    CHECK(ui_process(service, true)); scan_limit = false;
    puts("PASS directory limit integration: OPEN keeps playback/queue; async scan drops partial index, reports LIMIT, playback continues until explicit STOP");
}

static void never_played_worker_exit(void)
{
    for (run_case = 2; run_case <= 4; ++run_case) {
        lifecycle_reset();
        uint32_t saved_crc = 0;
        if (run_case == 2) {
            store.queue.position_ms = 88000;
            OK(music_store_save(&store)); saved_crc = disk[0].crc;
        } else if (run_case == 3) {
            reset(); /* No saved queue or directory on the first launch. */
        } else {
            submit_kind(MUSIC_CMD_PLAY); /* Accepted before the worker's initial settle. */
        }
        music_store_free(&store);
        current_page = (void *)2; music_ui_active(true);
        wait_phase = destroyed = cover_stops = 0;
        epd_service_t *service = worker;
        run(service);
        CHECK(stops == 1 && !worker && !backend && !commands && !queue_allocations);
        CHECK(destroyed == 1 && cover_stops == 1 && !store.queue.entries);
        if (run_case == 2) {
            CHECK(!backend_opens && disk[0].crc == saved_crc);
            restored_position(88000);
        } else if (run_case == 3) {
            CHECK(!backend_opens);
            music_store_state_t loaded; music_store_init(&loaded); OK(music_store_load(&loaded));
            CHECK(!loaded.queue.count && loaded.directory && !strcmp(loaded.directory, "/sd"));
            music_store_free(&loaded);
        }
        CHECK(ui_process(service, true));
    }
    puts("PASS unused worker: restored queue without PLAY, first launch save retry, pre-start queued PLAY protected");
}

static void unused_pending_work(void)
{
    lifecycle_reset();
    scan = (music_library_scan_t *)3;
    settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    scan = NULL; cover_busy = true;
    settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    cover_busy = false; has_pending_media = true;
    settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    has_pending_media = false; working.state = MUSIC_ERROR;
    settle(worker); CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    working.state = MUSIC_IDLE; session_active = true;
    for (music_state_t state = MUSIC_PAUSED; state <= MUSIC_SUSPENDED; ++state) {
        working.state = state; settle(worker);
        CHECK(!retire_ready && !ui_process(worker, false) && !stops);
    }
    session_active = false; working.state = MUSIC_IDLE;
    settle(worker); CHECK(retire_ready && !ui_process(worker, false) && stops == 1);
    puts("PASS unused blockers: scan, cover, pending media, error, paused/suspended sessions; completed idle retires");
}

static void retire_during_allocation(void)
{
    CHECK(queue_allocations == 1 && !commands);
    CHECK(!ui_process(worker, false) && stops == 1);
}

static void unsubmitted_play_race(void)
{
    lifecycle_reset(); settle(worker); CHECK(retire_ready);
    during_alloc = retire_during_allocation;
    music_command_t next = {.kind = MUSIC_CMD_PLAY};
    CHECK(!music_submit(&next) && !during_alloc && !commands && !queue_allocations && !accepting);
    CHECK(!begin_work() && !backend_opens);
    puts("PASS unsubmitted PLAY: retirement before queue admission rejects and frees node without executing");
}

static void flush_interleave_start(void)
{
    CHECK(!work.service && !test_cleanup_holds && !test_cleanup_references);
    lifecycle_reset();
    save_failed = false;
    test_cleanup_admit = true; test_cleanup_enter_fail = false;
    current_page = (void *)2;
    music_ui_active(true);
    CHECK(begin_work());
    command(MUSIC_CMD_PLAY, 0, 0, NULL);
    audio.position_ms = 1000;
    update_track();
    CHECK(working.playing && !flush_ready && session_active);
}

static void flush_interleave_finish(void)
{
    CHECK(flush_requested == flush_completed && !work.cleanup && !test_cleanup_holds && !during_save);
    close_track(false);
    lock(); app_work_release(&work); unlock();
    CHECK(!work_holds && !test_cleanup_references && test_cleanup_context == 1);
    reset(); music_store_free(&store);
}

static void submit_seek_during_save(void)
{
    CHECK(!lock_depth);
    music_command_t next = {.kind = MUSIC_CMD_SEEK, .value = 42};
    CHECK(music_submit(&next));
}

static void flush_after_command(void)
{
    music_command_t next;
    CHECK(take_command(&next)); execute(&next); update_track();
    CHECK(!commands && !flush_ack_pending && !flush_ready);
    CHECK(!flush()); dispatch_save(worker);
    CHECK(flush_ack_pending && flush() && !flush_ack_pending);
    music_store_state_t loaded; music_store_init(&loaded);
    OK(music_store_load(&loaded));
    CHECK(loaded.queue.position_ms == audio.position_ms);
    CHECK(loaded.queue.current_id == store.queue.current_id);
    CHECK(loaded.queue.count == store.queue.count);
    music_store_free(&loaded);
}

static void flush_progress_interleavings(void)
{
    /* The save body and update_track are extracted verbatim from the service.
     * Each explicit scheduling order is finite; no real-time sleep is needed. */
    flush_interleave_start();
    CHECK(!flush()); dispatch_save(worker);
    CHECK(flush_ok && flush_ready && !dirty && !test_cleanup_holds);
    CHECK(flush() && flush_requested == 1 && flush_completed == 1);
    restored_position(1000);
    audio.position_ms += 50; update_track();
    CHECK(!flush_ready && working.playing);
    puts("PASS flush order: UI observes success before next progress; one request/one completed save");
    flush_interleave_finish();

    flush_interleave_start();
    command(MUSIC_CMD_PAUSE, 0, 0, NULL); update_track();
    CHECK(!working.playing && working.state == MUSIC_PAUSED);
    CHECK(!flush()); dispatch_save(worker);
    for (unsigned i = 0; i < 300; ++i) {
        ticks += 50; update_track();
        CHECK(flush_ready && flush() && flush_requested == 1);
    }
    CHECK(flush_completed == 1 && !dirty && !save_failed && !test_cleanup_holds);
    restored_position(1000);
    puts("PASS flush paused: 300 progress polls preserve one completed acknowledgement");
    flush_interleave_finish();

    flush_interleave_start();
    unsigned writes = saves[1];
    rt_tick_t started = ticks;
    for (unsigned i = 0; i < 300; ++i) {
        CHECK(!flush());
        CHECK(flush_requested == i + 1 && flush_completed == i);
        CHECK(!flush() && flush_requested == i + 1);
        dispatch_save(worker);
        CHECK(flush_completed == flush_requested && flush_ok && flush_ready && flush_ack_pending);
        CHECK(!dirty && !save_failed && !work.cleanup && !test_cleanup_holds);
        restored_position(audio.position_ms);
        ticks += 50; audio.position_ms += 50;
        update_track();
        CHECK(!flush_ready && working.playing && flush_ack_pending);
        CHECK(flush() && !flush_ack_pending && flush_requested == i + 1);
    }
    CHECK(ticks - started == 15000 && saves[1] == writes + 300);
    CHECK(flush_requested == 300 && flush_completed == 300);
    puts("PASS flush progress-before-UI: 300 independent requests, 300 writes, 300 consumed acknowledgements; no progress starvation");
    flush_interleave_finish();

    flush_interleave_start();
    CHECK(!flush()); dispatch_save(worker);
    for (unsigned i = 0; i < 300; ++i) {
        ticks += 50; audio.position_ms += 50; update_track();
        CHECK(!flush_ready && flush_ack_pending && flush_completed == 1);
    }
    restored_position(1000);
    CHECK(flush() && !flush_ack_pending && flush_requested == 1);
    CHECK(!flush() && flush_requested == 2);
    dispatch_save(worker);
    CHECK(flush() && !flush_ack_pending && flush_completed == 2);
    restored_position(audio.position_ms);
    puts("PASS delayed acknowledgement: 300 progress updates retain one receipt; the next independent request saves the latest position");
    flush_interleave_finish();

    flush_interleave_start();
    CHECK(music_backend_pause(backend) == 0); play_intent = false;
    update_track();
    writes = saves[1]; fail_save = true;
    CHECK(!flush());
    for (unsigned i = 0; i < 8; ++i) {
        dispatch_save(worker); update_track();
        CHECK(!flush_ok && !flush_ready && !flush_ack_pending && dirty && save_failed);
        CHECK(working.result == MUSIC_ERR_SAVE && working.detail == MUSIC_MODEL_IO);
        CHECK(!work.cleanup && !test_cleanup_holds && !test_cleanup_references);
        restored_position(0);
        ticks += 50;
        CHECK(!flush());
    }
    CHECK(saves[1] == writes + 8);
    fail_save = false; dispatch_save(worker); update_track();
    CHECK(flush_ok && flush_ready && !dirty && !save_failed && flush());
    CHECK(working.result == MUSIC_OK && working.detail == 0 && !working.result_path[0]);
    CHECK(flush_requested == 9 && flush_completed == 9 && !test_cleanup_holds);
    restored_position(1000);
    puts("PASS flush storage failure: eight completed IO failures preserve old data/dirty responsibility; recovery succeeds on attempt nine");
    flush_interleave_finish();

    const music_command_t edits[] = {
        {.kind = MUSIC_CMD_PAUSE},
        {.kind = MUSIC_CMD_SEEK, .value = 42},
        {.kind = MUSIC_CMD_NEXT},
        {.kind = MUSIC_CMD_APPEND, .path = "/sd/added.flac"},
    };
    for (unsigned i = 0; i < sizeof(edits) / sizeof(edits[0]); ++i) {
        flush_interleave_start();
        CHECK(!flush()); dispatch_save(worker);
        CHECK(flush_ack_pending && flush_ready);
        CHECK(music_submit(&edits[i]));
        CHECK(!flush_ack_pending && !flush_ready && commands);
        CHECK(!flush()); dispatch_save(worker);
        CHECK(flush_ok && flush_completed == flush_requested);
        CHECK(!flush_ack_pending && !flush_ready && commands);
        restored_position(1000);
        flush_after_command();
        if (edits[i].kind == MUSIC_CMD_SEEK) CHECK(audio.position_ms == 42000);
        if (edits[i].kind == MUSIC_CMD_NEXT) CHECK(store.queue.current_id == 2);
        if (edits[i].kind == MUSIC_CMD_APPEND) CHECK(store.queue.count == 3);
        flush_interleave_finish();
    }
    puts("PASS command invalidation: queued PAUSE/SEEK/NEXT/APPEND reject old receipt before execution; a later request saves the edited model");

    flush_interleave_start();
    CHECK(!flush()); during_save = submit_seek_during_save;
    dispatch_save(worker);
    CHECK(!during_save && flush_ok && flush_completed == 1 && commands);
    CHECK(!flush_ack_pending && !flush_ready && !test_cleanup_holds);
    restored_position(1000);
    flush_after_command();
    restored_position(42000);
    puts("PASS command during storage commit: successful write cannot publish a stale receipt over a newly accepted SEEK");
    flush_interleave_finish();

    flush_interleave_start();
    CHECK(!flush()); dispatch_save(worker);
    CHECK(flush_ack_pending);
    audio.state = MUSIC_BACKEND_ENDED; update_track();
    CHECK(store.queue.current_id == 2 && !flush_ack_pending && !flush_ready);
    CHECK(!flush()); dispatch_save(worker); CHECK(flush());
    restored_position(0);
    puts("PASS automatic track change: real update_track end-of-track handling invalidates unread success and persists the next track");
    flush_interleave_finish();

    const music_result_t other_errors[] = {MUSIC_ERR_STORAGE, MUSIC_ERR_MEMORY, MUSIC_ERR_DECODE, MUSIC_ERR_OUTPUT};
    for (unsigned i = 0; i < sizeof(other_errors) / sizeof(other_errors[0]); ++i) {
        flush_interleave_start();
        result(other_errors[i], -17, "/sd/failed.flac");
        uint32_t revision = working.result_revision;
        CHECK(!flush()); dispatch_save(worker); CHECK(flush());
        CHECK(working.result == other_errors[i] && working.detail == -17 &&
              !strcmp(working.result_path, "/sd/failed.flac") && working.result_revision == revision);
        flush_interleave_finish();
    }
    puts("PASS successful persistence preserves unrelated storage/memory/decode/output errors and their revision");
}
