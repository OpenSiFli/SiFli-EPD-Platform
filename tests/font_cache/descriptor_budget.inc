/* Populate the real sparse cache through glyph queries, including its final
 * partial page, until neither a page nor a glyph block fits the hard budget. */
static void fill_descriptor_budget(lv_font_t *font)
{
    lv_font_glyph_dsc_t glyph;
    for(uint32_t letter = 0x100; letter < 0x110000u; letter += 16) {
        assert(lv_font_get_glyph_dsc(font, &glyph, letter, 0));
        assert(descriptor_bytes <= DESCRIPTOR_BUDGET);
        size_t remaining = DESCRIPTOR_BUDGET - descriptor_bytes;
        if(remaining < sizeof(descriptor_page_t) && remaining < 16u * sizeof(glyph)) break;
    }
    assert(DESCRIPTOR_BUDGET - descriptor_bytes < sizeof(descriptor_page_t));
    assert(DESCRIPTOR_BUDGET - descriptor_bytes < UNICODE_PAGES * sizeof(void *));
}

static void descriptor_budget_replacement(void)
{
    lv_font_t original = make_font(); original.line_height = 32;
    lv_font_t *old = &original; assert(ui_reader_font_cache_attach(old));
    lv_font_t *oblique = cache_test_derived(old); assert(oblique);
    assert(ui_reader_font_prepare(old, 65, true));
    assert(ui_reader_font_prepare(old, 66, true));
    assert(ui_reader_font_prepare(oblique, 65, true));
    lv_font_glyph_dsc_t held, derived, measured;
    assert(lv_font_get_glyph_dsc(old, &held, 65, 0));
    const lv_draw_buf_t *pixels = lv_font_get_glyph_bitmap(&held, NULL); assert(pixels);
    assert(lv_font_get_glyph_dsc(oblique, &derived, 65, 0));
    const lv_draw_buf_t *slanted = lv_font_get_glyph_bitmap(&derived, NULL); assert(slanted);
    assert(lv_font_get_glyph_dsc(old, &measured, 66, 0));
    size_t retained_bytes = glyph_bytes;
    unsigned retained_count = glyph_count, rasterizations = source_calls;
    for(unsigned pass = 0; pass < 3; ++pass) {
        fill_descriptor_budget(old);
        /* A distinct face forces descriptor-root allocation under pressure. */
        lv_font_t next = make_font(); next.line_height = 48;
        lv_font_t *replacement = &next; assert(ui_reader_font_cache_attach(replacement));
        assert(replacement && replacement->line_height == 48 && old->line_height == 32);
        assert(descriptor_bytes <= DESCRIPTOR_BUDGET);
        assert(glyph_bytes == retained_bytes && glyph_count == retained_count);
        assert(!lv_cache_entry_is_invalid(held.entry));
        assert(pixels->data[0] == 72);
        assert(slanted->data[(derived.box_h - 1) * slanted->header.stride] == 72);
        /* A descriptor copied before reclamation still resolves warm pixels. */
        lv_font_glyph_dsc_t copy = measured;
        assert(lv_font_get_glyph_bitmap(&copy, NULL));
        lv_font_glyph_release_draw_data(&copy);
        draw(old, 65); draw(old, 66);
        assert(source_calls == rasterizations);
        lv_font_glyph_dsc_t current;
        assert(lv_font_get_glyph_dsc(replacement, &current, 65, 0));
        assert(current.resolved_font == replacement);
        ui_reader_font_cache_detach(replacement);
        assert(lv_font_get_glyph_dsc(old, &current, 66, 0));
        assert(current.adv_w == measured.adv_w && current.gid.index == measured.gid.index);
        unsigned calls = descriptor_calls;
        assert(lv_font_get_glyph_dsc(old, &current, 66, 0));
        assert(descriptor_calls == calls);
    }
    lv_font_glyph_release_draw_data(&held);
    lv_font_glyph_release_draw_data(&derived);
    cache_test_free(oblique);
    ui_reader_font_cache_detach(old);
}

static void descriptor_root_only_limit(void)
{
    enum { COUNT = DESCRIPTOR_BUDGET / (UNICODE_PAGES * sizeof(void *)) };
    lv_font_t fonts[COUNT + 1];
    for(unsigned i = 0; i <= COUNT; ++i) fonts[i] = make_font();
    for(unsigned i = 0; i < COUNT; ++i) assert(ui_reader_font_cache_attach(&fonts[i]));
    size_t bytes = descriptor_bytes;
    assert(!ui_reader_font_cache_attach(&fonts[COUNT]));
    assert(descriptor_bytes == bytes && descriptor_bytes <= DESCRIPTOR_BUDGET);
    assert(fonts[COUNT].get_glyph_dsc == source_descriptor);
    ui_reader_font_cache_detach(&fonts[0]);
    assert(ui_reader_font_cache_attach(&fonts[COUNT]));
    for(unsigned i = 1; i <= COUNT; ++i) ui_reader_font_cache_detach(&fonts[i]);
}

static void descriptor_root_shared_reserve(void)
{
    lv_font_t old = make_font(), replacement = make_font();
    assert(ui_reader_font_cache_attach(&old));
    size_t bytes = descriptor_bytes;
    available = epd_app_allocation_peak(UNICODE_PAGES * sizeof(void *)) - 1;
    assert(!ui_reader_font_cache_attach(&replacement));
    assert(descriptor_bytes == bytes);
    ++available;
    assert(ui_reader_font_cache_attach(&replacement));
    available = 64u * 1024u * 1024u;
    ui_reader_font_cache_detach(&replacement); ui_reader_font_cache_detach(&old);
}

static void descriptor_root_allocation_failure(void)
{
    lv_font_t old = make_font(), replacement = make_font();
    assert(ui_reader_font_cache_attach(&old));
    assert(ui_reader_font_prepare(&old, 65, true));
    fill_descriptor_budget(&old);
    allocation_fails = true;
    assert(!ui_reader_font_cache_attach(&replacement));
    assert(replacement.get_glyph_dsc == source_descriptor);
    assert(descriptor_bytes <= DESCRIPTOR_BUDGET);
    draw(&old, 65);
    allocation_fails = false;
    assert(ui_reader_font_cache_attach(&replacement));
    draw(&old, 65); draw(&replacement, 66);
    ui_reader_font_cache_detach(&old); ui_reader_font_cache_detach(&replacement);
}

#ifdef _WIN32
static HANDLE descriptor_entered, descriptor_continue, attach_entered;
static lv_font_t *query_font, *attach_font;
static void pause_descriptor(void)
{
    SetEvent(descriptor_entered);
    assert(WaitForSingleObject(descriptor_continue, 5000) == WAIT_OBJECT_0);
}
static DWORD WINAPI query_descriptor_thread(void *unused)
{
    (void)unused;
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(query_font, &glyph, 66, 0));
    return 0;
}
static DWORD WINAPI attach_replacement_thread(void *unused)
{
    (void)unused;
    SetEvent(attach_entered);
    assert(ui_reader_font_cache_attach(attach_font));
    return 0;
}
static void descriptor_reclaim_waits_callback(void)
{
    lv_font_t old = make_font(), replacement = make_font();
    assert(ui_reader_font_cache_attach(&old));
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&old, &glyph, 65, 0));
    fill_descriptor_budget(&old);
    query_font = &old; attach_font = &replacement;
    descriptor_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    descriptor_continue = CreateEvent(NULL, TRUE, FALSE, NULL);
    attach_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(descriptor_entered && descriptor_continue && attach_entered);
    descriptor_hook = pause_descriptor;
    /* 66 misses inside 65's existing page: cached_descriptor retains that page
     * locally while its source callback is paused with descriptor_lock free. */
    HANDLE query = CreateThread(NULL, 0, query_descriptor_thread, NULL, 0, NULL); assert(query);
    assert(WaitForSingleObject(descriptor_entered, 5000) == WAIT_OBJECT_0);
    HANDLE attach = CreateThread(NULL, 0, attach_replacement_thread, NULL, 0, NULL); assert(attach);
    assert(WaitForSingleObject(attach_entered, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(attach, 100) == WAIT_TIMEOUT);
    /* Attach must not hold metadata while waiting for the callback mutex. */
    assert(TryEnterCriticalSection(&descriptor_lock.native));
    LeaveCriticalSection(&descriptor_lock.native);
    SetEvent(descriptor_continue);
    assert(WaitForSingleObject(query, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(attach, 5000) == WAIT_OBJECT_0);
    descriptor_hook = NULL;
    assert(lv_font_get_glyph_dsc(&old, &glyph, 66, 0));
    assert(lv_font_get_glyph_dsc(&replacement, &glyph, 66, 0));
    assert(descriptor_bytes <= DESCRIPTOR_BUDGET);
    CloseHandle(query); CloseHandle(attach);
    CloseHandle(descriptor_entered); CloseHandle(descriptor_continue); CloseHandle(attach_entered);
    ui_reader_font_cache_detach(&replacement); ui_reader_font_cache_detach(&old);
}
#endif
