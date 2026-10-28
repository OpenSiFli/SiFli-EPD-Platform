#ifndef EPD_USB_CONFIG_H
#define EPD_USB_CONFIG_H
#include <rtthread.h>

#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)
#define CONFIG_USB_DBG_LEVEL USB_DBG_WARNING
#define CONFIG_USB_ALIGN_SIZE 4
#define USB_NOCACHE_RAM_SECTION
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif
#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_MSC_MAX_LUN 1
/* 4 sectors per transfer, aligned for native SD block I/O. */
#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 2048
#endif
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "SiFli"
#define CONFIG_USBDEV_MSC_PRODUCT_STRING "TF Card Reader"
#define CONFIG_USBDEV_MSC_VERSION_STRING "1.00"
/* USB completion callbacks wake the resident worker to dispatch pending I/O. */
#define CONFIG_USBDEV_MSC_POLLING
#define CONFIG_USB_MUSB_EP_NUM 8
#define CONFIG_USB_MUSB_SIFLI
/* The SDK's shared SiFli glue also includes host type declarations. */
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_INTERFACES 8
#define CONFIG_USBHOST_MAX_EHPORTS 4
#endif
