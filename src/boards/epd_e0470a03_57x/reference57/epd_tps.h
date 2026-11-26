/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __EPD_TPS_H__
#define __EPD_TPS_H__
#include <stdbool.h>
#include <rtthread.h>
#ifdef __cplusplus
extern "C" {
#endif
//模组的VCOM电压(2100代表-2.10V)
rt_err_t tps_init(uint16_t vcom_voltage);
/* Serialized by the panel driver. Complete every started power-up or shut down. */
rt_err_t tps_start_powerup(void);
rt_err_t tps_wait_powerup(void);
bool tps_rails_good(void);
rt_tick_t tps_disable_vcom(void);
void tps_finish_powerdown(rt_tick_t vcom_disabled_at);
void tps_enter_sleep(void);
#ifdef __cplusplus
}
#endif
#endif /*__EPD_TPS_H__*/
