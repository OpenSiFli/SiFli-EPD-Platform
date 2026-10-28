#include "usb_card_reader.h"
#include "system_reset_guard.h"
#include <rtthread.h>
#include <string.h>

#ifdef EPD_USB_CARD_READER
#include "sleep_guard.h"
#include "platform/epd_power.h"
#include <rtdevice.h>
#include <rthw.h>
#include "bf0_hal.h"
#include "sdcard.h"
#include "storage.h"
#include "usbd_core.h"
#include "usbd_msc.h"

#define MSC_OUT 0x02
#define MSC_IN 0x85
/* SDK MSC callbacks are exported by usbd_msc.c, but not its header. */
extern void mass_storage_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes);
extern void mass_storage_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes);
static uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0, 0, 0, 0x38f4, 0xffff, 0x0100, 1)
};
static const uint8_t configuration_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(9 + MSC_DESCRIPTOR_LEN, 1, 1, USB_CONFIG_BUS_POWERED, 100),
    MSC_DESCRIPTOR_INIT(0, MSC_OUT, MSC_IN, 64, 2)
};
static const uint8_t *device_desc(uint8_t speed) { (void)speed; return device_descriptor; }
static const uint8_t *config_desc(uint8_t speed) { (void)speed; return configuration_descriptor; }
static const char *string_desc(uint8_t speed, uint8_t index)
{
    (void)speed;
    /* No serial descriptor: Windows identifies this removable reader by port. */
    static const char *const strings[] = {"\x09\x04", "SiFli", "EPD TF Card Reader"};
    return index < sizeof(strings) / sizeof(strings[0]) ? strings[index] : NULL;
}
static const struct usb_descriptor descriptors = {
    .device_descriptor_callback = device_desc,
    .config_descriptor_callback = config_desc,
    .string_descriptor_callback = string_desc
};
static struct usbd_interface msc_interface;
static rt_thread_t worker;
static struct rt_semaphore wake;
static volatile bool requested;
static volatile bool initialized;
static volatile bool configured;
static volatile int init_error;
static volatile bool disconnected;
static volatile bool data_pending;
static sleep_guard_owner_t reader_hold = {.id = "usb-card-reader"};
static bool reader_changing;
static sdcard_export_info_t media;

void usb_card_reader_notify(void)
{
    if (worker) rt_sem_release(&wake);
}

static void bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    mass_storage_bulk_out(busid, ep, nbytes);
    data_pending = true;
    usb_card_reader_notify();
}

static void bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    mass_storage_bulk_in(busid, ep, nbytes);
    data_pending = true;
    usb_card_reader_notify();
}

static struct usbd_endpoint reader_out = {.ep_addr = MSC_OUT, .ep_cb = bulk_out};
static struct usbd_endpoint reader_in = {.ep_addr = MSC_IN, .ep_cb = bulk_in};

static bool release_if_returned(void)
{
    /* Serialize against a new export and its acquire before inspecting return. */
    rt_base_t level = rt_hw_interrupt_disable();
    sdcard_snapshot_t card;
    sdcard_get_snapshot(&card);
    if (!reader_changing && !requested && !initialized && !card.export_requested &&
        (card.state == SDCARD_READY || card.state == SDCARD_ABSENT ||
         (card.state == SDCARD_FAULT && card.error_stage != SDCARD_ERROR_EXPORT_CLOSE)))
        sleep_guard_release(&reader_hold);
    bool returned = !reader_hold.held;
    rt_hw_interrupt_enable(level);
    return returned;
}

static bool cable_present(void)
{
    return rt_pin_read(EPD_USB_DETECT_PIN) == PIN_HIGH;
}

void usb_card_reader_status(usb_card_reader_status_t *out)
{
    release_if_returned();
    sdcard_snapshot_t card;
    sdcard_get_snapshot(&card);
    *out = (usb_card_reader_status_t){
        .supported = true, .connected = cable_present(),
        .enabled = requested,
        .ready = requested && initialized && configured && cable_present() &&
                 card.state == SDCARD_EXPORTED && card.export_requested,
        .busy = (requested && !initialized) || (!requested && reader_hold.held) ||
                card.state == SDCARD_DRAINING || card.state == SDCARD_UNMOUNTING,
        .error = init_error ? init_error : card.error
    };
}

static void usb_event(uint8_t busid, uint8_t event)
{
    (void)busid;
    /* The SiFli MUSB port does not signal disconnect on every cable removal.
     * The worker also reads this board's VBUS detect input. Suspend is not eject. */
    if (event == USBD_EVENT_DISCONNECTED) disconnected = true;
    if (event == USBD_EVENT_CONFIGURED) configured = true;
    if (event == USBD_EVENT_RESET || event == USBD_EVENT_DISCONNECTED ||
        event == USBD_EVENT_DEINIT)
    {
        configured = false;
        data_pending = false;
    }
    usb_card_reader_notify();
    epd_power_notify();
}

static void stop_reader(void)
{
    /* Dispatch and shutdown share a thread, so block I/O finishes before deinit. */
    if (initialized)
    {
        usbd_deinitialize(0);
#ifdef RT_USING_PM
        rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        initialized = false;
        configured = false;
        data_pending = false;
        rt_kprintf("usb_card: stopped\n");
        storage_lock();
        sdcard_export_info_t current;
        if (sdcard_export_info(&current) == RT_EOK && current.session == media.session)
        {
            int result = sdcard_export_sync();
            if (result) init_error = result;
        }
        storage_unlock();
    }
    memset(&media, 0, sizeof(media));
    sdcard_snapshot_t card;
    sdcard_get_snapshot(&card);
    if (card.export_requested)
    {
        int result = sdcard_set_export(RT_FALSE);
        if (result) init_error = result;
    }
}

static void start_reader(void)
{
    storage_lock();
    int result = sdcard_export_info(&media);
    storage_unlock();
    if (result) return;
    /* The SDK caches capacity at interface initialization, as in its SD demo. */
    if (!media.ready || !media.block_count || !media.block_size ||
        CONFIG_USBDEV_MSC_MAX_BUFSIZE % media.block_size)
    {
        init_error = -RT_EINVAL;
        requested = false;
        return;
    }
    device_descriptor[16] = 0; /* SDK macro otherwise requests a serial string. */
    disconnected = false;
    configured = false;
    data_pending = false;
    usbd_desc_register(0, &descriptors);
    usbd_add_interface(0, usbd_msc_init_intf(0, &msc_interface, MSC_OUT, MSC_IN));
    usbd_add_endpoint(0, &reader_out);
    usbd_add_endpoint(0, &reader_in);
#ifdef RT_USING_PM
    /* Keep the USB controller running while the worker waits for events. */
    rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
    result = usbd_initialize(0, (uintptr_t)USBC_BASE, usb_event);
    if (result)
    {
        init_error = result;
        usbd_deinitialize(0);
#ifdef RT_USING_PM
        rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        requested = false;
        rt_kprintf("usb_card: start failed (%d)\n", result);
        return;
    }
    initialized = true;
    rt_kprintf("usb_card: started, event worker, buffer=%u\n",
               (unsigned)CONFIG_USBDEV_MSC_MAX_BUFSIZE);
}

static void reader_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        sdcard_snapshot_t card;
        sdcard_get_snapshot(&card);
        if (disconnected || !cable_present() || !card.export_requested ||
            card.state == SDCARD_FAULT || card.state == SDCARD_ABSENT ||
            (initialized && usbd_msc_get_popup(0)))
            requested = false;

        if (requested && !initialized && card.state == SDCARD_EXPORTED)
            start_reader();
        rt_base_t level = rt_hw_interrupt_disable();
        bool dispatch = data_pending;
        data_pending = false;
        rt_hw_interrupt_enable(level);
        if (requested && initialized && dispatch)
            usbd_msc_polling(0);
        if (!requested)
        {
            stop_reader();
            disconnected = false;
        }
        release_if_returned();
        /* Timeout checks VBUS only; data processing is signalled by USB IRQs. */
        rt_sem_take(&wake, reader_hold.held ? rt_tick_from_millisecond(100) : RT_WAITING_FOREVER);
    }
}

static bool set_enabled(bool enabled)
{
    if (!enabled)
    {
        requested = false;
        usb_card_reader_notify();
        return true;
    }
    if (system_reset_blocks_apps()) return false;
    if (requested) return true;
    if (!cable_present() || reader_hold.held) return false;
    if (!sleep_guard_acquire(&reader_hold)) return false;
    if (!worker)
    {
        if (rt_sem_init(&wake, "usb_card", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
            return false;
        worker = rt_thread_create("usb_card", reader_worker, NULL, 2048, 4, 10);
        if (!worker) { rt_sem_detach(&wake); return false; }
        if (sdcard_set_export(RT_TRUE) != RT_EOK)
        { rt_thread_delete(worker); worker = NULL; rt_sem_detach(&wake); return false; }
        init_error = 0;
        requested = true;
        if (rt_thread_startup(worker) != RT_EOK)
        {
            requested = false;
            (void)sdcard_set_export(RT_FALSE);
            rt_thread_delete(worker); worker = NULL; rt_sem_detach(&wake); return false;
        }
        return true;
    }
    if (sdcard_set_export(RT_TRUE) != RT_EOK) return false;
    init_error = 0;
    requested = true;
    usb_card_reader_notify();
    return true;
}

bool usb_card_reader_set_enabled(bool enabled)
{
    rt_base_t level = rt_hw_interrupt_disable();
    reader_changing = true;
    rt_hw_interrupt_enable(level);
    bool accepted = set_enabled(enabled);
    level = rt_hw_interrupt_disable();
    reader_changing = false;
    rt_hw_interrupt_enable(level);
    release_if_returned();
    epd_power_notify();
    return accepted;
}

void usbd_msc_get_cap(uint8_t bus, uint8_t lun, uint32_t *count, uint32_t *size)
{
    *count = bus || lun ? 0 : media.block_count;
    *size = bus || lun ? 0 : media.block_size;
}

static int transfer(uint8_t bus, uint8_t lun, uint32_t sector,
                    uint8_t *data, uint32_t size, bool write)
{
    if (bus || lun || !requested || !initialized || !cable_present()) return -1;
    storage_lock();
    sdcard_export_info_t current;
    int result = sdcard_export_info(&current);
    if (result == RT_EOK && current.session != media.session) result = -RT_EBUSY;
    if (result == RT_EOK)
        result = write ? sdcard_export_write(sector, data, size) :
                         sdcard_export_read(sector, data, size);
    storage_unlock();
    return result;
}
int usbd_msc_sector_read(uint8_t bus, uint8_t lun, uint32_t sector, uint8_t *data, uint32_t size)
{ return transfer(bus, lun, sector, data, size, false); }
int usbd_msc_sector_write(uint8_t bus, uint8_t lun, uint32_t sector, uint8_t *data, uint32_t size)
{ return transfer(bus, lun, sector, data, size, true); }
#else
void usb_card_reader_status(usb_card_reader_status_t *status)
{ memset(status, 0, sizeof(*status)); }
bool usb_card_reader_set_enabled(bool enabled) { return !enabled; }
void usb_card_reader_notify(void) {}
#endif

bool usb_card_reader_active(void)
{
    usb_card_reader_status_t status;
    usb_card_reader_status(&status);
    return status.enabled || status.busy;
}
