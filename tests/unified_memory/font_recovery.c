#define main memory_test_main
#include "host.c"
#undef main
#include "../../src/ui/ui_font_memory.h"

int main(void)
{
    setup(); assert(epd_memory_init() == EPD_MEMORY_OK);
    epd_memory_owner_t owner = epd_memory_owner_create("font-caller");
    epd_memory_owner_t previous = epd_memory_owner_enter(owner);
    epd_memory_recovery_t state = {0};
    assert(epd_app_psram_available() < 1024u * 1024u);
    assert(ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    void *pixels = epd_app_alloc(4096, EPD_APP_PSRAM); assert(pixels);
    ui_font_memory_result(&state, true); assert(!state.used && !pressure_calls);
    epd_app_free(pixels);
    epd_memory_credit_t credit = request(owner, 64, 0, EPD_MEMORY_OK);
    epd_memory_owner_t old = epd_memory_owner_enter(credit);
    assert(ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    pixels = epd_app_alloc(4096, EPD_APP_PSRAM); assert(pixels && header(pixels)->owner == credit);
    ui_font_memory_result(&state, true); epd_app_free(pixels);
    epd_memory_credit_end(credit);
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    assert(state.result == EPD_MEMORY_CLOSED && !pressure_calls);
    epd_memory_owner_leave(old); epd_memory_owner_release(credit);
    memset(&state, 0, sizeof(state));
    assert(epd_memory_owner_set_limit(owner, backends[0], 128) == EPD_MEMORY_OK);
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    assert(state.result == EPD_MEMORY_LIMIT && !pressure_calls);
    assert(epd_memory_owner_set_limit(owner, backends[0], SIZE_MAX) == EPD_MEMORY_OK);
    memset(&state, 0, sizeof(state));
    epd_memory_owner_leave(previous);
    pixels = epd_app_alloc(30000, EPD_APP_PSRAM); assert(pixels);
    assert(epd_memory_owner_enter(owner) == previous);
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)) && pressure_calls == 1);
    for (unsigned i = 0; i < 10; ++i) assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    assert(pressure_calls == 1);
    epd_app_free(pixels); pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    pixels = epd_app_alloc(4096, EPD_APP_PSRAM); assert(pixels);
    ui_font_memory_result(&state, true); epd_app_free(pixels);
    epd_memory_owner_leave(previous);
    pixels = epd_app_alloc(30000, EPD_APP_PSRAM); assert(pixels);
    assert(epd_memory_owner_enter(owner) == previous);
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)));
    ticks += 5u * RT_TICK_PER_SECOND;
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)) && state.result == EPD_MEMORY_TIMEOUT);
    assert(!ui_font_memory_begin(&state, epd_app_allocation_peak(4096)) && pressure_calls == 2);
    epd_app_free(pixels); epd_memory_owner_leave(previous); epd_memory_owner_release(owner); audit();
    puts("PASS font production admission: low-memory/credit/closed/limit/recovery/deadline");
    return 0;
}
