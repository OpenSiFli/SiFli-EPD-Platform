/* SPDX-License-Identifier: Apache-2.0 */
#include "battery.h"


#include <rtdevice.h>
#include <rthw.h>
#include "mem_section.h"
#ifdef BSP_USING_CHARGER
#include "charge.h"
#include "bsp_epd_charge.h"
#endif
#if defined(BSP_USING_CHARGER) && defined(USING_BATTERY_CALCULATOR)
#include "battery_calculator.h"
static battery_calculator_t calculator;
static battery_calculator_config_t calculator_config;
static bool calculator_ready;
#endif

#define BATTERY_ADC_CHANNEL 11
#define BATTERY_STATUS_PERIOD_MS 1000
#define BATTERY_VOLTAGE_PERIOD_MS 10000
/* The SDK holds the shared ADC semaphore across sampling delays. Finish
 * ahead of drawing/GUI work so key identification does not wait for them;
 * Bluetooth and continuous audio still take precedence. */
#define BATTERY_THREAD_PRIORITY (RT_THREAD_PRIORITY_HIGH + 2)

static battery_snapshot_t published = {.percentage = -1};
static struct rt_mutex lifecycle_lock;
static bool lock_ready;
static struct rt_semaphore wakeup;
static struct rt_thread worker;
L2_RET_BSS_SECT_BEGIN(battery_stack)
ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t worker_stack[2048] L2_RET_BSS_SECT(battery_stack);
L2_RET_BSS_SECT_END
static rt_device_t adc;
static bool active, stopping, initializing;
static bool sleep_hold, refresh_requested;
static rt_err_t sleep_result = RT_EOK;
static rt_err_t adc_release_result = RT_EOK;
static uint8_t low_power;

static int battery_lock_init(void)
{
    rt_err_t result = rt_mutex_init(&lifecycle_lock, "batlife", RT_IPC_FLAG_PRIO);
    lock_ready = result == RT_EOK;
    return result;
}
INIT_PREV_EXPORT(battery_lock_init);

void battery_get_snapshot(battery_snapshot_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = published;
    rt_hw_interrupt_enable(level);
}

static void publish(const battery_snapshot_t *next)
{
    rt_base_t level = rt_hw_interrupt_disable();
    published = *next;
    rt_hw_interrupt_enable(level);
}

rt_err_t battery_suspend(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!sleep_hold)
    {
        sleep_hold = true;
        sleep_result = active || initializing ? -RT_EBUSY : adc_release_result;
        /* Keep the lifetime check and wake atomic with stop/start cleanup. */
        if (active && !stopping) rt_sem_release(&wakeup);
    }
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

rt_err_t battery_suspend_result(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_err_t result = initializing ? -RT_EBUSY :
        (sleep_hold ? sleep_result : (active ? -RT_EINVAL : adc_release_result));
    rt_hw_interrupt_enable(level);
    return result;
}

void battery_resume(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (sleep_hold)
    {
        sleep_hold = false;
        refresh_requested = true;
        if (active && !stopping) rt_sem_release(&wakeup);
    }
    rt_hw_interrupt_enable(level);
}

static void acquire_adc(void)
{
    adc = rt_device_find("bat1");
    if (adc && rt_device_open(adc, RT_DEVICE_OFLAG_RDONLY) != RT_EOK) adc = RT_NULL;
    /* The SDK converter configures/enables its slot under gpadc_lock and stops
     * conversion before releasing it. enable/disable bypass that lock and
     * overwrite bat1->channel, which belongs to ADC button wake restoration.
     * Own only a device reference; do not alter the shared slot or PM channel. */
}

static rt_err_t release_adc(void)
{
    /* Only the worker closes its reference, after its last read has returned.
     * The SDK ADC has no close callback and leaves other clients untouched. */
    rt_err_t result = adc ? rt_device_close(adc) : RT_EOK;
    rt_base_t level = rt_hw_interrupt_disable();
    /* A rejected close keeps ownership: resume reuses this reference and the
     * next suspend retries closing it instead of opening an additional one. */
    if (result == RT_EOK) adc = RT_NULL;
    adc_release_result = result == -RT_EBUSY ? -RT_ERROR : result;
    rt_hw_interrupt_enable(level);
    return adc_release_result;
}

static void sample(bool sample_voltage)
{
    battery_snapshot_t next;
    battery_get_snapshot(&next);
    next.charge_valid = false;
#ifdef BSP_USING_CHARGER
    if (epd_charge_ready())
    {
        uint8_t detected = 0, charging = 0;
        rt_charge_err_t detect_result = rt_charge_get_detect_status(&detected);
        rt_charge_err_t charge_result = rt_charge_get_status(&charging);
        if (detect_result == RT_CHARGE_EOK && charge_result == RT_CHARGE_EOK)
        {
            next.usb_present = detected != 0;
            next.charging = next.usb_present && charging != 0;
            next.charge_valid = true;
        }
    }
#endif
    if (!next.charge_valid) next.percentage_valid = false;
    if (!sample_voltage)
    {
        /* Status polling preserves the last measurement; only fresh voltage
         * samples advance the SDK's voltage and percentage filters. */
        publish(&next);
        return;
    }
    next.voltage_valid = false;
    next.percentage_valid = false;
    rt_uint32_t voltage = 0;
    if (adc && rt_device_read(adc, BATTERY_ADC_CHANNEL, &voltage, sizeof(voltage)) ==
               sizeof(voltage))
    {
        next.voltage_mv = voltage / 10u;
        next.voltage_valid = true;
        next.last_success_tick = rt_tick_get();
    }
#if defined(BSP_USING_CHARGER) && defined(USING_BATTERY_CALCULATOR)
    if (calculator_ready && next.voltage_valid && next.charge_valid)
    {
        /* The unmodified SDK reference tables use 0.1 mV (41808 = 4180.8 mV).
         * Pass the ADC value in that same unit; only the snapshot uses mV. */
        uint32_t errors = epd_charge_error_sequence();
        battery_calculator_t candidate = calculator;
        int percentage = battery_calculator_get_percent(&candidate, voltage);
        if (epd_charge_error_sequence() == errors)
        {
            calculator = candidate;
            next.percentage = percentage;
            next.percentage_valid = true;
        }
        else
        {
            next.charge_valid = false;
        }
    }
#endif
    publish(&next);
}

static void battery_worker(void *parameter)
{
    (void)parameter;
    const rt_tick_t period = rt_tick_from_millisecond(BATTERY_STATUS_PERIOD_MS);
    const rt_tick_t voltage_period = rt_tick_from_millisecond(BATTERY_VOLTAGE_PERIOD_MS);
    rt_tick_t last_voltage_attempt = 0;
    rt_tick_t last_status_attempt = 0, status_interval = period;
    bool first_sample = true;
    bool parked = false;
    rt_err_t release_result = RT_EOK;
    for (;;)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        bool stop = stopping;
        bool held = sleep_hold;
        bool refresh = refresh_requested;
        refresh_requested = false;
        rt_hw_interrupt_enable(level);
        if (stop) break;
        if (held)
        {
            /* Resume followed immediately by suspend is a new close attempt,
             * even if the worker never observed the intervening awake state. */
            if (!parked || refresh) release_result = release_adc();
            parked = true;
            first_sample = true;
            level = rt_hw_interrupt_disable();
            if (sleep_hold) sleep_result = release_result;
            bool wait = sleep_hold && !stopping;
            rt_hw_interrupt_enable(level);
            if (wait) rt_sem_take(&wakeup, RT_WAITING_FOREVER);
            continue;
        }
        parked = false;
        if (refresh) first_sample = true;
        rt_tick_t started = rt_tick_get();
        rt_tick_t elapsed = started - last_status_attempt;
        if (!first_sample && elapsed < status_interval)
        {
            /* A stale wake token must not create an extra status/ADC sample. */
            rt_sem_take(&wakeup, (rt_int32_t)(status_interval - elapsed));
            continue;
        }
        if (first_sample && !adc) acquire_adc();
        last_status_attempt = started;
        bool sample_voltage = first_sample ||
            (rt_tick_t)(started - last_voltage_attempt) >= voltage_period;
        if (sample_voltage)
        {
            last_voltage_attempt = started;
            first_sample = false;
        }
        sample(sample_voltage);
        elapsed = rt_tick_get() - started;
        /* Poll status once per second; do not catch up missed ADC samples. */
        status_interval = elapsed < period ? period : elapsed + period;
    }
    if (!parked || adc) release_result = release_adc();
    rt_base_t level = rt_hw_interrupt_disable();
    if (sleep_hold) sleep_result = release_result;
    rt_hw_interrupt_enable(level);
}

void battery_init(void)
{
    if (!lock_ready || rt_mutex_take(&lifecycle_lock, RT_WAITING_FOREVER) != RT_EOK)
        return;
    if (active) goto done;
    rt_base_t level = rt_hw_interrupt_disable();
    initializing = true;
    stopping = false;
    if (sleep_hold) sleep_result = -RT_EBUSY;
    rt_hw_interrupt_enable(level);
    battery_snapshot_t initial = {0};
    initial.percentage = -1;
    publish(&initial);
    battery_set_low_power_state(0);
#if defined(BSP_USING_CHARGER) && defined(USING_BATTERY_CALCULATOR)
    calculator_config = (battery_calculator_config_t) {
        .charging_table = charging_curve_table,
        .charging_table_size = charging_curve_table_size,
        .discharging_table = discharge_curve_table,
        .discharging_table_size = discharge_curve_table_size,
        .charge_filter_threshold = 50, /* 5 mV in the SDK table's 0.1 mV unit. */
        .discharge_filter_threshold = 50,
        .filter_count = 3, /* Confirm large changes over three 10-second samples. */
        .secondary_filter_enabled = true,
        /* 0.9^10 ~= 0.35: retain the one-second filter's decay time. */
        .secondary_filter_weight_pre = 35,
        .secondary_filter_weight_cur = 65,
    };
    calculator_ready = charging_curve_table_size && discharge_curve_table_size &&
        battery_calculator_init(&calculator, &calculator_config) == BATTERY_CALC_SUCCESS;
#endif
    if (rt_sem_init(&wakeup, "batwake", 0, RT_IPC_FLAG_FIFO) != RT_EOK) goto done;
    if (rt_thread_init(&worker, "battery", battery_worker, RT_NULL,
                       worker_stack, sizeof(worker_stack), BATTERY_THREAD_PRIORITY, 10) != RT_EOK)
        goto failed;
    initial.running = true;
    publish(&initial);
    level = rt_hw_interrupt_disable();
    active = true;
    rt_hw_interrupt_enable(level);
    if (rt_thread_startup(&worker) != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        active = false;
        rt_hw_interrupt_enable(level);
        rt_thread_detach(&worker);
        initial.running = false;
        publish(&initial);
        goto failed;
    }
    goto done;
failed:
    rt_sem_detach(&wakeup);
done:
    {
        rt_base_t saved = rt_hw_interrupt_disable();
        initializing = false;
        if (!active && sleep_hold) sleep_result = adc_release_result;
        rt_hw_interrupt_enable(saved);
    }
    rt_mutex_release(&lifecycle_lock);
}

void battery_stop(void)
{
    if (!lock_ready || rt_mutex_take(&lifecycle_lock, RT_WAITING_FOREVER) != RT_EOK)
        return;
    if (active)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        stopping = true;
        rt_hw_interrupt_enable(level);
        rt_sem_release(&wakeup);
        /* This static thread has no cleanup callback. RT_THREAD_CLOSE becomes
         * observable only after RT-Thread detaches it in rt_thread_exit(). */
        for (;;)
        {
            level = rt_hw_interrupt_disable();
            bool exited = (worker.stat & RT_THREAD_STAT_MASK) == RT_THREAD_CLOSE;
            rt_hw_interrupt_enable(level);
            if (exited) break;
            rt_thread_mdelay(1);
        }
        level = rt_hw_interrupt_disable();
        active = false;
        if (sleep_hold) sleep_result = adc_release_result;
        rt_hw_interrupt_enable(level);
        rt_sem_detach(&wakeup);
        battery_snapshot_t stopped;
        battery_get_snapshot(&stopped);
        stopped.running = false;
        stopped.voltage_valid = false;
        stopped.charge_valid = false;
        stopped.percentage_valid = false;
        publish(&stopped);
    }
    rt_mutex_release(&lifecycle_lock);
}

float battery_get_voltage(void)
{
    battery_snapshot_t value;
    battery_get_snapshot(&value);
    return value.voltage_valid ? (float)value.voltage_mv : 0.0f;
}

int battery_get_percentage(void)
{
    battery_snapshot_t value;
    battery_get_snapshot(&value);
    return value.percentage_valid ? value.percentage : -1;
}

bool battery_is_charging(void)
{
    battery_snapshot_t value;
    battery_get_snapshot(&value);
    return value.charge_valid && value.charging;
}

uint8_t battery_get_low_power_state(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint8_t value = low_power;
    rt_hw_interrupt_enable(level);
    return value;
}

void battery_set_low_power_state(uint8_t value)
{
    rt_base_t level = rt_hw_interrupt_disable();
    low_power = value;
    rt_hw_interrupt_enable(level);
}

