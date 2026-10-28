/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_TLS_CONFIG_H
#define EPD_TLS_CONFIG_H

/* Applied after the SDK configuration through MBEDTLS_USER_CONFIG_FILE. */
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME_DATE
/* Keep software AES lookup tables in Flash instead of the system SRAM. */
#define MBEDTLS_AES_ROM_TABLES
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
time_t epd_tls_time(time_t *out);
#ifdef __cplusplus
}
#endif
#define MBEDTLS_PLATFORM_TIME_MACRO epd_tls_time

#endif
