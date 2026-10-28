/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_SYSTEM_INFO_H
#define EPD_SYSTEM_INFO_H

#include <stdbool.h>
#include <stdint.h>
#include <lwip/ip_addr.h>

#define SYSTEM_INFO_ADDRESS_CAPACITY \
    (LWIP_IPV4 + (LWIP_IPV6 ? LWIP_IPV6_NUM_ADDRESSES : 0))

typedef struct
{
    char product[64];
    char model[64];
    char sdk[32];
    char ui_version[64]; /* Bloom UI build description or abbreviated app profile ID. */
    char build_date[11]; /* YYYY-MM-DD */
    unsigned address_count;
    char addresses[SYSTEM_INFO_ADDRESS_CAPACITY][IPADDR_STRLEN_MAX];
    char mac[18];
    char bt[18];
    uint64_t uptime_minutes;
} system_info_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous snapshot for ordinary threads. Missing fields are empty; IPs are
 * IPv4 first, then all valid IPv6 slots of the up, linked default interface.
 * False means invalid output or a failed network snapshot; other fields remain
 * available. An uninitialized stack or disconnected interface is not an error. */
bool system_info_read(system_info_t *out);

/* Call from the main UI cycle, starting before the first tick wrap and even when
 * this page is hidden. Every interval (including sleep) must be shorter than one
 * rt_tick_t wrap: about 49.7 days at 1000 Hz with 32-bit ticks. SDK-compensated
 * sleep ticks are included. system_info_read() also performs this maintenance. */
void system_info_process(void);

#ifdef __cplusplus
}
#endif
#endif
