#define main remote_fixture_main
#include "../reader_font_remote/host.c"
#undef main
#include "font_chain.inc"
#include <crtdbg.h>

/* No UI or application service is subscribed in the host process. */
void app_ui_signal(uint32_t events) { worker_only(); assert(events == EPD_SERVICE_UI_FONT_PREVIEW); }
void app_service_ui_signal(uint32_t events) { worker_only(); assert(events == EPD_SERVICE_UI_FONT); }

static const test_t chain_cases[] = {
    {"reader_async_sizes", reader_async_sizes},
    {"reader_async_error_cleanup", reader_async_error_cleanup},
    {"reader_revoked_cached_source", reader_revoked_cached_source},
    {"reader_remote_oblique", reader_remote_oblique}
};
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    assert(argc == 2);
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(chain_cases) / sizeof(*chain_cases); ++i) puts(chain_cases[i].name);
        return 0;
    }
    ui_thread = GetCurrentThreadId(); InitializeConditionVariable(&transition); InitializeCriticalSection(&interrupt_gate);
    io_entered = CreateEventA(NULL, TRUE, FALSE, NULL); io_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (unsigned i = 0; i < sizeof(chain_cases) / sizeof(*chain_cases); ++i) if (!strcmp(argv[1], chain_cases[i].name)) {
        chain_cases[i].run(); idle();
        assert(!fonts && !font_count && !pixel_bytes && !descriptor_bytes && !allocations && !leases && !pipe_count);
        assert(!sources && !resident_sources);
        printf("PASS %s\n", chain_cases[i].name); return 0;
    }
    return 2;
}
