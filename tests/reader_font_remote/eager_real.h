/* SPDX-License-Identifier: Apache-2.0 */
#if defined(REAL_FONT_PIPE) && UI_READER_FONT_EAGER
static void real_eager_faults(void)
{
    const char text[] = "\xe4\xb8\xad\xe6\x96\x87";
    lv_font_t *font = create(0); ready_font(font);
    real_allocation_fault(true);
    assert(ui_reader_font_remote_prepare(font, text, sizeof(text)-1, false) == 0); idle();
    assert(ui_reader_font_remote_prepare(font, text, sizeof(text)-1, false) == 1);
    assert(remote(font)->batches[1].count == 1 && remote(font)->batches[1].fallbacks == 2);
    assert(remote(font)->batches[0].glyphs == 2 && !remote(font)->deferred);
    assert(lookup(remote(font), 0x4e2d)->descriptor && !lookup(remote(font), 0x4e2d)->pixels);
    for (unsigned i = 0; i < 10; ++i) {
        assert(ui_reader_font_remote_prepare(font, text, sizeof(text)-1, false) == 1); idle();
    }
    assert(remote(font)->batches[1].count == 1);
    real_allocation_fault(false);
    prepare(font, text, true); assert(ui_reader_font_remote_ready(font, 0x4e2d, 0, true) == 1);
    destroy(font); real_no_buffers();
    puts("PASS real eager OOM: persistent ACPU allocation failure -> one MEASURE fallback -> pixels recover");
    const unsigned operations[] = {EPD_PIPE_READ, EPD_PIPE_OUTPUT};
    for (unsigned i = 0; i < sizeof(operations)/sizeof(*operations); ++i) {
        font = create(0); ready_font(font);
        ResetEvent(io_entered); ResetEvent(io_continue); hold_eager_operation = operations[i];
        assert(ui_reader_font_remote_prepare(font, text, sizeof(text)-1, false) == 0);
        assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
        ui_reader_font_remote_cancel(font); SetEvent(io_continue); idle();
        assert(ui_reader_font_remote_status(font) == -1);
        assert(ui_reader_font_remote_prepare(font, text, sizeof(text)-1, false) == -1);
        destroy(font); real_no_buffers();
        assert(!fonts && !sources && !allocations && !descriptor_bytes && !pixel_bytes);
    }
    puts("PASS real eager cancel: READ and OUTPUT cancellation release all buffers/glyphs");
}
#endif
