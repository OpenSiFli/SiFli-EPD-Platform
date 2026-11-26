static void prepared_request(EpdDrawMode mode)
{
    prepared_mode = mode;
    prepared_busy = prepared_in_flight = true;
    LCD_WriteMultiplePixels(&lcd, (const uint8_t *)&prepared_token, 0, 0, 0, 0);
}

static void driver_landscape_checks(uint8_t *expected)
{
    /* This span starts beyond portrait width and crosses a dirty-block boundary.
     * Its ends exercise single-pixel writes around a full eight-pixel group. */
    enum { X0 = 701, Y0 = 11, W = 18, H = 3, STRIDE = W * 2 + 5 };
    static const struct { uint16_t rgb; uint8_t gray; } colors[] = {
        {0x0000, 0}, {0xffff, 15}, {0xf800, 4}, {0x07e0, 9}, {0x001f, 1},
        {0xffe0, 13}, {0x07ff, 10}, {0xf81f, 6}, {0x8410, 8}, {0x7bef, 7},
    };
    _Alignas(4) uint8_t input[1 + H * STRIDE];
    uint8_t original[sizeof(input)];
    uint8_t *source = input + 1;
    epd_gray_dirty_t expected_dirty = {0};

    reset();
    memset(input, 0xe6, sizeof(input));
    memset(prepared_pixels, 0xa5, EPD_GRAY_BYTES / 2u);
    memset(expected, 0x77, EPD_GRAY_BYTES);
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            unsigned color = (x + 3u * y) % (sizeof(colors) / sizeof(colors[0]));
            memcpy(source + y * STRIDE + x * 2u, &colors[color].rgb, sizeof(colors[color].rgb));
            unsigned index = (Y0 + y) * PANEL_WIDTH + X0 + x;
            expected[index] = (uint8_t)(17u * colors[color].gray);
            unsigned block = index / EPD_GRAY_BLOCK_BYTES;
            expected_dirty.blocks[block / 32u] |= 1u << (block % 32u);
        }
    }
    memcpy(original, input, sizeof(input));
    epd_display_prepare_landscape(source, STRIDE, X0, Y0, X0 + W - 1, Y0 + H - 1);
    CHECK(!memcmp(input, original, sizeof(input)));
    CHECK(!memcmp(&prepared_dirty, &expected_dirty, sizeof(expected_dirty)));
    const uint8_t *coverage = prepared_pixels + EPD_GRAY_BYTES / 2u;
    for (unsigned y = 0; y < PANEL_HEIGHT; ++y) {
        for (unsigned x = 0; x < PANEL_WIDTH; ++x) {
            unsigned index = y * PANEL_WIDTH + x;
            bool covered = x >= X0 && x < X0 + W && y >= Y0 && y < Y0 + H;
            unsigned shift = (index & 1u) * 4u;
            unsigned gray = covered ? expected[index] & 15u : (0xa5u >> shift) & 15u;
            CHECK(((prepared_pixels[index / 2u] >> shift) & 15u) == gray);
            CHECK(((coverage[index / 8u] >> (index % 8u)) & 1u) == (unsigned)covered);
        }
    }

    prepared_request(EPD_DRAW_MODE_PARTIAL);
    CHECK(!memcmp(&frame_dirty, &expected_dirty, sizeof(expected_dirty)));
    drain();
    CHECK(prepared_complete(prepared_lcd, &prepared_token) == RT_EOK);
    CHECK(frame_succeeded && shadow_known && callbacks == 1 && !dirty_bytes());
    CHECK(!memcmp(mixed, expected, EPD_GRAY_BYTES));
    puts("PASS: landscape padded RGB565, exact native gray indices, coverage and dirty blocks, untouched pixels");
}

static void driver_gray_checks(void)
{
    static uint8_t expected[EPD_GRAY_BYTES];
    uint16_t first[16], second[8];
    for (unsigned i = 0; i < 16; ++i) first[i] = i & 1u ? 0xffff : 0;
    for (unsigned i = 0; i < 8; ++i) second[i] = i & 1u ? 0 : 0xffff;
    CHECK(epd_display_prepare_init());
    driver_landscape_checks(expected);
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
