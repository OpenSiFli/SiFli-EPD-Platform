/* SPDX-License-Identifier: Apache-2.0 */
static void reclaim_roundtrip(void)
{
    lv_font_t *font = create(0); ready_font(font); prepare(font, "AB", true);
    epd_app_font_glyph_t held = {0}, original = {0}, restored = {0};
    assert(ui_reader_font_remote_acquire(font, 'A', 0, true, &held) == 1);
    assert(ui_reader_font_remote_acquire(font, 'B', 0, true, &original) == 1);
    size_t bytes = original.stride * original.box_h;
    assert(bytes && original.pixels);
    uint8_t *copy = malloc(bytes); assert(copy); memcpy(copy, original.pixels, bytes);
    uint32_t advance = original.adv_w, width = original.box_w, height = original.box_h;
    int32_t x = original.ofs_x, y = original.ofs_y;
    glyph_t *g = lookup(remote(font), 'B'); size_t charge = g->charge;
    ui_reader_font_remote_release(&original);
    uint64_t measures = remote(font)->batches[0].count, renders = remote(font)->batches[1].count;
    size_t descriptors = descriptor_bytes;
    assert(provider_reclaim_target(epd_app_psram_available() + 1) == charge);
    assert(g->descriptor && !g->pixels && !g->wanted && !g->busy);
    assert(descriptor_bytes == descriptors && held.pixels);
    for (unsigned i = 0; i < 10; ++i) {
        assert(ui_reader_font_remote_prepare(font, "AB", 2, false) == 1); idle();
    }
    assert(remote(font)->batches[0].count == measures && remote(font)->batches[1].count == renders);
    prepare(font, "B", true);
    assert(remote(font)->batches[0].count == measures && remote(font)->batches[1].count == renders + 1);
    assert(ui_reader_font_remote_acquire(font, 'B', 0, true, &restored) == 1);
    assert(restored.adv_w == advance && restored.box_w == width && restored.box_h == height);
    assert(restored.ofs_x == x && restored.ofs_y == y && restored.stride * restored.box_h == bytes);
    assert(!memcmp(copy, restored.pixels, bytes)); free(copy);
    ui_reader_font_remote_release(&held); ui_reader_font_remote_release(&restored); destroy(font);
    puts("PASS reclaim roundtrip: metrics retained, no automatic render, explicit render restores identical pixels");
}

#ifndef REAL_FONT_PIPE
static void reclaim_inert(void)
{
    assert(!initialized);
    size_t before = epd_app_psram_available();
    assert(!ui_reader_font_remote_reclaim_pixels(SIZE_MAX));
    assert(!initialized && !allocations && epd_app_psram_available() == before);
    lv_font_t *font = create(0); ready_font(font); prepare(font, "A", true);
    before = pixel_bytes; fail_alloc = 1;
    assert(!ui_reader_font_remote_reclaim_pixels(0));
    assert(!ui_reader_font_remote_reclaim_pixels(epd_app_psram_available()));
    assert(pixel_bytes == before && fail_alloc == 1); fail_alloc = 0;
    destroy(font);
}

static void reclaim_frame_pressure(void)
{
    glyph_side = 64;
    lv_font_t *font = create(0); ready_font(font);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 1000, 112, true); idle();
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    for (uint32_t c = 1000; c < 1008; ++c)
        assert(ui_reader_font_remote_page_pin(page, font, c, 0, true) == 1);
    size_t charge = lookup(remote(font), 1000)->charge;
    const size_t frame_bytes = 1381360, reserve = 1024u * 1024u;
    const size_t target = frame_bytes + reserve + 64u;
    available_bytes = 2194308;
    size_t shortage = target - epd_app_psram_available();
    size_t victims = (shortage + charge + 31u) / (charge + 32u);
    size_t descriptors = descriptor_bytes, pixels = pixel_bytes;
    epd_app_font_remote_stats_t before, after;
    ui_reader_font_remote_stats(&before);
    LONG calls = commands, reads = raw_reads + pack_reads;
    size_t released = provider_reclaim_target(target);
    assert(released == victims * charge && pixel_bytes == pixels - released);
    assert(epd_app_psram_available() >= target && epd_app_psram_available() < target + charge + 32u);
    assert(descriptor_bytes == descriptors && commands == calls && raw_reads + pack_reads == reads);
    ui_reader_font_remote_stats(&after);
    assert(after.pixel_evictions == before.pixel_evictions + victims);
    assert(after.descriptor_evictions == before.descriptor_evictions);
    for (uint32_t c = 1000; c < 1112; ++c) {
        glyph_t *g = lookup(remote(font), c); assert(g && g->descriptor && !g->wanted);
        assert(g->pixels == (c < 1008 || c >= 1008 + victims));
    }
    assert(!provider_reclaim_target(target));
    void *frame = epd_app_alloc(frame_bytes, EPD_APP_PSRAM); assert(frame);
    assert(epd_app_psram_available() >= reserve); epd_app_free(frame);
    printf("Frame pressure: shortage=%zu reclaimed=%zu glyphs=%zu free=%zu; frame allocation preserves reserve\n",
           shortage, released, victims, epd_app_psram_available());
    available_bytes = 64u * 1024u * 1024u;
    ui_reader_font_remote_page_release(page); destroy(font);
}

static void reclaim_protected_cancel(void)
{
    lv_font_t *font = create(0); ready_font(font); prepare(font, "ABCD", true);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    assert(ui_reader_font_remote_page_pin(page, font, 'A', 0, true) == 1);
    epd_app_font_glyph_t held = {0};
    assert(ui_reader_font_remote_acquire(font, 'B', 0, true, &held) == 1);
    lv_font_glyph_dsc_t d = descriptor(font, 'C');
    const lv_draw_buf_t *drawing = font->get_glyph_bitmap(&d, NULL); assert(drawing);
    size_t charge = lookup(remote(font), 'D')->charge;
    observe_start(font, true);
    assert(ui_reader_font_remote_ready(font, 'E', 0, true) == 0);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(ui_reader_font_remote_ready(font, 'F', 0, true) == 0);
    assert(provider_reclaim_target(SIZE_MAX) == charge + sizeof(glyph_t));
    assert(!provider_reclaim_target(SIZE_MAX));
    lock();
    assert(lookup(remote(font), 'A')->pixels && lookup(remote(font), 'B')->pixels);
    assert(lookup(remote(font), 'C')->pixels && !lookup(remote(font), 'D'));
    assert(lookup(remote(font), 'E')->busy && lookup(remote(font), 'F')->wanted);
    unlock();
    assert(held.pixels[0] == 0xab && drawing->data[0] == 0xab);
    ui_reader_font_remote_cancel(font); ui_reader_font_remote_destroy(font);
    SetEvent(batch_continue); observe_end();
    assert(!provider_reclaim_target(SIZE_MAX));
    assert(held.pixels[0] == 0xab && drawing->data[0] == 0xab);
    ui_reader_font_remote_page_release(page);
    ui_reader_font_remote_release(&held); font->release_glyph(font, &d); idle();
    assert(!fonts && !sources && !allocations && !pixel_bytes && !descriptor_bytes);
}
#endif
