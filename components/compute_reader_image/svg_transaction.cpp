/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_image.h"
#include <cstring>
#include <new>
#include "svg_exception.h"
extern "C" {
bool ri_scope_enter(const ri_io *);
void ri_scope_leave(void);
void ri_domain_begin(void (*)(void));
void ri_domain_finish(void *keep);
int ri_svg_backend_init(void);
void ri_svg_backend_deinit(void);
void ri_svg_backend_reset(void);
void ri_svg_adapter_reset(void);
int ri_svg_render_active(const ri_io *, const char *, unsigned, unsigned, ri_bitmap *);
void ri_reset_tvgInitializer(void);
void ri_reset_tvgLoader(void);
void ri_reset_tvgSwRenderer(void);
void ri_reset_tvgTaskScheduler(void);
}
static bool initialized;
static void failed() { throw ri_svg_oom(); }
static void reset_backend()
{
    ri_reset_tvgTaskScheduler(); ri_reset_tvgSwRenderer();
    ri_reset_tvgLoader(); ri_reset_tvgInitializer();
    ri_svg_backend_reset(); ri_svg_adapter_reset();
}
extern "C" int ri_svg_init(const ri_io *io)
{
    if (!io || !io->alloc || !io->free) return RI_INVALID;
    if (initialized) return RI_BUSY;
    initialized = true;
    return RI_OK;
}
extern "C" void ri_svg_shutdown(void) { initialized = false; }
extern "C" int ri_svg_decode(const ri_io *io, const char *resource,
                              unsigned w, unsigned h, ri_bitmap *out)
{
    if (!out) return RI_INVALID;
    std::memset(out, 0, sizeof(*out));
    if (!initialized || !io || !io->read || !io->alloc || !io->free ||
        !resource || !w || !h || w > 65535u || h > 65535u ||
        (!!io->output_alloc != !!io->output_free)) return RI_INVALID;
    if (!ri_scope_enter(io)) return RI_BUSY;
    void *exception_storage = ri_work_alloc(RI_EXCEPTION_PREFIX + RI_EXCEPTION_PAYLOAD);
    if (!exception_storage) { ri_scope_leave(); return RI_NOMEM; }
    ri_exception_set_storage(exception_storage);
    int status;
    ri_domain_begin(failed);
    try {
        status = ri_svg_backend_init();
        if (!status) status = ri_svg_render_active(io, resource, w, h, out);
        ri_svg_backend_deinit();
    } catch (const ri_svg_oom &) {
        status = RI_NOMEM;
    } catch (const std::bad_alloc &) {
        status = RI_NOMEM;
    } catch (...) {
        status = RI_INVALID;
    }
    /* Size fitting happens before raster allocation. Allocation failure ends
     * this decode; retrying cannot reclaim another live session's workspace.
     * C++ objects have unwound before reclaiming unpublished C resources. */
    reset_backend();
    if (status) std::memset(out, 0, sizeof(*out));
    ri_domain_finish(out->pixels);
    ri_exception_set_storage(nullptr);
    ri_work_free(exception_storage);
    ri_scope_leave();
    return status;
}
