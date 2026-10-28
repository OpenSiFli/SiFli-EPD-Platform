#include "wlan_log.h"
#include "aic_log.h"

/* A single aligned word is read/written atomically on the Cortex-M target. */
static volatile unsigned int severity_limit = DBG_SEV_IDX_WRN;

void wlan_log_set_verbose(bool verbose)
{
    severity_limit = verbose ? DBG_SEV_IDX_VRB : DBG_SEV_IDX_WRN;
}

int __wrap_platform_test_module_severity(unsigned int module, unsigned int severity)
{
    return module < DBG_MOD_IDX_MAX &&
        (CONFIG_LOG_MODULES & (1u << module)) &&
        severity >= DBG_SEV_IDX_CRT && severity <= CONFIG_LOG_LEVEL &&
        severity <= severity_limit;
}
