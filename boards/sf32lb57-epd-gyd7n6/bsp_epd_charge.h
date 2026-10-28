/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BSP_EPD_CHARGE_H
#define BSP_EPD_CHARGE_H

#include <stdbool.h>
#include <stdint.h>

/* True after successful board registration and the SDK device-binding stage. */
bool epd_charge_ready(void);

/* Changes on failures of supported GPIO status queries, including the query
 * inside the SDK calculator whose error is not propagated by that API. */
uint32_t epd_charge_error_sequence(void);

#endif
