#include <assert.h>
#include <stdlib.h>
#include "../../src/platform/epd_compute.c"
const epd_compute_registration_t epd_compute_capabilities[] = {{"test", 1, 7, 3, 1}};
const uint32_t epd_compute_capability_count = 1;
void *epd_app_alloc(size_t size, epd_app_memory_t region) { (void)region; return malloc(size); }
void epd_app_free(void *p) { free(p); }
int main(void)
{
    epd_compute_capability_t c;
    epd_compute_session_t s = NULL;
    epd_compute_wait_t w = NULL;
    assert(epd_compute_query("test", 1, &c) == EPD_COMPUTE_OK && !c.ready);
    assert(epd_compute_open(&c, NULL, NULL, &s) == EPD_COMPUTE_UNAVAILABLE && !s);
    assert(epd_compute_wait_create(&w) == EPD_COMPUTE_UNAVAILABLE && !w);
    assert(epd_compute_open_wait(&c, NULL, NULL, (epd_compute_wait_t)&c, &s) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_set_purpose(s, EPD_COMPUTE_FOREGROUND) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_step(s, NULL) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_reset(s, NULL) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_close(s) == EPD_COMPUTE_UNAVAILABLE);
    assert(epd_compute_wait_destroy(w) == EPD_COMPUTE_UNAVAILABLE);
    epd_compute_cancel(s);
    epd_compute_wait_cancel(w);
    return 0;
}
