#include "app_service_internal.h"
#include "ui_settings_internal.h"
#include "system_reset.h"
#include "system_reset_store.h"
#include "storage.h"
#include "app_service.h"
#include "usb_card_reader.h"
#include "network_reset.h"
#include "wlan_service.h"
#include "ui/ui_settings.h"
#include "ui/platform/app_catalog.h"
#include "ui/platform/app_installer.h"
#include "boards/board_service.h"
#include "sleep_guard.h"
#include "speech/speech_settings.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <string.h>

static system_reset_status_t state;
static system_reset_target_t target;
static rt_thread_t worker;
static bool initialized, owns_storage, resume_record, cancel_card, saved;
static bool network_finished;
static bool initial_record_pending;
static rt_tick_t prepare_started;
static sleep_guard_owner_t reset_hold = {.id = "system-reset"};

static void release_if_finished(void)
{
    if (!worker && !network_finished && state.phase != SYSTEM_RESET_PREPARING &&
        state.phase != SYSTEM_RESET_RUNNING) sleep_guard_release(&reset_hold);
}

/* Failure ends the wait, never the service that still owns its resources. */
#define RESET_PREPARE_TIMEOUT_MS 30000

void system_reset_get_status(system_reset_status_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = state;
    rt_hw_interrupt_enable(level);
}

static void publish(system_reset_phase_t phase, rt_err_t error, const reset_record_t *record)
{
    rt_base_t level = rt_hw_interrupt_disable();
    state.phase = phase;
    state.error = error;
    if (record)
    {
        state.internal_done = record->step >= RESET_ERASE_CARD;
        state.card_pending = record->card_state == RESET_CARD_PENDING;
        state.card_cancelled = record->card_state == RESET_CARD_CANCELLED;
    }
    ++state.revision;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

bool system_reset_busy(void)
{
    system_reset_status_t status;
    system_reset_get_status(&status);
    return status.phase == SYSTEM_RESET_PREPARING || status.phase == SYSTEM_RESET_RUNNING ||
           status.factory_pending;
}

bool system_reset_blocks_apps(void)
{
    system_reset_status_t status;
    system_reset_get_status(&status);
    return status.phase != SYSTEM_RESET_IDLE;
}

bool system_reset_write_allowed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool allowed = !owns_storage || (worker && rt_thread_self() == worker);
    rt_hw_interrupt_enable(level);
    return allowed || wlan_service_reset_write_allowed();
}

void system_reset_init(void)
{
    if (initialized) return;
    initialized = true;
    reset_record_t record;
    bool exists;
    rt_err_t error = reset_record_read(&record, &exists);
    if (error == RT_EOK && !exists) return;
    if (!sleep_guard_acquire(&reset_hold)) { initialized = false; return; }
    state.kind = SYSTEM_RESET_FACTORY;
    state.factory_pending = true;
    owns_storage = true;
    resume_record = true;
    (void)network_reset_hold();
    prepare_started = rt_tick_get();
    publish(error == RT_EOK ? SYSTEM_RESET_PREPARING : SYSTEM_RESET_FAILED,
            error, error == RT_EOK ? &record : NULL);
    release_if_finished();
}

static bool conflict(void)
{
    return board_power_pending() || app_installer_busy() || usb_card_reader_active() || storage_changing();
}

rt_err_t system_reset_prepare(system_reset_kind_t kind, system_reset_target_t *out)
{
    if (!initialized || !out || (unsigned)kind > SYSTEM_RESET_CARD) return -RT_EINVAL;
    if (system_reset_blocks_apps() || conflict()) return -RT_EBUSY;
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == SYSTEM_RESET_NETWORK) return RT_EOK;
    sdcard_snapshot_t card;
    sdcard_get_snapshot(&card);
    if (kind == SYSTEM_RESET_FACTORY && !card.present) return RT_EOK;
    storage_lock();
    rt_err_t error = sdcard_get_identity(&out->card);
    if (error == RT_EOK)
    {
        sdcard_get_snapshot(&card);
        out->include_card = true;
        out->session = card.session;
        out->card_bytes = (uint64_t)out->card.sector_count * out->card.sector_size;
    }
    storage_unlock();
    return error;
}

rt_err_t system_reset_start(const system_reset_target_t *request)
{
    if (!initialized || !request || (unsigned)request->kind > SYSTEM_RESET_CARD ||
        (request->kind == SYSTEM_RESET_CARD && !request->include_card)) return -RT_EINVAL;
    if (system_reset_blocks_apps() || conflict()) return -RT_EBUSY;
    if (!sleep_guard_acquire(&reset_hold)) return -RT_EBUSY;
    if (request->kind != SYSTEM_RESET_CARD)
    {
        rt_err_t error = network_reset_hold();
        if (error != RT_EOK) { sleep_guard_release(&reset_hold); return error; }
    }
    target = *request;
    state.kind = target.kind;
    state.factory_pending = false;
    state.internal_done = state.card_cancelled = state.card_pending = false;
    resume_record = cancel_card = network_finished = saved = false;
    initial_record_pending = target.kind == SYSTEM_RESET_FACTORY;
    prepare_started = rt_tick_get();
    publish(SYSTEM_RESET_PREPARING, RT_EOK, NULL);
    return RT_EOK;
}

static rt_err_t advance(reset_record_t *record, reset_step_t step)
{
    reset_record_t next = *record;
    next.step = step;
    rt_err_t error = reset_record_write(&next);
    if (error == RT_EOK) *record = next;
    return error;
}

static rt_err_t network_reset_execute(void)
{
    rt_err_t error = network_reset_start();
    if (error != RT_EOK) return error;
    for (;;)
    {
        network_reset_status_t status;
        network_reset_get_status(&status);
        if (status.state == NETWORK_RESET_SUCCEEDED) return RT_EOK;
        if (status.state == NETWORK_RESET_FAILED)
        {
            rt_kprintf("[reset] wireless failed: stage=%u classic=%u ble=%u\n",
                       (unsigned)status.error, status.classic_store_status, status.ble_store_status);
            return status.error == NETWORK_RESET_ERROR_TIMEOUT ? -RT_ETIMEOUT : -RT_ERROR;
        }
        rt_thread_mdelay(100);
    }
}

static rt_err_t matching_card(const reset_record_t *record, uint32_t *session)
{
    sdcard_identity_t actual;
    storage_lock();
    rt_err_t error = sdcard_get_identity(&actual);
    if (error == RT_EOK && !sdcard_identity_equal(&record->card, &actual)) error = -RT_EBUSY;
    *session = storage_card_session();
    if (error == RT_EOK && !storage_session_valid(STORAGE_SD, *session)) error = -RT_EBUSY;
    storage_unlock();
    return error;
}

static void factory_worker(void)
{
    reset_record_t record;
    memset(&record, 0, sizeof(record));
    bool exists = false;
    rt_err_t error;
    if (resume_record)
    {
        error = reset_record_read(&record, &exists);
        if (error != RT_EOK || (!exists && !initial_record_pending))
        { publish(SYSTEM_RESET_FAILED, error != RT_EOK ? error : -RT_EIO, NULL); return; }
    }
    if (!exists)
    {
        /* Only the still-confirmed first request may retry an absent journal.
         * Boot recovery never reconstructs an erase request from missing data. */
        record.step = RESET_ERASE_INTERNAL;
        record.card_state = target.include_card ? RESET_CARD_PENDING : RESET_CARD_NONE;
        record.card = target.card;
        /* A failed verification can still have committed a valid intent. Keep
         * recovery blocked until the stored result has been read successfully. */
        state.factory_pending = true;
        resume_record = true;
        error = reset_record_write(&record);
        if (error != RT_EOK) { publish(SYSTEM_RESET_FAILED, error, NULL); return; }
    }
    initial_record_pending = false;
    state.factory_pending = true;
    publish(SYSTEM_RESET_RUNNING, RT_EOK, &record);
    if (record.step == RESET_ERASE_INTERNAL)
    {
        error = speech_settings_reset_storage() == EPD_SPEECH_OK ? RT_EOK : -RT_EIO;
        if (error == RT_EOK) error = reset_erase_internal();
        if (error == RT_EOK) error = advance(&record, RESET_WIRELESS);
        if (error != RT_EOK) goto failed;
    }
    if (record.step == RESET_WIRELESS)
    {
        error = network_reset_execute();
        if (error == RT_EOK) error = advance(&record, RESET_DEFAULTS);
        if (error != RT_EOK) goto failed;
    }
    if (record.step == RESET_DEFAULTS)
    {
        error = reset_default_directories();
        if (error == RT_EOK) error = advance(&record, RESET_ERASE_CARD);
        if (error != RT_EOK) goto failed;
    }
    if (record.step == RESET_ERASE_CARD)
    {
        if (record.card_state == RESET_CARD_PENDING)
        {
            if (cancel_card) record.card_state = RESET_CARD_CANCELLED;
            else
            {
                uint32_t session;
                error = matching_card(&record, &session);
                if (error != RT_EOK)
                { publish(SYSTEM_RESET_WAIT_CARD, error, &record); return; }
                error = reset_erase_card(&record.card, session);
                if (error != RT_EOK)
                {
                    uint32_t checked;
                    if (matching_card(&record, &checked) != RT_EOK)
                    { publish(SYSTEM_RESET_WAIT_CARD, error, &record); return; }
                    goto failed;
                }
                record.card_state = RESET_CARD_CLEARED;
            }
        }
        error = advance(&record, RESET_TERMINAL);
        if (error != RT_EOK) goto failed;
    }
    /* A terminal record reaches only this cleanup, including after a reboot. */
    error = reset_record_cleanup();
    if (error != RT_EOK) goto failed;
    state.factory_pending = false;
    publish(SYSTEM_RESET_DONE, RT_EOK, &record);
    return;
failed:
    publish(SYSTEM_RESET_FAILED, error, &record);
}

static void run(void *unused)
{
    (void)unused;
    if (state.kind == SYSTEM_RESET_FACTORY) factory_worker();
    else
    {
        rt_err_t error = state.kind == SYSTEM_RESET_CARD ?
                         sdcard_format(&target.card, target.session) : network_reset_execute();
        if (state.kind == SYSTEM_RESET_NETWORK && error == RT_EOK) network_finished = true;
        else publish(error == RT_EOK ? SYSTEM_RESET_DONE : SYSTEM_RESET_FAILED, error, NULL);
    }
    rt_base_t level = rt_hw_interrupt_disable();
    worker = NULL;
    release_if_finished();
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

void system_reset_process(void)
{
    if (!initialized || worker) return;
    if (network_finished)
    {
        network_finished = false;
        bool ok = ui_settings_reset_network();
        if (ok) ok = network_reset_resume() == RT_EOK;
        publish(ok ? SYSTEM_RESET_DONE : SYSTEM_RESET_FAILED, ok ? RT_EOK : -RT_EIO, NULL);
        release_if_finished();
        return;
    }
    if (state.phase != SYSTEM_RESET_PREPARING) return;
    if ((rt_tick_t)(rt_tick_get() - prepare_started) >= rt_tick_from_millisecond(RESET_PREPARE_TIMEOUT_MS))
    {
        publish(SYSTEM_RESET_FAILED, -RT_ETIMEOUT, NULL);
        release_if_finished();
        return;
    }
    if (conflict()) return;
    /* Do not invalidate sessions while an accepted asynchronous save is pending. */
    if (!saved)
    {
        if (!app_service_flush()) return;
        saved = true;
    }
    if (!app_catalog_release_storage()) return;
    rt_thread_t next = rt_thread_create("reset", run, NULL, 8192, 23, 10);
    if (!next)
    { publish(SYSTEM_RESET_FAILED, -RT_ENOMEM, NULL); release_if_finished(); return; }
    storage_lock();
    owns_storage = state.kind == SYSTEM_RESET_FACTORY;
    worker = next;
    publish(SYSTEM_RESET_RUNNING, RT_EOK, NULL);
    rt_err_t error = rt_thread_startup(next);
    if (error != RT_EOK)
    {
        worker = NULL;
        rt_thread_delete(next);
        publish(SYSTEM_RESET_FAILED, error, NULL);
    }
    storage_unlock();
    release_if_finished();
}

rt_err_t system_reset_retry(void)
{
    if (worker || conflict() || (state.phase != SYSTEM_RESET_FAILED && state.phase != SYSTEM_RESET_WAIT_CARD))
        return -RT_EBUSY;
    /* Formatting is never retried against a replacement session. */
    if (!sleep_guard_acquire(&reset_hold)) return -RT_EBUSY;
    saved = false;
    prepare_started = rt_tick_get();
    publish(SYSTEM_RESET_PREPARING, RT_EOK, NULL);
    return RT_EOK;
}

rt_err_t system_reset_finish_without_card(void)
{
    if (worker || !state.factory_pending || !state.internal_done || !state.card_pending ||
        (state.phase != SYSTEM_RESET_WAIT_CARD && state.phase != SYSTEM_RESET_FAILED)) return -RT_EBUSY;
    reset_record_t record;
    bool exists;
    rt_err_t error = reset_record_read(&record, &exists);
    if (error != RT_EOK || !exists || record.step != RESET_ERASE_CARD) return -RT_EBUSY;
    if (!sleep_guard_acquire(&reset_hold)) return -RT_EBUSY;
    cancel_card = true;
    saved = false;
    prepare_started = rt_tick_get();
    publish(SYSTEM_RESET_PREPARING, RT_EOK, NULL);
    return RT_EOK;
}

void system_reset_dismiss(void)
{
    if (worker || system_reset_busy()) return;
    if (state.kind == SYSTEM_RESET_FACTORY && state.phase == SYSTEM_RESET_DONE)
    {
        if (!board_power_request(true)) publish(SYSTEM_RESET_DONE, -RT_EBUSY, NULL);
        return;
    }
    if (state.phase != SYSTEM_RESET_FAILED && state.phase != SYSTEM_RESET_DONE) return;
    if (state.kind != SYSTEM_RESET_CARD && network_reset_resume() != RT_EOK) return;
    owns_storage = false;
    publish(SYSTEM_RESET_IDLE, RT_EOK, NULL);
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
    wlan_service_resume_for_reset();
#endif
    app_catalog_changed();
}
