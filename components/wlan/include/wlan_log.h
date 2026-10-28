#ifndef EPD_WLAN_LOG_H
#define EPD_WLAN_LOG_H

#include <stdbool.h>

/* AIC SDK logs start at WARN; verbose enables compiled-in INFO/VRB logs. */
void wlan_log_set_verbose(bool verbose);

#endif
