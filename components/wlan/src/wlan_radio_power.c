#include "wlan_radio_power.h"

#include "wifi_port.h"
#include "sys_api.h"

/* wifi_port.c exports this entry without a public function declaration. */
extern void wifi_driver_action(unsigned int act);

/* Official wifi_port_priv.h operation values; 0x05/0x06 are PD/PU. */
enum
{
    WLAN_DRIVER_OPEN = 0x01,
    WLAN_DRIVER_CLOSE = 0x02,
    WLAN_RADIO_TIMEOUT_MS = 10000,
    WLAN_RADIO_POLL_MS = 20
};

struct radio_snapshot
{
    uint32_t operation;
    uint32_t init_status;
    int initialized;
    int queued;
    AIC_WIFI_MODE mode;
    bool chip_active;
    bool manager_ready;
};

static bool radio_busy;
static bool radio_enabled;
static volatile bool power_allowed;

void wlan_radio_allow_power(bool allowed)
{
    power_allowed = allowed;
}

bool wlan_radio_power_allowed(void)
{
    return power_allowed;
}

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
#include <string.h>
#include <rthw.h>
#include <drivers/mmcsd_core.h>
#include <drivers/sdio.h>
#include <drivers/sd.h>
#include <drivers/pin.h>
#include "bf0_hal.h"
#ifdef EPD_BOARD_SF32LB57_EPD
#include "bsp_wifi_power.h"
#include "plat_port.h"
#include "wifi_if.h"
#include "wifi_api.h"
#include "netif_port.h"
extern int aicwf_sdio_drv_register(void);
extern void wifi_driver_init(void);
static bool board_supply_off = true;
static struct rt_mmcsd_host *boot_host;
static bool enumerating, driver_registered, manager_created, mac_initialized;

static bool radio_enable_supply(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool allowed = power_allowed;
    if (allowed)
    {
        bsp_wifi_set_supply(true);
        board_supply_off = false;
    }
    rt_hw_interrupt_enable(level);
    return allowed;
}
#endif

extern rt_int32_t __real_mmcsd_send_cmd(struct rt_mmcsd_host *, struct rt_mmcsd_cmd *, int);
extern void __real_mmcsd_send_request(struct rt_mmcsd_host *, struct rt_mmcsd_req *);
extern void __real_mmcsd_set_clock(struct rt_mmcsd_host *, rt_uint32_t);
extern rt_int32_t __real_sdio_detach_irq(struct rt_sdio_function *);
extern HAL_StatusTypeDef __real_HAL_DMA_PollForTransfer(DMA_HandleTypeDef *,
                                                       HAL_DMA_LevelCompleteTypeDef, uint32_t);
extern HAL_StatusTypeDef __real_HAL_DMA_Start(DMA_HandleTypeDef *, uint32_t, uint32_t, uint32_t);

enum { RADIO_SEEN_CMD = 1, RADIO_SEEN_DATA = 2, RADIO_SEEN_DETACH = 4,
       RADIO_SEEN_DMA = 8, RADIO_SEEN_ALL = 15,
       RADIO_IGNORE = 0, RADIO_TRACK = 1, RADIO_BLOCK = 2 };

static struct
{
    struct rt_mmcsd_host *host;
    struct rt_mmcsd_card *card;
    unsigned in_flight;
    unsigned seen;
    rt_err_t cycle_error;
    bool dma_uncertain;
    bool failed_open;
    bool closing;
    bool detached;
    bool closed;
    bool sealed;
    bool nvic_was_enabled;
} radio_bus;

static SD_TypeDef *radio_controller(IRQn_Type *irqn);
static rt_err_t radio_bus_reselect(void);
/* Protected by the public host bus mutex, including the error cleanup. */
static struct
{
    rt_thread_t owner;
    SD_TypeDef *controller;
    DMA_HandleTypeDef *dma;
    uintptr_t channel;
    uint32_t index;
    bool dma_pending;
} radio_transfer;

/* SDIO requests are serialized by the SDK host mutex. Other DMA users must
 * not replace this context while Start performs its cache writeback. */
struct sdio_dma_start_context
{
    rt_thread_t owner;
    DMA_HandleTypeDef *dma;
    uint32_t fifo;
    bool receive;
};
static struct sdio_dma_start_context sdio_dma_starts[2];

extern int __real_mpu_dcache_clean(void *, uint32_t);
int __wrap_mpu_dcache_clean(void *address, uint32_t size)
{
    int result = __real_mpu_dcache_clean(address, size);
    struct sdio_dma_start_context *context = RT_NULL;
    if (rt_interrupt_get_nest() == 0 && rt_thread_self() != RT_NULL)
        for (unsigned i = 0; i < 2; ++i)
            if (sdio_dma_starts[i].owner == rt_thread_self())
                context = &sdio_dma_starts[i];
    DMA_HandleTypeDef *dma = context == RT_NULL ? RT_NULL : context->dma;
    if (dma != RT_NULL &&
        dma->State == HAL_DMA_STATE_BUSY && dma->SrcAddress == (uintptr_t)address &&
        (context->receive ? dma->SrcAddress : dma->DstAddress) == context->fifo &&
        dma->TransCounts != 0 &&
        size == (dma->TransCounts << dma->SrcWidth) && dma->IsGPDMA && dma->OrgChannelIndex < 2)
    {
        /* HAL has allocated the channel and configured this segment, but has
         * not enabled it yet. Keep memory bursts; a fixed peripheral address
         * uses single beats. Also discard FIX bits left by a previous owner.
         * Do not mask interrupts during the original cache writeback above. */
        rt_base_t level = rt_hw_interrupt_disable();
        if ((dma->Instance->CCR & DMAC_CCR1_EN) == 0)
        {
            dma->Instance->CCR &= ~(GPDMA_CCR1_SFIX | GPDMA_CCR1_DFIX |
                (context->receive ? GPDMA_CCR1_SBURST_Msk : GPDMA_CCR1_DBURST_Msk));
        }
        rt_hw_interrupt_enable(level);
    }
    return result;
}

HAL_StatusTypeDef __wrap_HAL_DMA_Start(DMA_HandleTypeDef *dma, uint32_t src,
                                      uint32_t dst, uint32_t count)
{
    bool receive = dma->Init.Direction == DMA_PERIPH_TO_MEMORY;
    bool track_sdio = ((receive &&
         (src == (uint32_t)(uintptr_t)&SDIO1->FIFO || src == (uint32_t)(uintptr_t)&SDIO2->FIFO)) ||
        (!receive && dma->Init.Direction == DMA_MEMORY_TO_PERIPH &&
         (dst == (uint32_t)(uintptr_t)&SDIO1->FIFO || dst == (uint32_t)(uintptr_t)&SDIO2->FIFO))) &&
        dma->Init.PeriphInc == DMA_PINC_DISABLE &&
        dma->Init.MemInc == DMA_MINC_ENABLE && dma->Init.Mode == DMA_NORMAL &&
        dma->Init.MemDataAlignment == DMA_MDATAALIGN_WORD &&
        dma->Init.PeriphDataAlignment == DMA_PDATAALIGN_WORD && rt_interrupt_get_nest() == 0;
    struct sdio_dma_start_context *context = &sdio_dma_starts[
        (receive ? src : dst) == (uint32_t)(uintptr_t)&SDIO2->FIFO ? 1 : 0];
    if (track_sdio)
    {
        context->dma = dma;
        context->fifo = receive ? src : dst;
        context->receive = receive;
        context->owner = rt_thread_self();
    }
    HAL_StatusTypeDef result = __real_HAL_DMA_Start(dma, src, dst, count);
    if (track_sdio)
    {
        context->owner = RT_NULL;
        context->dma = RT_NULL;
    }
    if (result == HAL_OK && rt_interrupt_get_nest() == 0 && radio_transfer.owner != RT_NULL &&
        radio_transfer.owner == rt_thread_self() &&
        (src == (uint32_t)(uintptr_t)&radio_transfer.controller->FIFO ||
         dst == (uint32_t)(uintptr_t)&radio_transfer.controller->FIFO))
    {
        radio_transfer.dma = dma;
        radio_transfer.channel = (uintptr_t)dma->Instance;
        radio_transfer.index = dma->ChannelIndex;
        radio_transfer.dma_pending = true;
    }
    return result;
}

static bool radio_wifi_host(const struct rt_mmcsd_host *host)
{
    const struct rt_mmcsd_card *card = host == RT_NULL ? RT_NULL : host->card;
    /* Public CIS IDs from the official sdio_def.h, D40-N/LN/WN only. */
    return card != RT_NULL && card->card_type == CARD_TYPE_SDIO &&
           card->cis.manufacturer == 0xc8a1 &&
           (card->cis.product == 0x9084 || card->cis.product == 0x9085 ||
            card->cis.product == 0x9086);
}

void __wrap_mmcsd_set_clock(struct rt_mmcsd_host *host, rt_uint32_t clock)
{
    /* Use the board's default-speed ceiling until high speed is negotiated. */
    if (radio_wifi_host(host) && !(host->card->flags & CARD_FLAG_HIGHSPEED) &&
        clock > 24000000)
        clock = 24000000;
    __real_mmcsd_set_clock(host, clock);
}

static unsigned radio_bus_begin(struct rt_mmcsd_host *host, unsigned seen)
{
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned track = RADIO_IGNORE;
#ifdef EPD_BOARD_SF32LB57_EPD
    if (host && (host->flags & MMCSD_HOST_TYPE_MASK) == MMCSD_HOST_TYPE_SDIO)
    {
        /* The fixed SDIO module and the removable SD card use separate hosts.
         * Remember the former during the OS probe, even with the rail off. */
        boot_host = host;
        if (board_supply_off || enumerating)
        {
            /* The SDK cold probe can set up a clock before its first command.
             * With U13 off no transaction is allowed; first OPEN sets the
             * minimum clock again after reconnecting the bus. */
            if (board_supply_off) SET_BIT(SDIO1->CLKCR, SD_CLKCR_STOP_CLK);
            rt_hw_interrupt_enable(level);
            return board_supply_off ? RADIO_BLOCK : RADIO_IGNORE;
        }
    }
#endif
    if ((host != RT_NULL && host == radio_bus.host) || radio_wifi_host(host))
    {
        if (radio_bus.host == RT_NULL)
        {
            radio_bus.host = host;
            radio_bus.card = host->card;
        }
        if (host != radio_bus.host || host->card != radio_bus.card)
        {
            radio_bus.cycle_error = -RT_EIO;
            track = RADIO_BLOCK;
        }
        else if (radio_bus.sealed)
            track = RADIO_BLOCK;
        else
        {
            ++radio_bus.in_flight;
            radio_bus.seen |= seen;
            track = RADIO_TRACK;
        }
    }
    rt_hw_interrupt_enable(level);
    return track;
}

static void radio_bus_end(bool failed, bool dma_uncertain)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (failed)
    {
        radio_bus.cycle_error = -RT_EIO;
        if (dma_uncertain)
            radio_bus.dma_uncertain = true;
    }
    --radio_bus.in_flight;
    rt_hw_interrupt_enable(level);
}

static bool radio_command_failed(const struct rt_mmcsd_cmd *cmd)
{
    return cmd != RT_NULL && (cmd->err != RT_EOK ||
           (resp_type(cmd) == RESP_R5 && R5_STATUS(cmd->resp[0]) != 0));
}

rt_int32_t __wrap_mmcsd_send_cmd(struct rt_mmcsd_host *host,
                               struct rt_mmcsd_cmd *cmd, int retries)
{
    unsigned track = radio_bus_begin(host, RADIO_SEEN_CMD);
    rt_int32_t result;
    if (track == RADIO_BLOCK)
        return cmd->err = -RT_EBUSY;
    result = __real_mmcsd_send_cmd(host, cmd, retries);
    if (track == RADIO_TRACK)
        radio_bus_end(result != RT_EOK || radio_command_failed(cmd) ||
                      cmd->retries < retries - 1, false);
    return result;
}

void __wrap_mmcsd_send_request(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req)
{
    unsigned track = radio_bus_begin(host, req->data != RT_NULL ? RADIO_SEEN_DATA : 0);
    rt_int32_t retries = req->cmd->retries;
    IRQn_Type irqn;
    SD_TypeDef *controller = track == RADIO_TRACK ? radio_controller(&irqn) : RT_NULL;
    bool guarded = controller != RT_NULL && req->data != RT_NULL &&
                   req->cmd->cmd_code == SD_IO_RW_EXTENDED;
    bool dma_uncertain = req->data != RT_NULL;
    if (track == RADIO_BLOCK)
    {
        req->cmd->err = -RT_EBUSY;
        if (req->data != RT_NULL)
            req->data->err = -RT_EBUSY;
        if (req->stop != RT_NULL)
            req->stop->err = -RT_EBUSY;
        return;
    }
    if (guarded)
    {
        /* The SDK request takes this recursive mutex internally, but releases
         * it before returning. Retain one level until DMA cleanup is complete. */
        if (rt_mutex_take(&host->bus_lock, RT_WAITING_FOREVER) != RT_EOK)
        {
            req->cmd->err = req->data->err = -RT_EBUSY;
            radio_bus_end(true, false);
            return;
        }
        memset(&radio_transfer, 0, sizeof(radio_transfer));
        radio_transfer.owner = rt_thread_self();
        radio_transfer.controller = controller;
    }
    __real_mmcsd_send_request(host, req);
    if (guarded)
    {
        DMA_HandleTypeDef *dma = radio_transfer.dma;
        bool failed = radio_command_failed(req->cmd) || radio_command_failed(req->stop) ||
                      req->data->err != RT_EOK;
        if (failed && radio_transfer.dma_pending && dma && dma->State == HAL_DMA_STATE_BUSY &&
            (uintptr_t)dma->Instance == radio_transfer.channel &&
            dma->ChannelIndex == radio_transfer.index)
        {
            /* CMD53 errors skip the SDK's DMA completion wait. Stop the
             * original channel before the caller releases its data buffer. */
            HAL_StatusTypeDef stopped = HAL_DMA_Abort(dma);
            if (stopped == HAL_OK)
                radio_transfer.dma_pending = false;
            rt_kprintf("wlan: failed CMD53 DMA abort=%d\n", stopped);
        }
        dma_uncertain = radio_transfer.dma_pending;
        memset(&radio_transfer, 0, sizeof(radio_transfer));
        rt_mutex_release(&host->bus_lock);
    }
    if (track == RADIO_TRACK)
        radio_bus_end(radio_command_failed(req->cmd) || radio_command_failed(req->stop) ||
                      (req->data != RT_NULL && req->data->err != RT_EOK) ||
                      req->cmd->retries < retries - 1, dma_uncertain);
}

rt_int32_t __wrap_sdio_detach_irq(struct rt_sdio_function *func)
{
    unsigned track = radio_bus_begin(func->card->host, RADIO_SEEN_DETACH);
    rt_int32_t result;
    if (track == RADIO_BLOCK)
        return -RT_EBUSY;
    result = __real_sdio_detach_irq(func);
    if (track == RADIO_TRACK)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        if (radio_bus.closing && func->num == 1 && result == RT_EOK)
            radio_bus.detached = true;
        rt_hw_interrupt_enable(level);
        radio_bus_end(result != RT_EOK, false);
    }
    return result;
}

/* Public SF32LB57x sdio_config.h maps sd0/sd1 to these controllers. */
static SD_TypeDef *radio_controller(IRQn_Type *irqn)
{
    if (radio_bus.host == RT_NULL)
        return RT_NULL;
#ifdef BSP_USING_SDMMC1
    if (strcmp(radio_bus.host->name, "sd0") == 0)
    {
        *irqn = SDMMC1_IRQn;
        return SDIO1;
    }
#endif
#ifdef BSP_USING_SDMMC2
    if (strcmp(radio_bus.host->name, "sd1") == 0)
    {
        *irqn = SDMMC2_IRQn;
        return SDIO2;
    }
#endif
    return RT_NULL;
}

HAL_StatusTypeDef __wrap_HAL_DMA_PollForTransfer(DMA_HandleTypeDef *hdma,
                                                HAL_DMA_LevelCompleteTypeDef complete,
                                                uint32_t timeout)
{
    IRQn_Type irqn;
    SD_TypeDef *instance = radio_controller(&irqn);
    unsigned track = RADIO_IGNORE;
    HAL_StatusTypeDef result;
    /* SF32LB57x uses dynamic DMA allocation. These HAL fields retain the
     * non-incrementing peripheral address; no rthw_sdio layout is involved. */
    if (instance != RT_NULL &&
        ((uintptr_t)hdma->SrcAddress == (uintptr_t)&instance->FIFO ||
         (uintptr_t)hdma->DstAddress == (uintptr_t)&instance->FIFO))
        track = radio_bus_begin(radio_bus.host, RADIO_SEEN_DMA);
    if (track == RADIO_BLOCK)
        return HAL_ERROR;
    result = __real_HAL_DMA_PollForTransfer(hdma, complete, timeout);
    if (track == RADIO_TRACK)
    {
        bool guarded = radio_transfer.owner == rt_thread_self() && radio_transfer.dma == hdma;
        bool stopped = complete == HAL_DMA_FULL_TRANSFER && hdma->State == HAL_DMA_STATE_READY &&
            ((result == HAL_OK && hdma->LeftCounts == 0) || hdma->ErrorCode == HAL_DMA_ERROR_TE);
        if (guarded && stopped)
            radio_transfer.dma_pending = false;
        radio_bus_end(result != HAL_OK || complete != HAL_DMA_FULL_TRANSFER ||
                      hdma->State != HAL_DMA_STATE_READY || hdma->ErrorCode != HAL_DMA_ERROR_NONE ||
                      hdma->LeftCounts != 0, !guarded && !stopped);
    }
    return result;
}

/* Called under the scheduler lock, before submitting a fresh operation. */
static void radio_bus_operation(bool enabled)
{
    if (enabled)
    {
        IRQn_Type irqn;
        if (radio_bus.sealed && radio_controller(&irqn) != RT_NULL)
        {
            radio_bus.sealed = false;
            if (radio_bus.nvic_was_enabled)
                HAL_NVIC_EnableIRQ(irqn);
        }
    }
    /* A completed CLOSE is a fresh quiescence check, independent of an
     * earlier transfer error. Unconfirmed DMA shutdown remains latched. */
    radio_bus.cycle_error = RT_EOK;
    radio_bus.closing = !enabled;
    radio_bus.closed = false;
    radio_bus.detached = false;
}

/* Caller owns the host mutex and holds the scheduler lock. */
static rt_err_t radio_bus_seal_locked(bool failed_open)
{
    struct rt_mmcsd_host *host = radio_bus.host;
    SD_TypeDef *instance;
    IRQn_Type irqn;
    rt_err_t result = RT_EOK;
    unsigned i;
    bool nvic_enabled;

    if (radio_bus.dma_uncertain)
        return -RT_EIO;
    if (!failed_open && radio_bus.cycle_error != RT_EOK)
        return radio_bus.cycle_error;
    if (host == RT_NULL || (!failed_open && radio_bus.seen != RADIO_SEEN_ALL) ||
        (instance = radio_controller(&irqn)) == RT_NULL)
        return -RT_ENOSYS;
    /* Failed OPEN can unwind before a CLOSE or IRQ attach. Prove quiescence
     * from the live host and controller state in that case. */
    if ((!failed_open && (!radio_bus.closed || !radio_bus.detached)) ||
        radio_bus.in_flight || radio_transfer.owner)
        return -RT_EBUSY;
    if (host->card != radio_bus.card || !radio_wifi_host(host))
        return -RT_EIO;
    if (host->sdio_irq_num != 0 || host->sdio_irq_thread != RT_NULL ||
        host->sdio_irq_sem != RT_NULL)
        return -RT_EBUSY;
    for (i = 0; i <= SDIO_MAX_FUNCTIONS; ++i)
        if (host->card->sdio_function[i] != RT_NULL &&
            host->card->sdio_function[i]->irq_handler != RT_NULL)
            return -RT_EBUSY;
    if (host->ops == RT_NULL || host->ops->enable_sdio_irq == RT_NULL)
        return -RT_ENOSYS;
    /* No worker can start a request while this scheduler lock is held.
     * Mask NVIC too: a previously pending DATAEND ISR can re-enable DAT1. */
    nvic_enabled = HAL_NVIC_GetEnableIRQ(irqn) != 0;
    HAL_NVIC_DisableIRQ(irqn);
    host->ops->enable_sdio_irq(host, 0);
    if (HAL_SDMMC_GET_IRQ_MASK(instance) != 0 ||
        (HAL_SDMMC_GET_STA(instance) & (SD_SR_CMD_BUSY | SD_SR_DATA_BUSY)) != 0 ||
        HAL_NVIC_GetEnableIRQ(irqn) != 0)
        result = -RT_EBUSY;
    else
    {
        HAL_NVIC_ClearPendingIRQ(irqn);
        if (!radio_bus.sealed)
            radio_bus.nvic_was_enabled = nvic_enabled;
        radio_bus.sealed = true;
    }
    if (result != RT_EOK && nvic_enabled)
        HAL_NVIC_EnableIRQ(irqn);
    return result;
}

static rt_err_t radio_bus_seal(bool failed_open)
{
    struct rt_mmcsd_host *host = radio_bus.host;
    rt_err_t result;
    if (host == RT_NULL)
        return -RT_ENOSYS;
    /* RT-Thread forbids mutex operations while scheduling is locked, even
     * for a zero-timeout acquisition. Recheck all bus state after locking. */
    if (rt_mutex_take(&host->bus_lock, 0) != RT_EOK)
        return -RT_EBUSY;
    rt_enter_critical();
    result = radio_bus_seal_locked(failed_open);
    rt_exit_critical();
    rt_mutex_release(&host->bus_lock);
    return result;
}

static rt_err_t radio_power_down_locked(SD_TypeDef *controller)
{
#if defined(AIC8800_RTOS_WIRELESS_PWR_GPIO) && AIC8800_RTOS_WIRELESS_PWR_GPIO >= 0 && \
    !defined(AIC8800_RTOS_WIRELESS_PWR_VIA_IOEXP)
    extern void aicwf_sdio_pdn_wifi(void);
    /* Stop the bus before removing radio power, even with a negotiated clock. */
    SET_BIT(controller->CLKCR, SD_CLKCR_STOP_CLK);
    if (!(controller->CLKCR & SD_CLKCR_STOP_CLK)) return -RT_EIO;
    aicwf_sdio_pdn_wifi();
    if (rt_pin_read(AIC8800_RTOS_WIRELESS_PWR_GPIO) != PIN_LOW) return -RT_EIO;
#ifdef EPD_BOARD_SF32LB57_EPD
    bsp_wifi_set_supply(false);
    if (rt_pin_read(8) != PIN_LOW) return -RT_EIO;
    board_supply_off = true;
#endif
    return RT_EOK;
#else
    return -RT_ENOSYS;
#endif
}

static rt_err_t radio_bus_power_down(void)
{
    struct rt_mmcsd_host *host = radio_bus.host;
    IRQn_Type irqn;
    SD_TypeDef *controller = radio_controller(&irqn);
    if (!host || !controller || !radio_bus.sealed) return -RT_EBUSY;
    if (rt_mutex_take(&host->bus_lock, 0) != RT_EOK) return -RT_EBUSY;
    rt_err_t result = radio_power_down_locked(controller);
    rt_mutex_release(&host->bus_lock);
    return result;
}
#endif

/* Caller holds the scheduler lock. Use the SDK's published port definition. */
static struct radio_snapshot radio_snapshot_locked(void)
{
    struct radio_snapshot state;
    const volatile struct aic_wifi_dev *dev = &aicwf_wifi_dev;

    state.operation = dev->wifi_drive_op;
    state.init_status = dev->wifi_init_status;
    state.initialized = dev->g_wifi_init;
    state.mode = dev->wifi_mode;
    state.manager_ready = dev->wifi_task_sem != RT_NULL &&
                          dev->wifi_task_init_hdl != RT_NULL;
    state.queued = dev->wifi_task_sem == RT_NULL ? 0 :
                   aicwf_rtos_semaphore_get_count(dev->wifi_task_sem);
    state.chip_active = wifi_chip_status_get() != 0;
    return state;
}

static bool radio_idle(const struct radio_snapshot *state)
{
    return state->operation == 0 && state->queued == 0;
}

static bool radio_started(const struct radio_snapshot *state)
{
    return radio_idle(state) && state->initialized == 1 &&
           state->init_status == 1 && state->chip_active;
}

static bool radio_stopped(const struct radio_snapshot *state)
{
    return radio_idle(state) && state->initialized == 0 &&
           state->init_status == 0 && !state->chip_active;
}

static void radio_refresh_locked(const struct radio_snapshot *state)
{
    if (radio_started(state))
        radio_enabled = true;
    else if (radio_stopped(state))
        radio_enabled = false;
}

bool wlan_radio_is_enabled(void)
{
    bool enabled;

    rt_enter_critical();
    if (!radio_busy)
    {
        struct radio_snapshot state = radio_snapshot_locked();
        radio_refresh_locked(&state);
    }
    enabled = radio_enabled;
    rt_exit_critical();
    return enabled;
}

bool wlan_radio_is_ready(void)
{
    rt_enter_critical();
    struct radio_snapshot state = radio_snapshot_locked();
    bool ready = !radio_busy && state.manager_ready && radio_started(&state);
    rt_exit_critical();
    return ready;
}

static rt_err_t radio_wait_idle(rt_tick_t started, struct radio_snapshot *state)
{
    const rt_tick_t timeout = rt_tick_from_millisecond(WLAN_RADIO_TIMEOUT_MS);

    for (;;)
    {
        rt_enter_critical();
        *state = radio_snapshot_locked();
        rt_exit_critical();
        /* The official task clears wifi_drive_op before publishing
         * wifi_init_status; preemption between those stores is possible. */
        if (radio_idle(state) &&
            state->init_status == (uint32_t)state->initialized)
            return RT_EOK;
        if ((rt_tick_t)(rt_tick_get() - started) >= timeout)
            return -RT_ETIMEOUT;
        rt_thread_mdelay(WLAN_RADIO_POLL_MS);
    }
}

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
/* Called by the serialized lifecycle on the first actual ON request. */
static rt_err_t radio_first_open(struct radio_snapshot *state)
{
    struct rt_mmcsd_host *host = boot_host;
    rt_uint32_t ocr = 0;
    rt_err_t result;
    if (!host || strcmp(host->name, "sd0") != 0) return -RT_ENOSYS;
    result = rt_mutex_take(&host->bus_lock, rt_tick_from_millisecond(WLAN_RADIO_TIMEOUT_MS));
    if (result != RT_EOK) return result;
    if (!host->card)
    {
        if (!radio_enable_supply())
        {
            rt_mutex_release(&host->bus_lock);
            return -RT_EBUSY;
        }
        aicwf_platform_pwr_wifi_pin_init();
        aicwf_sdio_reset_wifi();
        bsp_wifi_connect_bus();
        enumerating = true;
        mmcsd_set_bus_mode(host, MMCSD_BUSMODE_OPENDRAIN);
        mmcsd_set_bus_width(host, MMCSD_BUS_WIDTH_1);
        mmcsd_set_clock(host, host->freq_min);
        result = mmcsd_go_idle(host);
        if (result == RT_EOK) result = sdio_io_send_op_cond(host, 0, &ocr);
        if (result == RT_EOK) result = init_sdio(host, ocr);
        enumerating = false;
    }
    if (result == RT_EOK && !radio_wifi_host(host)) result = -RT_EIO;
    if (result == RT_EOK)
    {
        radio_bus.host = host;
        radio_bus.card = host->card;
    }
    else
    {
        rt_err_t off = radio_power_down_locked(SDIO1);
        if (off != RT_EOK) result = off;
    }
    rt_mutex_release(&host->bus_lock);
    if (result != RT_EOK) return result;

    if (board_supply_off)
    {
        result = radio_bus_reselect();
        if (result != RT_EOK) return result;
    }
    if (!driver_registered)
    {
        result = aicwf_sdio_drv_register();
        if (result != RT_EOK) goto cold_off;
        driver_registered = true;
    }
    if (!power_allowed) { result = -RT_EBUSY; goto cold_off; }
#ifdef CONFIG_SDIO_BUS_PWRCTRL
    /* The SDK samples this switch before creating its SDIO power-control task. */
    aic_pwrctl_enable_set(1);
#endif
    wifi_driver_init(); /* Creates the SDK manager and submits its initial OPEN. */
    manager_created = true;
    result = radio_wait_idle(rt_tick_get(), state);
    if (result != RT_EOK) return result;
    return radio_started(state) ? RT_EOK : -RT_EIO;
cold_off:
    /* No driver task or IRQ handler has been started yet. */
    mmcsd_host_lock(host);
    rt_err_t off = radio_power_down_locked(SDIO1);
    mmcsd_host_unlock(host);
    return off == RT_EOK ? result : off;
}
#endif

rt_err_t wlan_radio_set_enabled(bool enabled, rt_bool_t *failed_open_powered_down)
{
    struct radio_snapshot state;
    rt_tick_t started;
    rt_err_t result;
    unsigned int operation = enabled ? WLAN_DRIVER_OPEN : WLAN_DRIVER_CLOSE;
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    bool roll_back = false;
#endif

    if (failed_open_powered_down) *failed_open_powered_down = RT_FALSE;
    if (rt_interrupt_get_nest() != 0 || rt_thread_self() == RT_NULL)
        return -RT_EINVAL;

    rt_enter_critical();
    if (radio_busy || (enabled && !power_allowed))
    {
        rt_exit_critical();
        return -RT_EBUSY;
    }
    state = radio_snapshot_locked();
    radio_refresh_locked(&state);
    radio_busy = true;
    rt_exit_critical();

    started = rt_tick_get();
    result = radio_wait_idle(started, &state);
    if (result != RT_EOK)
        goto done;
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    if (enabled && !manager_created)
    {
        result = radio_first_open(&state);
        goto done;
    }
#endif

    rt_enter_critical();
    /* Recheck under the same lock used to submit the SDK operation. */
    state = radio_snapshot_locked();
    if (!radio_idle(&state))
        result = -RT_EBUSY;
    else if (enabled && !radio_started(&state) && !radio_stopped(&state))
        result = -RT_EIO;
    else
    {
        radio_refresh_locked(&state);
        if (enabled ? radio_started(&state) : radio_stopped(&state))
            result = RT_EOK;
        else if (!state.manager_ready)
            result = -RT_ENOSYS;
        else
        {
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
            radio_bus_operation(enabled);
#endif
            rt_exit_critical();
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
            /* CLOSE stops the host tasks but leaves firmware running in the
             * module. A new OPEN loads boot-ROM firmware, so reset and restore
             * the existing SDIO card before submitting it. */
            if (enabled && radio_bus.host != RT_NULL)
            {
                result = radio_bus_reselect();
                if (result != RT_EOK)
                    goto done;
            }
#endif
            rt_enter_critical();
            state = radio_snapshot_locked();
            if (!radio_idle(&state))
            {
                rt_exit_critical();
                result = -RT_EBUSY;
                goto done;
            }
            /* CLOSE clears the mode. OPEN reads this public platform field
             * before a new driver instance exists, so restore it first. */
            if (enabled)
                aicwf_wifi_dev.wifi_mode = WIFI_MODE_STA;
            wifi_driver_action(operation);
            result = aicwf_wifi_dev.wifi_drive_op == operation ?
                     RT_EOK : -RT_EIO;
            rt_exit_critical();
            if (result != RT_EOK)
                goto done;

            result = radio_wait_idle(rt_tick_get(), &state);
            if (result != RT_EOK)
                goto done;

            /* wifi_chip_status is cleared early during CLOSE. Neither it,
             * a disconnect event, nor g_wifi_init alone proves completion. */
            if (enabled ? (radio_started(&state) && state.mode == WIFI_MODE_STA) :
                          (radio_stopped(&state) && state.mode == WIFI_MODE_UNKNOWN))
            {
                rt_enter_critical();
                radio_enabled = enabled;
                rt_exit_critical();
            }
            else
                result = -RT_EIO;
            goto done;
        }
    }
    rt_exit_critical();

done:
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    if (result == RT_EOK && enabled && !mac_initialized)
    {
        result = aic_wifi_init_mac();
        if (result == RT_EOK)
        {
            aicwf_net_if_set_tousedip(0x010da8c0u); /* SDK default AP, 192.168.13.1 */
            aicwf_net_if_set_netmask(0x00ffffffu);
            mac_initialized = true;
        }
    }
#endif
    rt_enter_critical();
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
    if (result != RT_EOK)
    {
        radio_bus.cycle_error = result;
        radio_bus.closed = false;
    }
    else if (!enabled && radio_bus.closing)
    {
        radio_bus.closed = true;
        radio_bus.failed_open = false;
    }
    radio_bus.closing = false;
#ifdef EPD_BOARD_SF32LB57_EPD
    if (enabled)
    {
        radio_bus.failed_open = result != RT_EOK;
        state = radio_snapshot_locked();
        roll_back = radio_bus.failed_open && !board_supply_off && radio_stopped(&state);
    }
#endif
#endif
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    if (roll_back)
    {
        /* Retain radio_busy across host verification and physical rollback. */
        rt_exit_critical();
        rt_err_t off = radio_bus_seal(true);
        if (off == RT_EOK) off = radio_bus_power_down();
        if (failed_open_powered_down) *failed_open_powered_down = off == RT_EOK;
        rt_kprintf("wlan: open failed (%d), power rollback=%d\n", result, off);
        rt_enter_critical();
    }
#endif
    radio_busy = false;
    rt_exit_critical();
    return result;
}

bool wlan_radio_power_off_confirmed(void)
{
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    rt_enter_critical();
    struct radio_snapshot state = radio_snapshot_locked();
    bool off = !radio_busy && !power_allowed && board_supply_off && radio_stopped(&state) &&
               rt_pin_read(8) == PIN_LOW && rt_pin_read(9) == PIN_LOW;
    rt_exit_critical();
    return off;
#else
    return false;
#endif
}

rt_err_t wlan_radio_prepare_deep(void)
{
    struct radio_snapshot state;
    rt_err_t result;
    bool failed_open = false;
    if (rt_interrupt_get_nest() != 0 || rt_thread_self() == RT_NULL)
        return -RT_EINVAL;
    rt_enter_critical();
    state = radio_snapshot_locked();
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED && defined(EPD_BOARD_SF32LB57_EPD)
    if (!radio_busy && board_supply_off && radio_stopped(&state))
    {
        rt_exit_critical();
        return rt_pin_read(8) == PIN_LOW && rt_pin_read(9) == PIN_LOW ? RT_EOK : -RT_EIO;
    }
    failed_open = radio_bus.failed_open;
#endif
    if (radio_busy || !radio_stopped(&state) ||
        (!failed_open && state.mode != WIFI_MODE_UNKNOWN))
    {
        rt_exit_critical();
        return -RT_EBUSY;
    }
    /* Reserve the lifecycle operation while the bus mutex is acquired in
     * normal thread context. OPEN/CLOSE cannot race the final handoff. */
    radio_busy = true;
    rt_exit_critical();
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
    result = radio_bus_seal(failed_open);
    if (result == RT_EOK) result = radio_bus_power_down();
#else
    result = -RT_ENOSYS;
#endif
    rt_enter_critical();
    radio_busy = false;
    rt_exit_critical();
    return result;
}

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
/* The soldered module keeps the same RT card/function objects. The official
 * port caches their addresses; neither unregister nor allocate replacements. */
int __wrap_aicwf_sdio_reset(void)
{
    extern int __real_aicwf_sdio_reset(void);
    /* Native fault recovery resets the module without restoring the host's
     * selected SDIO card. Reuse the checked CLOSE/OPEN reselect path. */
    if (!radio_bus.host) return __real_aicwf_sdio_reset();
    if (rt_thread_self() != (rt_thread_t)aicwf_wifi_dev.wifi_task_init_hdl)
        return -RT_EBUSY;
    return radio_bus_reselect();
}

static rt_err_t radio_bus_reselect(void)
{
    extern void aicwf_sdio_reset_wifi(void);
#ifdef EPD_BOARD_SF32LB57_EPD
    bool repowered = false;
#endif
    struct rt_mmcsd_host *host = radio_bus.host;
    struct rt_mmcsd_card *card = radio_bus.card;
    rt_uint32_t ocr = 0;
    rt_uint32_t clock;
    rt_int32_t result;
    rt_uint8_t value;
    IRQn_Type irqn;
    SD_TypeDef *controller = radio_controller(&irqn);
    if (!host || !card || !controller || !card->sdio_function[0])
        return -RT_ENOSYS;
    if (rt_mutex_take(&host->bus_lock, 0) != RT_EOK)
        return -RT_EBUSY;
    if (host->card != card || !radio_wifi_host(host))
    { result = -RT_EIO; goto out; }
    if (radio_bus.in_flight || radio_bus.dma_uncertain || radio_transfer.owner ||
        host->sdio_irq_num || host->sdio_irq_thread || host->sdio_irq_sem ||
        (HAL_SDMMC_GET_STA(controller) & (SD_SR_CMD_BUSY | SD_SR_DATA_BUSY)))
    { result = -RT_EBUSY; goto out; }
    for (unsigned i = 0; i <= SDIO_MAX_FUNCTIONS; ++i)
        if (card->sdio_function[i] && card->sdio_function[i]->irq_handler)
        { result = -RT_EBUSY; goto out; }
#ifdef EPD_BOARD_SF32LB57_EPD
    repowered = board_supply_off;
    if (!radio_enable_supply()) { result = -RT_EBUSY; goto out; }
#endif
    aicwf_sdio_reset_wifi();
#ifdef EPD_BOARD_SF32LB57_EPD
    bsp_wifi_connect_bus();
#endif
    mmcsd_set_clock(host, 400000);
    mmcsd_set_bus_width(host, MMCSD_BUS_WIDTH_1);
    result = mmcsd_go_idle(host);
    if (result) goto out;
    result = sdio_io_send_op_cond(host, 0, &ocr);
    if (result) goto out;
    ocr = mmcsd_select_voltage(host, ocr);
    if (!ocr) { result = -RT_EIO; goto out; }
    result = sdio_io_send_op_cond(host, ocr, RT_NULL);
    if (result) goto out;
    result = mmcsd_get_card_addr(host, &card->rca);
    if (result) goto out;
    mmcsd_set_bus_mode(host, MMCSD_BUSMODE_PUSHPULL);
    result = mmcsd_select_card(card);
    if (result) goto out;
    card->flags &= ~CARD_FLAG_HIGHSPEED;
    if (card->cccr.high_speed && (host->flags & MMCSD_SUP_HIGHSPEED))
    {
        value = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_SPEED, &result);
        if (result) goto out;
        result = sdio_io_writeb(card->sdio_function[0], SDIO_REG_CCCR_SPEED, value | SDIO_SPEED_EHS);
        if (result) goto out;
        card->flags |= CARD_FLAG_HIGHSPEED;
    }
    if ((host->flags & MMCSD_BUSWIDTH_4) && (!card->cccr.low_speed || card->cccr.bus_width))
    {
        value = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_BUS_IF, &result);
        if (result) goto out;
        result = sdio_io_writeb(card->sdio_function[0], SDIO_REG_CCCR_BUS_IF, value | SDIO_BUS_WIDTH_4BIT);
        if (result) goto out;
        mmcsd_set_bus_width(host, MMCSD_BUS_WIDTH_4);
    }
    clock = (card->flags & CARD_FLAG_HIGHSPEED) ? 50000000 : card->cis.max_tran_speed;
    if (clock > host->freq_max) clock = host->freq_max;
    mmcsd_set_clock(host, clock);
out:
#ifdef EPD_BOARD_SF32LB57_EPD
    if (result != RT_EOK && repowered && !radio_bus.dma_uncertain &&
        !radio_bus.in_flight && !radio_transfer.owner &&
        !(controller->SR & (SD_SR_CMD_BUSY | SD_SR_DATA_BUSY)))
    {
        rt_err_t rollback = radio_power_down_locked(controller);
        if (rollback != RT_EOK)
            rt_kprintf("wlan: supply rollback failed: %d\n", rollback);
    }
#endif
    rt_mutex_release(&host->bus_lock);
    if (result != RT_EOK)
        rt_kprintf("wlan: SDIO reselect failed: %d\n", result);
    return result;
}
#endif
