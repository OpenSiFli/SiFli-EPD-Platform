/* Exercise the real mailbox-to-UI result path without a display driver. */
static struct {
    void *session;
    bool paused,pending,configuration_failed;
    unsigned id,intent,requested_at;
    int turn_delta;
    reader_view_t info;
    view_frame_t *ready,*bound;
} ui;
static struct { void *context; view_command_t command; } font_request;
static void ui_configuration_rejected(void) { assert(false); }
static void epd_reader_note_close(void) {}
static void frame_ref(view_frame_t *frame) { assert(frame==ui.bound); }
static void turn_cancel(void) {}
static void view_cancel(void) {}
static void wake_view(void) {}
static unsigned elapsed_ms(unsigned started) { return clock_ticks-started; }
static char visible_chapter[64];
static void epd_reader_collect(void) {}
static bool epd_reader_suspended(void) { return mailbox.suspended; }
static bool epd_reader_settings_changed(void) { return false; }
static bool epd_reader_reflow(void) { assert(false);return false; }
static bool font_request_process(void) { assert(false);return false; }
static void epd_reader_set_error_code(reader_error_t error) { ui.info.error_code=error; }
static bool cursor_equal(document_cursor_t a,document_cursor_t b) { return a.record==b.record && a.byte==b.byte; }
static void epd_reader_save(void) { assert(false); }
static void document_current_title(void *document,unsigned record,char *out,size_t size,void *unused)
{ (void)document;(void)record;(void)out;(void)size;(void)unused;assert(false); }
