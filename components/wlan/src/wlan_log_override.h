#ifndef EPD_WLAN_LOG_OVERRIDE_H
#define EPD_WLAN_LOG_OVERRIDE_H

#include "aic_log.h"

int __wrap_platform_test_module_severity(unsigned int module, unsigned int severity);

/* Keep SDK compile-time gates and formatting, including plat_port.c's local
 * calls which the linker's --wrap cannot redirect. */
#undef __DBG_MOD_SEV_TEMPLATE
#define __DBG_MOD_SEV_TEMPLATE(mod, sev, fmt, ...)                        \
do {                                                                    \
    if (__wrap_platform_test_module_severity(DBG_MOD_IDX_##mod,           \
                                             DBG_SEV_IDX_##sev)) {       \
        OSAL_DBG_PRINTF("[AIC]["#mod"]"fmt, ##__VA_ARGS__);                \
    }                                                                   \
} while (0)

#endif
