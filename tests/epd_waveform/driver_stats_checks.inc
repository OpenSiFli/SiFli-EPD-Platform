static void clear_stats(void)
{
    char *args[] = {"epd_stats", "reset"};
    epd_stats(2, args);
}

static unsigned dirty_bytes(void)
{
    unsigned next = 0, offset, length, total = 0;
    while (epd_gray_next_span(&frame_dirty, &next, &offset, &length)) total += length;
    return total;
}

static void driver_stats_checks(void)
{
    char *query[] = {"epd_stats"};
    reset(); clear_stats();
    memset(&sync_dirty, 0, sizeof(sync_dirty)); clean_bytes = 0;
    request(0, 3, EPD_DRAW_MODE_INPUT);
    CHECK(request_input_pixels == EPD_GRAY_BLOCK_BYTES);
    CHECK(dirty_bytes() == EPD_GRAY_BLOCK_BYTES && clean_bytes == EPD_GRAY_BLOCK_BYTES);
    CHECK(mixed[at(0)] == 0x70 && mixed[at(1)] == 0x77 && mixed[0] == 0x77);
    drive_to_finish();
    CHECK(!panel_stats.completed && !panel_stats.input);
    rt_tick_t last_eof = host_tick - request_started;
    drain();
    CHECK(panel_stats.input == 1 && panel_stats.completed == 1 && !panel_stats.gc);
    CHECK(panel_stats.frame_count == 20 && panel_stats.input_pixels == EPD_GRAY_BLOCK_BYTES);
    CHECK(panel_stats.timing.poweron == 14 && panel_stats.timing.poweroff == 160);
    CHECK(panel_stats.timing.prepare == 3 && panel_stats.timing.scan == 140);
    CHECK(panel_stats.timing.commit == 0 && panel_stats.timing.eof == last_eof);
    CHECK(panel_stats.elapsed_ms == last_eof + 5 + 160);
    CHECK(mixed[at(0)] == 0 && mixed[at(1)] == 0x77 && mixed[0] == 0x77);
    unsigned sent = sends, completed = callbacks;
    epd_stats(1, query);
    CHECK(sends == sent && callbacks == completed && epd_panel_idle());

    /* Quantization can collapse a new gray to the already displayed black. */
    request(0, 2, EPD_DRAW_MODE_INPUT);
    CHECK(!drawing && sends == sent && panel_stats.completed == 1 && !dirty_bytes());
    CHECK(mixed[at(0)] == 0);

    /* Retained targets may be in different dirty blocks than the new input. */
    reset(); clear_stats(); fail_power = true;
    request(0, 3, EPD_DRAW_MODE_INPUT); drain();
    CHECK(shadow_known && mixed[at(0)] == 0x70 && panel_stats.errors == 1);
    fail_power = false;
    request(128, 10, EPD_DRAW_MODE_INPUT);
    CHECK(!request_input_pixels && mixed[at(0)] == 0x70 && mixed[at(128)] == 0x7a);
    drain();
    CHECK(panel_stats.gc == 1 && panel_stats.frame_count == 44);
    CHECK(mixed[at(0)] == 0 && mixed[at(128)] == 0xaa && mixed[at(64)] == 0x77);

    /* Last EOF failure preserves the last successful timing snapshot. */
    panel_stats_t good = panel_stats;
    overflow_complete_at = completions + 20;
    request(256, 11, EPD_DRAW_MODE_INPUT); drain();
    CHECK(!shadow_known && mixed[at(256)] == 0x7f && panel_stats.errors == 2);
    CHECK(panel_stats.completed == good.completed && panel_stats.elapsed_ms == good.elapsed_ms);
    CHECK(!memcmp(&panel_stats.timing, &good.timing, sizeof(good.timing)));
    overflow_complete_at = 0;
    unsigned ons = power_ons, offs = power_offs;
    request(384, 5, EPD_DRAW_MODE_INPUT); drain();
    CHECK(panel_stats.gc == 2 && panel_stats.frame_count == 85 && panel_stats.errors == 2);
    CHECK(panel_stats.timing.poweron <= 17 && panel_stats.timing.poweroff == 160);
    CHECK(power_ons == ons + 1 && power_offs == offs + 1);
    CHECK(panel_stats.timing.scan == 85 * 7);
    CHECK(panel_stats.elapsed_ms == panel_stats.timing.eof + 165);
    CHECK(mixed[at(256)] == 0xff && mixed[at(384)] == 0x55 && mixed[at(64)] == 0x77);
    epd_stats(1, query);

    /* Multiple sparse INPUT blocks retain gray neighbours and auto cadence. */
    reset(); clear_stats(); epd_wave_set_part_times(2);
    request(0, 0, EPD_DRAW_MODE_AUTO); drain();
    for (unsigned i = 1; i <= 3; ++i) {
        clean_bytes = 0;
        request(i * 128, 3, EPD_DRAW_MODE_INPUT);
        CHECK(request_input_pixels == EPD_GRAY_BLOCK_BYTES);
        CHECK(clean_bytes <= 2 * EPD_GRAY_BLOCK_BYTES);
        CHECK(ref57_wave_get_selected_mode() == 1); drain();
    }
    CHECK(panel_stats.gl == 1 && panel_stats.input == 3 && !panel_stats.gc);
    clear_stats();
    request(512, 3, EPD_DRAW_MODE_AUTO); CHECK(ref57_wave_get_selected_mode() == 0); drain();
    CHECK(panel_stats.gc == 1 && panel_stats.completed == 1);

    /* A stats reset during an in-flight frame preserves timing and callbacks. */
    reset(); epd_wave_set_part_times(10); host_tick = 0xfffffff0u;
    request(0, 0, EPD_DRAW_MODE_AUTO); eof();
    rt_tick_t started = request_started; unsigned next = next_frame;
    sent = sends; completed = callbacks;
    clear_stats(); epd_stats(1, query);
    CHECK(drawing && request_started == started && next_frame == next);
    CHECK(sends == sent && callbacks == completed);
    drain();
    CHECK(panel_stats.completed == 1 && panel_stats.gl == 1 && panel_stats.frame_count == 29);
    CHECK(panel_stats.timing.scan == 29 * 7 && panel_stats.elapsed_ms == host_tick - started);
    CHECK(panel_stats.elapsed_ms == panel_stats.timing.eof + 165);
    puts("PASS: sparse INPUT dirty spans, physical old/failed targets, no-change skip, stage/EOF timing, recovery totals, reset/query, tick wrap and cadence");
}
