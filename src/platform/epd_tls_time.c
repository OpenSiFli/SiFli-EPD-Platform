/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_tls_config.h"
#include "services/system_time.h"
#include <rtm.h>

time_t epd_tls_time(time_t *out)
{
    int64_t utc = system_time_get_utc();
    time_t now = system_time_utc_valid(utc) ? (time_t)utc : 0;
    if (out) *out = now;
    return now;
}
RTM_EXPORT(epd_tls_time);
