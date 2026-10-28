/**
 * @file battery.h
 * @brief 电池服务接口（纯 C）
 */
#ifndef __BATTERY_H__
#define __BATTERY_H__

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the board's battery service. EPD 57 initialization is idempotent.
 */
void battery_init(void);

/**
 * @brief Stop sampling. EPD 57 waits for the current conversion to finish.
 */
void battery_stop(void);

/* Nonblocking, idempotent sleep handoff. RT_EOK means the request is accepted.
 * suspend_result(): -RT_EBUSY only while initialization, a current sample or
 * ADC reference release is pending; RT_EOK once this service holds no ADC
 * reference and both the 1-second status and 10-second voltage polling stop.
 * Other errors are final failures. A failed ADC close retains the reference;
 * resume reuses it and the next suspend retries close. An inactive service
 * succeeds without I/O unless a previous close still failed; this error is
 * retained across failed initialization too. An active service without a
 * suspend request reports -RT_EINVAL.
 * The hold survives stop/init until resume. Call resume on cancellation too.
 * Suspend preserves cached values/validity, calculator history and the user's
 * low-power flag; running still means the service is started. The timestamp
 * remains the age indicator. Charging hardware and other ADC clients continue
 * independently, so this is not a barrier for all users of the shared ADC. */
rt_err_t battery_suspend(void);
rt_err_t battery_suspend_result(void);

/* Release the hold and wake the existing worker for immediate status AND
 * voltage refresh, then restore the 1-second/10-second periods. Repeated calls
 * are no-ops. Does not start an inactive service or wait for fresh data;
 * last_success_tick/validity reflect the next completed voltage attempt. */
void battery_resume(void);

/**
 * @brief Read voltage in mV. On the EPD 57 board, invalid voltage returns 0.
 */
float battery_get_voltage(void);

/**
 * @brief Read percentage (0-100); the EPD 57 board returns -1 when unknown.
 */
int battery_get_percentage(void);

/**
 * @brief 是否正在充电
 */
bool battery_is_charging(void);

/**
 * @brief 获取低电量标志
 */
uint8_t battery_get_low_power_state(void);

/**
 * @brief 设置低电量标志
 */
void battery_set_low_power_state(uint8_t state);

typedef struct
{
    uint32_t voltage_mv;
    int percentage; /* Unmodified SDK calculator result; valid only with its flag. */
    rt_tick_t last_success_tick;
    bool running;
    bool voltage_valid;
    bool percentage_valid;
    bool charge_valid;
    bool usb_present;
    bool charging;
} battery_snapshot_t;

/* EPD 57: copy one complete background snapshot without hardware access.
 * Charge status is polled every second; voltage is sampled at startup and
 * every ten seconds. Status-only updates do not advance percentage filters.
 * Failed samples retain historical values, but clear the matching validity.
 * last_success_tick timestamps the last successful voltage conversion.
 * Charger/calculator-disabled builds use the same worker and sleep handshake,
 * with the corresponding charge/percentage validity flags left false. */
void battery_get_snapshot(battery_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __BATTERY_H__ */
