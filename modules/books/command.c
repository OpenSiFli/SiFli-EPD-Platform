#include "bookshelf.h"
#include "books_ui.h"
#include "reader_controls.h"
#include "document/document_view.h"
#include "../common/reader/reader.h"
#include "../common/reader/compute/include/epd_reader_layout.h"
#include "epd_compute.h"
#include "platform/epd_app_font.h"
#include <string.h>

typedef enum { CMD_NONE, CMD_LIST, CMD_OPEN, CMD_NEXT, CMD_PREV,
               CMD_CHAPTER, CMD_BACK, CMD_STATE, CMD_SHELF_STATE, CMD_TOC } books_request_t;
/* Only the mailbox crosses threads. All listing and reader state below is UI-owned. */
static struct { books_request_t command; unsigned argument; } request;
static bool listing, listed;
static rt_tick_t list_started;
static uint32_t listed_revision, listed_session;
static bool toc_pending;
static rt_tick_t toc_started;
static lv_obj_t *toc_screen;
static char toc_identity[BOOKSHELF_PATH_MAX];
static uint32_t toc_revision, toc_session;

static void command_finish(const char *result)
{
    rt_kprintf("books command: %s\n", result);
    listing = false;
    if (toc_pending) {
        if (current && current->id == UI_PAGE_READER && current->screen == toc_screen &&
            reader_session && !strcmp(toc_identity, ui_reader_identity()) && !books_menu_active())
            epd_reader_toc_prepare(false);
        toc_pending = false;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    request.command = CMD_NONE;
    rt_hw_interrupt_enable(level);
}

static void command_post(books_request_t command, unsigned argument)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = request.command != CMD_NONE;
    if (!busy)
    {
        request.argument = argument;
        request.command = command;
        epd_service_ui_request(books_service, 0);
    }
    rt_hw_interrupt_enable(level);
    rt_kprintf("books command: %s\n", busy ? "busy; retry after completion" : "queued");
}

static bool command_index(int argc, char **argv, unsigned *value)
{
    if (argc != 2 || !argv[1][0]) return false;
    unsigned index = 0;
    for (const char *p = argv[1]; *p; ++p)
    {
        if (*p < '0' || *p > '9' || index > (UINT_MAX - (unsigned)(*p - '0')) / 10u)
            return false;
        index = index * 10u + (unsigned)(*p - '0');
    }
    *value = index;
    return true;
}

static void cmd_ui(int argc, char **argv)
{
    unsigned index = 0;
    books_request_t command;
    if (!strcmp(argv[0], "open") || !strcmp(argv[0], "chapter") || !strcmp(argv[0], "toc"))
    {
        bool toc = !strcmp(argv[0], "toc");
        if (!(toc && argc == 1) && !command_index(argc, argv, &index))
        {
            rt_kprintf("usage: %s <zero-based ASCII index>\n", argv[0]);
            return;
        }
        command = toc ? CMD_TOC : !strcmp(argv[0], "open") ? CMD_OPEN : CMD_CHAPTER;
    }
    else
    {
        if (argc != 1) { rt_kprintf("usage: %s\n", argv[0]); return; }
        command = !strcmp(argv[0], "list") ? CMD_LIST :
                  !strcmp(argv[0], "next") ? CMD_NEXT :
                  !strcmp(argv[0], "prev") ? CMD_PREV :
                  !strcmp(argv[0], "back") ? CMD_BACK : CMD_STATE;
    }
    command_post(command, index);
}

static void cmd_shelf_state(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) { rt_kprintf("usage: svc bookshelf state\n"); return; }
    command_post(CMD_SHELF_STATE, 0);
}

/* Called after the production worker's snapshot has been published by the UI. */
void books_command_scan_result(int error)
{
    listed = false;
    if (!listing) return;
    if (error) { command_finish("list failed; previous indices invalid"); return; }
    listed_revision = storage_revision();
    listed_session = storage_card_session();
    listed = true;
    rt_kprintf("bookshelf \"%s\": %u book(s)\n", bookshelf_dir(), ui_bookshelf_count());
    for (unsigned i = 0; i < ui_bookshelf_count(); ++i)
    {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(i);
        if (book) rt_kprintf("  [%u] %s  %u bytes  progress %u%%\n", i,
                            book->file.path, (unsigned)book->file.size,
                            (unsigned)book->position.progress);
    }
    command_finish("list complete; use svc bookshelf open <index>");
}

void books_command_ui_process(bool stopping)
{
    rt_base_t level = rt_hw_interrupt_disable();
    books_request_t command = request.command;
    unsigned index = request.argument;
    rt_hw_interrupt_enable(level);
    if (stopping) listed = false;
    if (command == CMD_NONE) return;
    if (stopping) { command_finish("cancelled: service stopping"); return; }
    if (command == CMD_SHELF_STATE)
    {
        if (current && current->id == UI_PAGE_BOOKSHELF) books_page_diagnostic();
        else rt_kprintf("bookshelf binding: inactive\n");
        command_finish("state complete");
        return;
    }
    if (command == CMD_STATE)
    {
        const ui_reader_view_t *view = ui_reader_view();
        rt_kprintf("reader UI: foreground=%u screen=%s session=%u ready=%u waiting=%u "
                   "page=%u pages=%u revision=%u indexing=%u chapters=%u error=%d display_idle=%u\n",
                   current && current->foreground,
                   !current ? "inactive" : current->id == UI_PAGE_READER ? "reader" : "bookshelf",
                   reader_session, view->ready, ui_reader_waiting(), view->page, view->pages,
                   (unsigned)view->revision, view->indexing, document_view_toc_count(),
                   view->error_code, lv_refreshing_done());
        rt_kprintf("reader file: %s\n", ui_reader_identity());
        char chapter[256];
        unsigned toc_index = UINT_MAX;
        bool has_chapter = epd_reader_chapter_title(chapter, sizeof(chapter), &toc_index);
        rt_kprintf("reader chapter: index=%u title=%s\n", toc_index,
                   has_chapter ? chapter : "");
        books_menu_diagnostic();
        books_page_diagnostic();
        command_finish("state complete");
        return;
    }
    if (command == CMD_TOC)
    {
        if (!current || !current->foreground || current->id != UI_PAGE_READER || !reader_session)
        { command_finish("rejected: reader is not foreground"); return; }
        if (storage_changing() || pending_page != NAV_IDLE || epd_reader_suspended() || ui_reader_view()->error_code)
        { command_finish("cancelled: reader unavailable"); return; }
        if (!toc_pending) {
            toc_pending = true; toc_started = rt_tick_get(); toc_screen = current->screen;
            snprintf(toc_identity, sizeof(toc_identity), "%s", ui_reader_identity());
            toc_revision = storage_revision(); toc_session = storage_card_session();
        } else if (current->screen != toc_screen || strcmp(toc_identity, ui_reader_identity()) ||
                   toc_revision != storage_revision() || toc_session != storage_card_session()) {
            command_finish("cancelled: reader changed"); return;
        }
        int navigation = epd_reader_toc_prepare(true);
        unsigned count = document_view_toc_count();
        if (index >= count && navigation == 0) {
            if ((rt_tick_t)(rt_tick_get() - toc_started) >= rt_tick_from_millisecond(30000))
                command_finish("toc timed out; retry toc");
            else epd_service_ui_request(books_service, 16);
            return;
        }
        rt_kprintf("reader chapters: %u indexing=%u\n", count, ui_reader_view()->indexing);
        if (index >= count) {
            command_finish(navigation < 0 ? "toc preparation failed" : "chapter index unavailable"); return;
        }
        for (unsigned i = index; i < count && i - index < 10u; ++i)
        {
            char title[256];
            unsigned depth;
            if (document_view_toc_title(i, title, sizeof(title), &depth))
                rt_kprintf("  [%u] depth=%u %s\n", i, depth, title);
        }
        command_finish("toc complete; use svc reader chapter <index>");
        return;
    }
    if (!current || !current->foreground)
    { command_finish("rejected: books UI is not foreground; unlock/open books first"); return; }
    if (storage_changing())
    { command_finish("rejected: storage changing"); return; }
    if (current->popup || pending_page != NAV_IDLE)
    { command_finish("rejected: modal or navigation busy"); return; }
    if (listing)
    {
        if ((rt_tick_t)(rt_tick_get() - list_started) >= rt_tick_from_millisecond(30000))
            command_finish("list timed out; retry list");
        return;
    }
    if (!lv_refreshing_done())
    { command_finish("rejected: display busy; retry"); return; }
    if (command == CMD_LIST || command == CMD_OPEN)
    {
        if (current->id != UI_PAGE_BOOKSHELF || reader_session)
        { command_finish("rejected: return to bookshelf and wait for reader close"); return; }
        if (command == CMD_LIST)
        {
            listed = false;
            if (ui_bookshelf_refresh() != RT_EOK)
            { command_finish("list could not be queued"); return; }
            listing = true;
            list_started = rt_tick_get();
            return;
        }
        if (!listed || listed_revision != storage_revision() || listed_session != storage_card_session())
        { command_finish("rejected: run svc bookshelf list for current indices"); return; }
        if (!ui_bookshelf_book(index))
        { command_finish("rejected: book index out of range"); return; }
        current->bookshelf_first = index / BOOKSHELF_PAGE_SIZE * BOOKSHELF_PAGE_SIZE;
        bookshelf_resume();
        pending_page = BOOK_OPEN_BASE + (int)(index % BOOKSHELF_PAGE_SIZE);
    }
    else
    {
        if (current->id != UI_PAGE_READER || !reader_session)
        { command_finish("rejected: reader is not open"); return; }
        if (command == CMD_BACK) pending_page = NAV_BACK;
        else
        {
            const ui_reader_view_t *view = ui_reader_view();
            if (current->reader_panel || current->reader_confirm_pending || current->reader_reflow_pending ||
                !view->ready || view->error_code || ui_reader_waiting() || ui_reader_settings_changed())
            { command_finish("rejected: reader busy or menu open; retry when ready"); return; }
            if (command == CMD_CHAPTER)
            {
                if (!document_view_toc_seek(index))
                { command_finish("rejected: chapter unavailable"); return; }
            }
            else pending_page = command == CMD_NEXT ? READER_NEXT : READER_PREVIOUS;
        }
    }
    lv_display_trigger_activity(NULL);
    command_finish("dispatched to UI; check svc reader state for completion");
}

static void cmd_rd_status(int argc, char **argv)
{
    epd_compute_capability_t capability;
    (void)argc; (void)argv;
    int result = epd_compute_query(EPD_READER_CAPABILITY, EPD_READER_VERSION, &capability);
    if (result != EPD_COMPUTE_OK) {
        rt_kprintf("reader layout: unavailable (%d)\n", result);
        return;
    }
    rt_kprintf("reader layout: %s v%u ready=%u flags=0x%x\n",
               EPD_READER_CAPABILITY, (unsigned)EPD_READER_VERSION,
               (unsigned)capability.ready, (unsigned)capability.registration.flags);
}

static void cmd_rd_diagnostic(int argc, char **argv)
{
    cmd_rd_status(argc, argv);
    epd_app_font_cache_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    epd_app_font_cache_stats(&stats);
    rt_kprintf("reader fonts: hits=%llu misses=%llu prepare_failures=%llu\n",
               (unsigned long long)stats.glyph_hits, (unsigned long long)stats.glyph_misses,
               (unsigned long long)stats.prepare_failures);
    rt_kprintf("reader fonts: bitmap_calls=%llu bitmap_ticks=%llu ticks_per_second=%u\n",
               (unsigned long long)stats.source_bitmap_calls, (unsigned long long)stats.source_bitmap_ticks,
               (unsigned)stats.ticks_per_second);
    epd_app_font_remote_stats_t remote;
    memset(&remote, 0, sizeof(remote));
    epd_app_font_remote_stats(&remote);
    rt_kprintf("reader remote-fonts: fonts=%u sources=%u pins=%u pixels=%u peak=%u descriptors=%u peak=%u hz=%u\n",
               remote.fonts, remote.sources, remote.pins, remote.pixel_bytes, remote.pixel_peak,
               remote.descriptor_bytes, remote.descriptor_peak, remote.ticks_per_second);
    rt_kprintf("reader remote-fonts: ready_hit=%llu miss=%llu draw_hit=%llu miss=%llu evict_pixels=%llu evict_desc=%llu\n",
               (unsigned long long)remote.ready_hits, (unsigned long long)remote.ready_misses,
               (unsigned long long)remote.draw_hits, (unsigned long long)remote.draw_misses,
               (unsigned long long)remote.pixel_evictions, (unsigned long long)remote.descriptor_evictions);
    rt_kprintf("reader remote-fonts: oom_budget=%llu reserve=%llu alloc=%llu postcheck=%llu\n",
               (unsigned long long)remote.oom_budget, (unsigned long long)remote.oom_reserve,
               (unsigned long long)remote.oom_alloc, (unsigned long long)remote.oom_postcheck);
    rt_kprintf("reader font-io: calls=%llu requested=%llu errors=%llu read_bytes=%llu read_ticks=%llu max=%u lock_wait=%llu max=%u lock_hold=%llu max=%u\n",
               (unsigned long long)remote.io.supply_calls, (unsigned long long)remote.io.requested_bytes,
               (unsigned long long)remote.io.errors, (unsigned long long)remote.io.read.bytes,
               (unsigned long long)remote.io.read.ticks, remote.io.read.max_ticks,
               (unsigned long long)remote.io.lock_wait.ticks, remote.io.lock_wait.max_ticks,
               (unsigned long long)remote.io.lock_hold.ticks, remote.io.lock_hold.max_ticks);
}


typedef struct
{
    const char *name, *usage;
    void (*handler)(int argc, char **argv);
} svc_cmd_t;
static const svc_cmd_t bookshelf_cmds[] =
{
    { "state", "query currently bound visible cards without scanning", cmd_shelf_state },
    { "list", "async worker scan; list zero-based book indices", cmd_ui },
    { "open", "<index> open a listed book through bookshelf UI", cmd_ui },
};

static const svc_cmd_t reader_cmds[] =
{
    { "status", "reader layout capability and readiness", cmd_rd_status },
    { "info", "reader layout capability and readiness", cmd_rd_status },
    { "diagnostic", "reader layout and font-cache counters", cmd_rd_diagnostic },
    { "state", "async visible reader state and file identity", cmd_ui },
    { "toc", "[index] list up to 10 zero-based chapter indices", cmd_ui },
    { "next", "next page through reader UI", cmd_ui },
    { "prev", "previous page through reader UI", cmd_ui },
    { "chapter", "<index> enter a zero-based chapter", cmd_ui },
    { "back", "return to bookshelf through UI navigation", cmd_ui },
};


int books_command(int argc, char **argv)
{
    const svc_cmd_t *commands;
    unsigned count;
    if (argc && !strcmp(argv[0], "bookshelf"))
    {
        commands = bookshelf_cmds;
        count = sizeof(bookshelf_cmds) / sizeof(bookshelf_cmds[0]);
    }
    else if (argc && !strcmp(argv[0], "reader"))
    {
        commands = reader_cmds;
        count = sizeof(reader_cmds) / sizeof(reader_cmds[0]);
    }
    else
    {
        rt_kprintf("svc books bookshelf|reader <command> [args]\n");
        return RT_EOK;
    }
    if (argc < 2)
    {
        for (unsigned i = 0; i < count; ++i)
            rt_kprintf("  %-10s %s\n", commands[i].name, commands[i].usage);
        return RT_EOK;
    }
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(commands[i].name, argv[1]))
        {
            commands[i].handler(argc - 1, argv + 1);
            return RT_EOK;
        }
    return -RT_ENOSYS;
}
