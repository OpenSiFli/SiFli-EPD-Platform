static void prepared_request(EpdDrawMode mode)
{
    prepared_mode = mode;
    prepared_busy = prepared_in_flight = true;
    LCD_WriteMultiplePixels(&lcd, (const uint8_t *)&prepared_token, 0, 0, 0, 0);
}

static void driver_gray_checks(void)
{
    static uint8_t expected[EPD_GRAY_BYTES];
    uint16_t first[16], second[8];
    for (unsigned i = 0; i < 16; ++i) first[i] = i & 1u ? 0xffff : 0;
    for (unsigned i = 0; i < 8; ++i) second[i] = i & 1u ? 0 : 0xffff;
    CHECK(epd_display_prepare_init());
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset();
        memset(&prepared_dirty, 0, sizeof(prepared_dirty));
        memset(prepared_pixels, 0, EPD_GRAY_PREPARE_BYTES);
        prepared_busy = prepared_in_flight = false;
        fail_power = failure == 1;
        fail_send_at = failure == 2 ? 1 : 0;
        overflow_complete_at = failure == 3 ? 29 : 0;
        epd_display_prepare_chunk((const uint8_t *)first, 2, 0, 0, 0, 15);
        prepared_request(EPD_DRAW_MODE_AUTO);
        CHECK(!prepared_busy);
        /* Prepare the next pixels before the preceding completion releases its token. */
        epd_display_prepare_chunk((const uint8_t *)second, 2, 0, 128, 0, 135);
        epd_gray_dirty_t next_dirty = prepared_dirty;
        drain();
        CHECK(callbacks == 1 && frame_succeeded == (failure == 0));
        CHECK(prepared_complete(prepared_lcd, &prepared_token) == RT_EOK);
        CHECK(!memcmp(&prepared_dirty, &next_dirty, sizeof(next_dirty)));
        memset(expected, 0x77, sizeof(expected));
        for (unsigned i = 0; i < 16; ++i)
            expected[at(i)] = (uint8_t)((failure ? 0x70 : (i & 1u ? 0xf0 : 0)) | (i & 1u ? 15 : 0));
        CHECK(!memcmp(mixed, expected, sizeof(expected)));
        CHECK(frame_coverage[at(0) / 8u] == 0xff && frame_coverage[at(8) / 8u] == 0xff);
        fail_power = false; fail_send_at = overflow_complete_at = 0;
        unsigned before = sends;
        prepared_request(EPD_DRAW_MODE_AUTO);
        drain(); prepared_complete(prepared_lcd, &prepared_token);
        CHECK(callbacks == 2 && frame_succeeded && shadow_known);
        CHECK(sends - before == (failure >= 2 ? 41 + 44 : failure == 1 ? 44 : 29));
        for (unsigned i = 0; i < 16; ++i) expected[at(i)] = i & 1u ? 0xff : 0;
        for (unsigned i = 0; i < 8; ++i) expected[at(128 + i)] = i & 1u ? 0 : 0xff;
        CHECK(!memcmp(mixed, expected, sizeof(expected)));
        CHECK(frame_coverage[at(0) / 8u] == 0 && frame_coverage[at(128) / 8u] == 0xff);
        CHECK(!dirty_bytes());
    }
    free(prepared_pixels); prepared_pixels = NULL;
    puts("PASS: prepared full coverage, concurrent next preparation, power/send/final-EOF failure, whole-image recovery, exact frame/callback counts");
}
