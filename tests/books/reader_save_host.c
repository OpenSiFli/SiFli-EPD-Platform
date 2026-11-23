#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK_MSG(x, message) do { if (!(x)) { fprintf(stderr, "line %u: %s: %s\n", __LINE__, message, #x); exit(1); } } while (0)
#define CHECK(x) CHECK_MSG(x, #x)
#define EPD_READER_PATH_MAX 256
#define EPD_READER_TITLE_MAX 128
#define BOOKSHELF_PATH_MAX EPD_READER_PATH_MAX
#define BOOKSHELF_MAX_ITEMS 32
#define HISTORY_MAGIC 0x45505233u
#define READER_ERROR_IO 1
#define RT_EOK 0
#define BOOKS_NOTICE_READER 2
typedef int reader_error_t;
typedef int lv_font_t;
typedef struct { char text[196]; } lv_obj_t;
typedef int document_t;
typedef struct { void *memory; } document_bitmap_t;
typedef int rt_base_t;
typedef int rt_int32_t;
typedef struct { int unused; } epd_service_t;
typedef enum { STORAGE_FLASH, STORAGE_SD } storage_volume_t;
#define STORAGE_SD_ROOT "/sdcard"
#define RT_WAITING_FOREVER -1
static struct { epd_service_t *service; } scan;
static uint32_t host_tick;
/* DECLARATIONS */
typedef reader_view_t ui_reader_view_t;
static ui_bookshelf_book_t book_array[2], *books = book_array, external_book;
static bool external_valid;
static unsigned count = 2;
static uint32_t order;
static unsigned char saved_slots[BOOKSHELF_MAX_ITEMS + 1];

static bool storage_ok = true, write_ok = true, waiting, live;
static unsigned writes, submitted, recent, body_renders, text_writes;
static history_t disk;
static char disk_path[64];
static void (*during_write)(void);
static document_t host_document;
static struct {
    reader_view_t info;
    struct bound_frame { reader_position_t position; } *bound;
    bool (*save)(void *, const reader_position_t *);
    void *user;
} ui;
static struct bound_frame frame;
static lv_obj_t footer;
static struct {
    lv_obj_t *reader_footer, *popup;
    bool reader_loading_shown, reader_has_content, reader_loading, reader_error_reported;
    uint32_t reader_revision, reader_loading_tick;
    unsigned reader_rendered_pages;
} page, *current = &page;
enum { reader_preparing, reader_paginating };
/* PROTOTYPES */
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_kprintf(const char *format, ...) { (void)format; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static unsigned rt_tick_from_millisecond(unsigned ms) { return ms; }
static uint32_t rt_tick_get(void) { return host_tick; }
static void epd_service_wake(epd_service_t *s) { (void)s; ++submitted; }
static void epd_service_ui_request(epd_service_t *s, unsigned ms) { (void)s; (void)ms; }
static uint32_t storage_card_session(void) { return 1; }
static bool storage_session_valid(storage_volume_t volume, uint32_t session) { (void)volume; return session == 1 && storage_ok; }
static const char *storage_path_root(const char *path) { (void)path; return "/flash"; }
static bool storage_app_available(const char *app) { CHECK(!strcmp(app, "books")); return storage_ok; }
static bool storage_file_replace(const char *path, const void *data, size_t size)
{
    CHECK(size == sizeof(disk)); ++writes;
    if (during_write) { void (*callback)(void) = during_write; during_write = NULL; callback(); }
    if (write_ok) { memcpy(&disk, data, size); snprintf(disk_path, sizeof(disk_path), "%s", path); }
    return write_ok;
}
static void format_last_read(ui_bookshelf_book_t *book) { (void)book; }
static const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index) { return index < count ? &books[index] : NULL; }
static void ui_bookshelf_cache_path(unsigned index, char *path, size_t size, const char *suffix)
{ snprintf(path, size, "/history/%u.%s", index, suffix); }
static const reader_view_t *epd_reader_get(void) { return &ui.info; }
static const reader_view_t *ui_reader_view(void) { return epd_reader_get(); }
static bool ui_reader_waiting(void) { return waiting; }
static void epd_app_set_recent_reading(const char *title, unsigned progress)
{ CHECK(title && progress <= 100); ++recent; }
static document_t *document_open_at(const bookshelf_item_t *file, const document_anchor_t *source, char *error, size_t size)
{ (void)file; (void)source; (void)error; (void)size; return &host_document; }
static void epd_reader_set_error_code(unsigned error) { (void)error; }
static unsigned document_open_error(void) { return 1; }
static void epd_reader_close(void)
{
    if (live) epd_reader_save();
    memset(&ui, 0, sizeof(ui)); live = false;
}
static bool epd_reader_attach(document_t *doc, const char *title, const reader_position_t *restore,
    bool (*save)(void *, const reader_position_t *), document_t *(*reopen)(void *, char *, size_t), void *user)
{
    CHECK(doc == &host_document && restore && reopen);
    ui.info.title = title; ui.save = save; ui.user = user; live = true;
    return true;
}
static bool document_view_active(void) { return live; }
static bool ui_reader_save(void) { return document_view_save(); }
static const char *epd_get_str(int id) { return id == reader_preparing ? "preparing" : "paginating"; }
static bool text_update(lv_obj_t *object, const char *text)
{
    if (strcmp(object->text, text)) { snprintf(object->text, sizeof(object->text), "%s", text); ++text_writes; }
    return true;
}
static void reader_render(void) { ++body_renders; }
static uint32_t lv_tick_elaps(uint32_t tick) { (void)tick; return 0; }
static void reader_panel_close(void) {}
static void popup_show(int notice, int error, int result) { (void)notice; (void)error; (void)result; }
/* PRODUCTION */
static void ready(unsigned page_number)
{
    ui.bound = &frame;
    frame.position = (reader_position_t){0};
    frame.position.source = (document_anchor_t){2, 7, 1234 + page_number, 3};
    frame.position.conversion_version = DOCUMENT_CONVERSION_VERSION;
    frame.position.layout = 17;
    ui.info.ready = true; ui.info.saved = false;
    ui.info.page = page_number; ui.info.pages = 10; ui.info.percent = 50;
}
static bool warning(void) { return strstr(footer.text, "保存失败") != NULL; }
static void queue_newer(void)
{
    ui_reading_position_t newer = books[0].position;
    ++newer.source.offset;
    CHECK(ui_bookshelf_save(0, &newer));
}
static void background_at(unsigned line)
{
    host_tick += HISTORY_COALESCE_MS;
    if (!history_write(NULL, NULL)) {
        fprintf(stderr, "line %u: expected a pending history write\n", line);
        exit(1);
    }
}
#define background() background_at(__LINE__)
int main(void)
{
    current->reader_footer = &footer;
    strcpy(books[0].file.name, "one.epub"); strcpy(books[1].file.name, "two.epub");
    strcpy(books[0].file.path, "/flash/one.epub"); strcpy(books[1].file.path, "/flash/two.epub");
    books[0].file.format = books[1].file.format = 2;
    CHECK(document_view_open(0));
    CHECK(!epd_reader_save() && !document_view_save_failed());
    CHECK(!document_view_save() && !document_view_save_failed());
    ready(0); CHECK(reader_footer_render());
    CHECK_MSG(!warning() && !strcmp(footer.text, "50%"), "pending footer");
    ready(1); CHECK(reader_footer_render() && !warning());
    ui.info.pages = 0; CHECK(reader_footer_render() && !warning());
    ui.info.ready = false; current->reader_has_content = current->reader_loading_shown = true;
    CHECK(reader_footer_render() && !strcmp(footer.text, "preparing"));
    ready(1); frame.position.source.unit = 0;
    CHECK(!epd_reader_save() && !document_view_save_failed());
    ready(1); CHECK(epd_reader_save() && writer.count == 1 && !writes && !document_view_save_failed());
    CHECK(books[0].position.source.unit == 2 && books[0].position.source.chapter == 7);
    CHECK(books[0].position.source.offset == 1235 && books[0].position.source.skip_utf8 == 3);
    CHECK(books[0].position.current_page == 1 && books[0].position.total_pages == 10);
    unsigned queued = submitted;
    CHECK(epd_reader_save() && submitted == queued);
    background(); CHECK(!writer.count && !document_view_save_failed() && writes == 1);
    CHECK(disk.position.source.offset == 1235 && disk.position.current_page == 1);
    CHECK(strstr(disk_path, ".b") && disk.checksum == hash(&disk, offsetof(history_t, checksum)));
    puts("PASS pending/loading footer, autosave deduplication and persisted source anchor");

    ready(2); CHECK(epd_reader_save());
    write_ok = false; background();
    CHECK(writer.count && document_view_save_failed() && ui_bookshelf_worker_delay() == RT_WAITING_FOREVER);
    CHECK(reader_footer_render() && warning());
    ui.info.page = 0; CHECK(reader_footer_render() && warning());
    ui.info.page = 2; ui.info.indexing = true; waiting = true;
    write_ok = true; ui_bookshelf_history_retry(); background(); reader_footer_render();
    CHECK(!warning() && !document_view_save_failed());
    unsigned unchanged = text_writes; reader_footer_render(); CHECK(text_writes == unchanged);
    ready(3); CHECK(epd_reader_save()); write_ok = false; background();
    reader_footer_render(); CHECK(warning());
    ready(4); CHECK(epd_reader_save() && document_view_save_failed());
    write_ok = true; ui_bookshelf_history_retry(); during_write = queue_newer; background();
    CHECK(writer.count && document_view_save_failed());
    background(); reader_footer_render(); CHECK(!warning() && !document_view_save_failed());
    puts("PASS failure/retry footer and superseded-write completion");

    ready(5); storage_ok = false;
    CHECK(document_view_save() && !document_view_save_failed());
    background(); CHECK(document_view_save_failed() && ui_bookshelf_flush());
    reader_footer_render(); CHECK(warning());
    storage_ok = true; ui_bookshelf_history_retry(); background(); reader_footer_render(); CHECK(!warning());
    ready(6); write_ok = false;
    CHECK(document_view_save()); background(); CHECK(document_view_save_failed());
    CHECK(document_view_save() && ui_bookshelf_flush());
    write_ok = true; ui_bookshelf_history_retry(); background(); CHECK(ui_bookshelf_flush() && !document_view_save_failed());
    ready(8); unsigned before = writes; reader_save_position();
    CHECK(writer.count && writes == before); background(); CHECK(disk.position.current_page == 8);
    ready(9); document_view_close();
    CHECK(writer.count && !live && !document_view_save_failed());
    CHECK(!ui_bookshelf_flush()); background(); CHECK(ui_bookshelf_flush() && disk.position.current_page == 9);
    puts("PASS asynchronous storage failure/recovery and pause/close saving");

    CHECK(document_view_open(0)); ready(1); CHECK(epd_reader_save());
    write_ok = false; background(); CHECK(document_view_save_failed());
    CHECK(document_view_open(1) && !document_view_save_failed());
    ready(2); CHECK(epd_reader_save() && !document_view_save_failed() && writer.count == 2);
    write_ok = true; ui_bookshelf_history_retry(); background(); background();
    CHECK(!writer.count && disk.position.current_page == 2);
    CHECK(!document_view_open(99) && !document_view_save_failed());
    CHECK(recent > 0);
    puts("PASS cross-book failure isolation and independent pending records");
    return 0;
}
