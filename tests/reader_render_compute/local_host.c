/* SPDX-License-Identifier: Apache-2.0 */
#define main unused_scheduler_main
#include "../compute_scheduler/host.c"
#undef main
#include "epd_reader_render.h"
#include "epd_resource_runtime.h"

static epd_compute_wait_t cancel_during_reserve;
static unsigned reservations;

size_t epd_app_allocation_peak(size_t bytes) { return bytes; }
int epd_resource_context_reserve(epd_memory_region_t region, size_t bytes)
{
    assert(region == EPD_MEMORY_REGION_SRAM && bytes);
    ++reservations;
    if (cancel_during_reserve) {
        assert(epd_compute_wait_destroy(cancel_during_reserve) == EPD_COMPUTE_BUSY);
        epd_compute_wait_cancel(cancel_during_reserve);
    }
    return EPD_RESOURCE_OK;
}
void mpu_dcache_clean(void *address, uint32_t bytes) { (void)address; (void)bytes; }

int main(void)
{
    InitializeCriticalSection(&irq);
    InitializeConditionVariable(&changed);
    __declspec(align(32)) uint8_t pixels[32];
    epd_compute_buffer_t frame = {pixels, NULL, sizeof(pixels)};
    rr_frame format = {8, 4, 8, RR_FORMAT_L8};
    epd_reader_render_t renderer = NULL;
    epd_compute_wait_t wait = NULL;

    assert(epd_compute_wait_create(&wait) == EPD_COMPUTE_OK);
    epd_compute_wait_cancel(wait);
    assert(epd_reader_render_open_wait(&frame, format, EPD_COMPUTE_FOREGROUND, wait, &renderer) ==
           EPD_COMPUTE_CANCELLED);
    assert(!renderer && reservations == 0);
    assert(epd_compute_wait_destroy(wait) == EPD_COMPUTE_OK);

    assert(epd_compute_wait_create(&wait) == EPD_COMPUTE_OK);
    cancel_during_reserve = wait;
    assert(epd_reader_render_open_wait(&frame, format, EPD_COMPUTE_FOREGROUND, wait, &renderer) ==
           EPD_COMPUTE_CANCELLED);
    assert(!renderer && reservations == 1);
    assert(epd_compute_wait_destroy(wait) == EPD_COMPUTE_OK);
    cancel_during_reserve = NULL;

    assert(epd_compute_wait_create(&wait) == EPD_COMPUTE_OK);
    assert(epd_reader_render_open_wait(&frame, format, EPD_COMPUTE_BACKGROUND, wait, &renderer) ==
           EPD_COMPUTE_OK);
    assert(renderer && reservations == 2);
    assert(epd_compute_wait_destroy(wait) == EPD_COMPUTE_OK);
    assert(epd_reader_render_shutdown(&renderer) == EPD_COMPUTE_OK && !renderer);
    assert(!allocations && !semaphores && !calls && !admission_head && !active);
    DeleteCriticalSection(&irq);
    puts("PASS: local renderer cancellation before and during admission, and admission without an ACPU session");
    return 0;
}
