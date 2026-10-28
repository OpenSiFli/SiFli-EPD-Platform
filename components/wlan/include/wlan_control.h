#ifndef WLAN_CONTROL_H
#define WLAN_CONTROL_H

#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_dev.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project security values use the unused WPA3 bit without changing SDK enums. */
#define WLAN_SECURITY_WPA3_SAE ((rt_wlan_security_t)(0x00800000u | AES_ENABLED))
#define WLAN_SECURITY_WPA2_WPA3_PSK ((rt_wlan_security_t)(0x00800000u | WPA2_SECURITY | AES_ENABLED))
#define WLAN_SAE_PASSWORD_MIN_LENGTH 8
/* STA: 0 disables low power; 1 selects clock gating. */
#define WLAN_STA_POWERSAVE_MIN_LEVEL 0
#define WLAN_STA_POWERSAVE_MAX_LEVEL 1
/* AP active:doze presets 3:7, 5:5, 7:3, 8:2, 9:1; not an on/off switch. */
#define WLAN_AP_POWERSAVE_MIN_LEVEL 1
#define WLAN_AP_POWERSAVE_MAX_LEVEL 5

#define WLAN_CTRL_GET_CAPS     0x5740
#define WLAN_CTRL_CANCEL       0x5741
#define WLAN_CTRL_GET_STATE    0x5742
#define WLAN_CTRL_PREPARE_DEEP 0x5743
#define WLAN_CTRL_CLAIM        0x5744
#define WLAN_CTRL_P2P_FIRST    0x5750
#define WLAN_CTRL_P2P_LAST     0x575f

#define WLAN_CAP_SCAN_FILTERS  (1u << 0)
#define WLAN_CAP_JOIN_BSSID    (1u << 1)
#define WLAN_CAP_AP            (1u << 2)
#define WLAN_CAP_AP_CLIENTS    (1u << 3)
#define WLAN_CAP_AP_DEAUTH     (1u << 4)
#define WLAN_CAP_STA_AP        (1u << 5)
#define WLAN_CAP_POWERSAVE     (1u << 6) /* For the queried wlan device's role. */
#define WLAN_CAP_CANCEL        (1u << 7)
#define WLAN_CAP_P2P           (1u << 8)
#define WLAN_CAP_AP_P2P        (1u << 9)
#define WLAN_CAP_AP_HIDDEN     (1u << 10)
#define WLAN_CAP_SUSPEND       (1u << 11) /* Supports the checked PREPARE_DEEP handoff. */

#define WLAN_AUTH_OPEN         (1u << 0)
#define WLAN_AUTH_WEP          (1u << 1)
#define WLAN_AUTH_WPA_PSK      (1u << 2)
#define WLAN_AUTH_WPA2_PSK     (1u << 3)
#define WLAN_AUTH_WPA3_SAE     (1u << 4)
#define WLAN_AUTH_WPA2_WPA3    (1u << 5)

#define WLAN_ROLE_STA          (1u << 0)
#define WLAN_ROLE_AP           (1u << 1)
#define WLAN_ROLE_P2P          (1u << 2)
#define WLAN_CANCEL_SCAN       (1u << 0)
#define WLAN_CANCEL_JOIN       (1u << 1)
#define WLAN_CANCEL_ALL        (WLAN_CANCEL_SCAN | WLAN_CANCEL_JOIN)

struct wlan_control_caps
{
    rt_uint32_t version;
    rt_uint32_t flags;
    rt_uint32_t sta_security;
    rt_uint32_t ap_security;
};

struct wlan_control_state
{
    rt_uint32_t active_roles;
    rt_uint32_t busy;       /* WLAN_CANCEL_SCAN/JOIN bits until the call returns. */
    rt_uint32_t cancelled;
    rt_bool_t radio_enabled;
};

/* CANCEL takes a rt_uint32_t mask, or NULL for ALL. -RT_EBUSY means the
 * request was recorded but the blocking host operation is still draining.
 * It never takes the RT-WLAN management/device lock or calls the host driver.
 * GET_CAPS and GET_STATE write the corresponding structures on wlan0/wlan1. */

/* CLAIM takes a rt_bool_t: true reserves operations for the calling thread
 * and cancels an older JOIN/SCAN. EBUSY retains the reservation while the host
 * call drains. The owner must release with false, including after timeout. */

/* PREPARE_DEEP takes NULL and runs synchronously in the WLAN worker after all
 * roles have reached NONE. Only RT_EOK grants the DEEP handoff for that close
 * cycle; errors (including -RT_EBUSY) are completed check results, not a queued
 * operation. WLAN_CAP_SUSPEND advertises this check, not unconditional safety.
 * Success also stops the card clock and lowers the module power-enable pin.
 * Reopening through standard STATION mode powers up and reselects the SDIO
 * card before restoring radio bus access. */

#ifdef __cplusplus
}
#endif
#endif
