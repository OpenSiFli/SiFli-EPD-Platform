/* SPDX-License-Identifier: Apache-2.0 */
#include "image_internal.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/stdlib/lv_mem.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/core/lv_global.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/draw/lv_draw_buf_private.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/draw/lv_image_decoder_private.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/misc/lv_fs_private.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/bin_decoder/lv_bin_decoder.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/thorvg_capi.h"
/* ACPU-local data support only: no display, objects, timers or draw worker. */
lv_global_t lv_global;
void ri_svg_backend_reset(void) { memset(&lv_global, 0, sizeof(lv_global)); }
int ri_svg_backend_init(void)
{
    memset(&lv_global, 0, sizeof(lv_global));
    lv_global.memory_zero = ZERO_MEM_SENTINEL;
    lv_draw_buf_init_handlers();
    lv_fs_init();
    lv_image_decoder_init(0, 0);
    lv_bin_decoder_init();
    return tvg_engine_init(TVG_ENGINE_SW, 0) == TVG_RESULT_SUCCESS ? RI_OK : RI_NOMEM;
}
void ri_svg_backend_deinit(void)
{
    lv_image_decoder_deinit();
    tvg_engine_term(TVG_ENGINE_SW);
    lv_fs_deinit();
}
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *p, size_t n) { (void)p; (void)n; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t p) { (void)p; }
void *lv_malloc_core(size_t n) { return ri_work_alloc(n); }
void *lv_realloc_core(void *p, size_t n) { return ri_work_realloc(p, n); }
void lv_free_core(void *p) { ri_work_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *p) { memset(p, 0, sizeof(*p)); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
