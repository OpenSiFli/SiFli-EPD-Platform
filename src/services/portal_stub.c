#include "portal/portal.h"
#if defined(EPD_USE_LVGL) && !defined(RT_USING_WIFI)
#include <string.h>

rt_err_t portal_start(void) { return -RT_ENOSYS; }
void portal_process(void) {}
void portal_stop(void) {}
bool portal_prepare_sleep(void) { return true; }
void portal_resume(void) {}
void portal_snapshot(portal_snapshot_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out));
    out->phase=PORTAL_ERROR;
    out->error=-RT_ENOSYS;
}
bool portal_release_storage(void) { return true; }
#endif
