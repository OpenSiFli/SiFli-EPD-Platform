"""Test the current reader pixel pool with host sanitizers."""
import re
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
src = (root / "modules/common/reader/reader_view.c").read_text(encoding="utf-8")
spare = re.search(r"typedef struct \{\n    epd_compute_buffer_t pixels;.*?static frame_spare_t \*frame_spare_extra;", src, re.S).group()
start = src.index("#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED\nstatic void frame_pixels_free")
helpers = src[start:src.index("static void *frame_cold_allocate", start)]
start = src.index("static void frame_release(view_frame_t *frame)\n{")
release = src[start:src.index("static void frame_cache_clear", start)]
start = src.index("static size_t view_resource_released(")
pressure = src[start:src.index("static void state_reflow", start)]
fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { void *data, *allocation; uint32_t capacity; } epd_compute_buffer_t;
typedef struct { unsigned session, references; } view_state_t;
typedef struct frame {
    epd_compute_buffer_t pixels;
    view_state_t *owner;
    void *renderer, *render_image, *pins;
    bool required;
    struct frame *backing, *next;
    unsigned intent, number, complete, references, window_charge, layout;
    struct { unsigned data_size; } image;
} view_frame_t;
static struct { bool thread, stop, suspended, replace; unsigned session; view_frame_t *cleanup; } mailbox;
static unsigned depth, frees, allocations, app_frees, before_release, completed;
static void (*during_pixel_free)(void);
static uintptr_t last_free;
static bool oom, change_during_install;
static bool view_resource_pending;
static uint32_t view_resource_requested, view_resource_backend;
static struct { unsigned provider; } view_resource;
static view_state_t *primary_state, *state;
static view_frame_t *render_frame;
static size_t frame_bytes, auxiliary;
#define EPD_APP_PSRAM 1
#define EPD_RESOURCE_OK 0
static void rt_enter_critical(void) { ++depth; }
static void rt_exit_critical(void) { assert(depth-- == 1); }
#define rt_kprintf printf
static void epd_compute_buffer_free(epd_compute_buffer_t *pixels)
{
    assert(!depth);
    if (pixels->allocation) {
        last_free = (uintptr_t)pixels->allocation;
        ++frees;
        if (during_pixel_free) during_pixel_free();
    }
    memset(pixels, 0, sizeof(*pixels));
}
typedef struct { size_t physical; } epd_memory_block_snapshot_t;
#define EPD_MEMORY_OK 0
static size_t snapshot_physical;
static int epd_memory_block_get(const void *allocation, epd_memory_block_snapshot_t *out)
{
    assert(!depth && allocation);
    if (!snapshot_physical) return -1;
    out->physical = snapshot_physical;
    return EPD_MEMORY_OK;
}
static void *view_allocate(size_t size, int region)
{
    assert(!depth && region == EPD_APP_PSRAM);
    ++allocations;
    if (change_during_install) ++mailbox.session;
    return oom ? NULL : malloc(size);
}
static void epd_app_free(void *p) { assert(!depth); if (p) { ++app_frees; free(p); } }
static void frame_drop_note(view_frame_t *f) { (void)f; }
static void frame_cold_clear(view_frame_t *f) { (void)f; }
static void image_release(void *p) { (void)p; }
static void epd_app_font_page_release(void *p) { (void)p; }
static void window_uncharge_bytes(size_t n) { (void)n; }
static size_t view_resource_bytes(void) { return frame_bytes + auxiliary; }
static bool frame_reclaim_layout(size_t bytes, unsigned retained)
{ (void)bytes; (void)retained; if (!auxiliary) return false; --auxiliary; return true; }
static bool frame_reclaim_pages(size_t bytes) { (void)bytes; return false; }
static unsigned window_contexts(view_state_t *current, view_state_t **contexts)
{ (void)current; (void)contexts; return 0; }
static void images_prune(bool force) { (void)force; }
static void view_resource_publish(void) {}
static void epd_resource_provider_cache_complete(unsigned id, unsigned backend, size_t bytes, int result)
{ (void)id; (void)backend; assert(!result && !view_resource_pending); completed = bytes; }
"""
cases = r"""
static view_frame_t frame(view_state_t *owner, unsigned cap, uintptr_t addr)
{
    view_frame_t f = { .owner = owner, .intent = 4, .number = 8, .complete = 1, .layout = 1 };
    f.image.data_size = cap;
    if (addr) {
        f.pixels = (epd_compute_buffer_t){ (void *)(addr + 32), (void *)addr, cap };
        frame_bytes += cap;
    }
    return f;
}

static view_frame_t *owned_frame(view_state_t *owner, unsigned cap, uintptr_t addr)
{
    view_frame_t *f = malloc(sizeof(*f));
    assert(f);
    *f = frame(owner, cap, addr);
    ++owner->references;
    f->references = 2;
    return f;
}

static void pressure_request(unsigned bytes, unsigned expected_bytes, unsigned expected_frees)
{
    before_release = frees;
    view_resource_requested = bytes;
    view_resource_pending = true;
    view_resource_step();
    assert(completed == expected_bytes && frees - before_release == expected_frees);
}

static view_frame_t *late_ui_frame;

static void release_ui_during_pixel_free(void)
{
    during_pixel_free = NULL;
    frame_release(late_ui_frame);
    late_ui_frame = NULL;
}

int main(void)
{
    view_state_t a = { .session = 3 }, b = { .session = 4 };
    mailbox.thread = true;
    mailbox.session = a.session;
    frame_spare_install(a.session);
    assert(frame_spare_extra && allocations == 1);
    view_frame_t one = frame(&a, 64, 0x60001000), two = frame(&a, 96, 0x60002000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    assert(frame_spare_reclaimable() == 160 && frame_bytes == 160 && !frees);
    view_frame_t wrong = frame(&b, 64, 0), large = frame(&a, 128, 0);
    assert(!frame_spare_take(&wrong) && !frame_spare_take(&large));
    frame_spare_trim(&large);
    assert(frame_spare_reclaimable() == 160);
    one.layout = 99; /* Raw pixel storage can be reused across layouts. */
    assert(frame_spare_available(&one) && frame_spare_take(&one) && frame_spare_take(&two));
    assert((uintptr_t)one.pixels.allocation == 0x60001000 &&
           (uintptr_t)two.pixels.allocation == 0x60002000);
    assert(frame_spare_reclaimable() == 0 && frame_bytes == 160 && allocations == 1);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));

    pressure_request(0, 0, 0);
    primary_state = &a; auxiliary = 40;
    pressure_request(20, 20, 0);
    pressure_request(80, 84, 1);
    assert(last_free == 0x60001000 && frame_spare_reclaimable() == 96);
    pressure_request(256, 96, 1);
    assert(!frame_bytes && !frame_spare_reclaimable());
    one = frame(&a, 64, 0x60001000); two = frame(&a, 96, 0x60002000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    pressure_request(120, 160, 2);
    assert(!frame_bytes);
    one = frame(&a, 64, 0x60001000); two = frame(&a, 96, 0x60002000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    frame_spare_trim(&wrong);
    assert(frame_spare_reclaimable() == 160);
    large.required = true;
    frame_spare_trim(&large);
    assert(last_free == 0x60002000 && frame_bytes == 64 && frame_spare_reclaimable() == 64);
    pressure_request(128, 64, 1);

    /* The UI can donate an already charged frame after the worker detaches
     * a slot and before it deducts the returned block's capacity. */
    one = frame(&a, 64, 0x60001000); two = frame(&a, 96, 0x60002000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    late_ui_frame = owned_frame(&a, 64, 0x60008000);
    frame_release(late_ui_frame);
    during_pixel_free = release_ui_during_pixel_free;
    pressure_request(64, 64, 1);
    assert(!late_ui_frame && frame_bytes == 160 && frame_spare_reclaimable() == 160);
    assert(frame_spare_take(&one) && (uintptr_t)one.pixels.allocation == 0x60008000);
    assert(frame_spare_keep(&one));
    pressure_request(256, 160, 2);
    assert(!frame_bytes && !frame_spare_reclaimable());

    /* Even if a last reference refills a slot, the larger request
     * drains only the finite set of already charged buffers. */
    one = frame(&a, 64, 0x60001000); two = frame(&a, 96, 0x60002000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    late_ui_frame = owned_frame(&a, 64, 0x60008000);
    frame_release(late_ui_frame);
    during_pixel_free = release_ui_during_pixel_free;
    pressure_request(512, 224, 3);
    assert(!late_ui_frame && !frame_bytes && !frame_spare_reclaimable());

    /* Physical heap span covers the entire request after returning one slot. */
    one = frame(&a, 690688, 0x60001000); two = frame(&a, 690688, 0x60101000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    snapshot_physical = 690760;
    pressure_request(690760, 690760, 1);
    assert(frame_bytes == 690688 && frame_spare_reclaimable() == 690688);
    pressure_request(690760, 690760, 1);
    assert(!frame_bytes && !frame_spare_reclaimable());

    /* Snapshot unavailable: count known capacity rather than guessing overhead. */
    one = frame(&a, 690688, 0x60001000); two = frame(&a, 690688, 0x60101000);
    assert(frame_spare_keep(&one) && frame_spare_keep(&two));
    snapshot_physical = 0;
    pressure_request(690760, 2 * 690688, 2);
    assert(!frame_bytes);
    puts("PASS physical reclaim and unavailable snapshot fallback");
    one = frame(&a, 64, 0);

    /* A net increase cannot turn into an unsigned release underflow. */
    auxiliary = 96;
    assert(view_resource_released(64) == 0);
    auxiliary = 0;

    view_frame_t *ui = owned_frame(&a, 64, 0x60003000);
    frame_release(ui);
    assert(!frame_spare_available(&one) && ui->references == 1);
    frame_release(ui);
    assert(frame_spare_available(&one));
    mailbox.suspended = true;
    assert(!frame_spare_take(&one));
    frame_spare_clear("session_suspend", false);
    assert(frame_spare_extra && !frame_bytes);
    mailbox.suspended = false;

    view_frame_t *working = owned_frame(&a, 96, 0x60004000);
    working->renderer = working;
    frame_release(working); frame_release(working);
    assert(mailbox.cleanup == working && !frame_spare_reclaimable());
    working->renderer = NULL; working->references = 1; mailbox.cleanup = NULL;
    mailbox.replace = true; mailbox.session = b.session;
    frame_spare_clear("session_replace", true);
    frame_release(working);
    assert(!frame_spare_extra && !frame_bytes);

    oom = true; mailbox.replace = false;
    frame_spare_install(b.session);
    assert(!frame_spare_extra);
    one = frame(&b, 64, 0x60005000); two = frame(&b, 64, 0x60006000);
    assert(frame_spare_keep(&one) && !frame_spare_keep(&two));
    assert(frame_spare_reclaimable() == 64 && frame_spare_take(&one));
    epd_compute_buffer_free(&one.pixels); epd_compute_buffer_free(&two.pixels); frame_bytes -= 128;
    assert(!frame_bytes);
    oom = false;
    change_during_install = true;
    frame_spare_install(b.session);
    assert(!frame_spare_extra);
    change_during_install = false;
    b.session = mailbox.session;
    frame_spare_install(b.session);
    one = frame(&b, 64, 0x60007000);
    assert(frame_spare_keep(&one));
    mailbox.stop = true;
    assert(!frame_spare_available(&one));
    frame_spare_clear("session_stop", true);
    assert(!frame_spare_extra && !frame_bytes && !depth && app_frees == 7);
    puts("PASS dual retention/reuse, pressure deficit, session/layout/size, async refs, stop/replace/suspend, OOM");
}
"""
def main():
    with tempfile.TemporaryDirectory(prefix="reader-pixel-pool-") as directory:
        build = Path(directory)
        unit = build / "host.c"
        unit.write_text("\n".join((fixture, spare, helpers, release, pressure, cases)), encoding="utf-8")
        for enabled in (0, 1):
            exe = build / f"host-input{enabled}"
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", f"-DEPD_INPUT_TEST_ENABLED={enabled}",
                            str(unit), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
            print(f"PASS INPUT_TEST={enabled}", flush=True)


if __name__ == "__main__":
    main()
