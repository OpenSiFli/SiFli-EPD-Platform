/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file bt_pan.c
 * @brief 蓝牙 PAN 联网服务实现
 *
 * 链路：手机配对连接（ACL/加密）→ 延迟发起 PAN 连接（手机作 NAP）
 *       → PAN 连通后设备获得网络出口。
 *
 * Unexpected link loss is handled solely by the SDK connection manager.
 *
 * 注意：对外符号统一使用 btpan_ 前缀（SDK 已占用 bt_pan_ 前缀）。
 */
#include "network_internal.h"
#include "bt_pan.h"
#include "epd_memory.h"
#include "bt_audio.h"
#include "network.h"
#include "network_reset.h"
#include "wlan_service.h"

#include <string.h>
#include <rthw.h>

#include "bts2_app_inc.h"
#include "ble_connection_manager.h"
#include "bt_connection_manager.h"
#include "ulog.h"
#include "bf0_ble_common.h" /* bd_addr_t / ble_common_update_type_t / ble_get_public_address */
#include "bf0_sibles_nvds.h"
#include "bf0_ble_gap.h"

/*---------------------------------------------------------------------------*/
/* 配置常量 */
/*---------------------------------------------------------------------------*/
#define PAN_TIMER_MS           3000
#define PAN_THREAD_STACK_SIZE  4096
#define PAN_THREAD_PRIORITY    20
#define PAN_THREAD_TICK        20
#define PAN_LOCAL_NAME_MAX     (BTPAN_LOCAL_NAME_MAX_BYTES + 1)

/*---------------------------------------------------------------------------*/
/* 内部消息 */
/*---------------------------------------------------------------------------*/
typedef enum
{
    PAN_MSG_STACK_READY = 1, /* 蓝牙协议栈初始化完成 */
    PAN_MSG_CONNECT_PAN = 2, /* 发起 PAN 连接（由定时器/外部请求触发） */
    PAN_MSG_APPLY_ENABLED = 3, /* 在工作线程应用蓝牙开关 */
    PAN_MSG_RESET = 5,
    PAN_MSG_DEVICES = 6,
    PAN_MSG_SUSPEND = 7,
} pan_msg_t;

#define PAN_MSG_BITS 8
#define PAN_GENERATION_MASK 0x00FFFFFFu

/*---------------------------------------------------------------------------*/
/* 模块状态 */
/*---------------------------------------------------------------------------*/
typedef struct
{
    rt_bool_t initialized;
    rt_bool_t enabled;      /* 设置页蓝牙开关 */
    rt_bool_t stop_bluetooth;
    rt_bool_t pan_draining; /* 等待旧 PAN 连接/断开请求完成 */
    rt_bool_t pan_cancelled; /* Discard failure replies from an intentional disconnect. */
    rt_bool_t earphone_draining; /* Keep old audio ACL attempts gated until CM releases them. */
    rt_uint32_t generation;
    rt_bool_t stack_ready;
    rt_bool_t stack_ready_pending;
    rt_bool_t apply_pending;
    rt_bool_t bt_connected; /* 手机链路已建立 */
    rt_bool_t pan_connected;
    rt_bool_t pan_failed;   /* No further PAN attempt on a failed phone connection. */
    rt_bool_t connect_pending;
    rt_bool_t manual_disconnect;
    rt_bool_t restore_pending; /* One request after Bluetooth enable or sleep. */
    bt_notify_device_mac_t bd_addr;
    rt_mailbox_t mailbox;
    rt_timer_t pan_connect_timer;
    rt_thread_t worker;
    char local_name[PAN_LOCAL_NAME_MAX];
    btpan_state_t last_state;
} btpan_service_t;

static btpan_service_t g_pan;
static struct rt_mutex g_pan_lock;

typedef struct {
    BTS2S_BD_ADDR address;
    unsigned phase;
} btpan_drain_request_t;

/* Independent of g_pan: a no-service suspend also fences later initialization. */
static struct {
    bool held, resume_pending, resume_posted, initializing, controller_closed;
    bool scan_pending, scan_write_pending, scan_confirmed, scan_failed, scan_started;
    bool started_work, restoring, progress;
    uint8_t expected_scan;
    btpan_drain_request_t acl_requests[BT_CM_DEVICE_MAX_CONN];
    btpan_drain_request_t pan_requests[PAN_MAX_NUM];
    rt_tick_t started;
    rt_err_t result;
    uint8_t ble_activities[32];
    bool ble_scanning, ble_initiating;
} g_suspend;

#define BTPAN_SUSPEND_TIMEOUT_MS 30000

static struct {
    btpan_snapshot_t snapshot;
    uint8_t target[6];
    rt_tick_t started;
    bool command_pending, cancel_scan, scan_stopping, refresh, name_pending;
    bool unpair_wait, unpair_verify, scan_barrier_pending;
} g_devices;

/* Separate lifetime: a boot recovery can pause before btpan_init clears g_pan. */
static struct
{
    network_reset_status_t status;
    rt_tick_t started;
    rt_bool_t ble_ready;
    rt_bool_t erase_unconfirmed;
    rt_bool_t scan_pending;
    rt_bool_t clear_started;
    rt_bool_t classic_query_pending;
    rt_bool_t wlan_reset_started;
} g_reset;

#define NETWORK_RESET_POLL_MS 100
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
/* Allow the BT 30s budget plus native join (30s), management wait (10s),
 * credential-save fence (5s), and persistence headroom. */
#define NETWORK_RESET_TIMEOUT_MS 90000
#else
#define NETWORK_RESET_TIMEOUT_MS 30000
#endif

static void network_reset_step(void);
static void btpan_devices_step(void);
static void btpan_devices_event(uint16_t type, uint16_t event, uint8_t *data, uint16_t size);
static void btpan_devices_query(uint16_t type, uint16_t event, uint8_t *data);
static btpan_device_t *btpan_device(const uint8_t address[6], bool add);
static void btpan_devices_finish(rt_err_t error);
static void btpan_stop_scan(void);
static void btpan_suspend_step(void);
static void btpan_scan_mode(uint8_t mode);
static rt_base_t network_reset_lock(void);
static void network_reset_unlock(rt_base_t level);

static rt_bool_t btpan_bluetooth_allowed(void)
{
    return !g_suspend.held && !g_reset.status.paused && g_pan.enabled && !g_pan.stop_bluetooth;
}

static rt_bool_t btpan_earphone_allowed(void)
{
    return btpan_bluetooth_allowed() && !g_pan.earphone_draining;
}

static bt_cm_link_type_t btpan_class_link_type(uint32_t dev_cls)
{
    uint32_t major = dev_cls & 0x1f00u;
    if (major == BT_DEVCLS_PHONE || major == BT_DEVCLS_COMPUTER) return BT_LINK_PHONE;
    return major == BT_DEVCLS_AUDIO || (dev_cls & (BT_SRVCLS_AUDIO | BT_SRVCLS_RENDER)) ?
           BT_LINK_EARPHONE : BT_LINK_PHONE;
}

static bool btpan_class_known(uint32_t dev_cls)
{
    uint32_t major = dev_cls & 0x1f00u;
    return major == BT_DEVCLS_PHONE || major == BT_DEVCLS_COMPUTER ||
           major == BT_DEVCLS_AUDIO || (dev_cls & (BT_SRVCLS_AUDIO | BT_SRVCLS_RENDER));
}

static bool btpan_cm_peer(const uint8_t address[6], bt_cm_dev_acl_info_t *out)
{
    BTS2S_BD_ADDR bd;
    bd_addr_t mac;
    memcpy(mac.addr, address, sizeof(mac.addr));
    bt_addr_convert_to_bts(&mac, &bd);
    rt_base_t level = rt_hw_interrupt_disable();
    bt_cm_dev_acl_info_t *peer = bt_cm_get_conn_by_addr(bt_cm_get_env(), &bd);
    if (peer) *out = *peer;
    rt_hw_interrupt_enable(level);
    return peer != NULL;
}

static bt_cm_link_type_t btpan_link_type(const uint8_t address[6])
{
    bt_cm_dev_acl_info_t peer;
    if (btpan_cm_peer(address, &peer)) return peer.info.link_type;
    btpan_device_t *device = btpan_device(address, false);
    /* A fresh device class corrects an old saved role before opening an ACL. */
    if (device && btpan_class_known(device->device_class))
        return btpan_class_link_type(device->device_class);
    BTS2S_BD_ADDR bd;
    bd_addr_t mac;
    memcpy(mac.addr, address, sizeof(mac.addr));
    bt_addr_convert_to_bts(&mac, &bd);
    rt_base_t level = rt_hw_interrupt_disable();
    bt_cm_bonded_dev_t *bonds = bt_cm_get_bonded_dev();
    for (unsigned i = 0; i < BT_CM_MAX_BOND; ++i)
        if (bonds->info[i].is_use && bd_eq(&bonds->info[i].bd_addr, &bd))
        {
            bt_cm_link_type_t type = bonds->info[i].link_type;
            rt_hw_interrupt_enable(level);
            return type;
        }
    rt_hw_interrupt_enable(level);
    return device && device->earphone ? BT_LINK_EARPHONE : BT_LINK_PHONE;
}

static bool btpan_earphone_active(void)
{
    bool active = false;
    rt_base_t level = rt_hw_interrupt_disable();
    bt_cm_device_manager_t *cm = bt_cm_get_env();
    for (unsigned i = 0; i < BT_CM_DEVICE_MAX_CONN; ++i)
        if (cm->bt_devices[i].is_use && cm->bt_devices[i].info.link_type == BT_LINK_EARPHONE)
            active = true;
    rt_hw_interrupt_enable(level);
    return active;
}

static rt_bool_t btpan_pan_allowed(void)
{
    return btpan_bluetooth_allowed() && !g_pan.pan_draining && !g_pan.manual_disconnect;
}

static rt_bool_t btpan_pan_connect_allowed(void)
{
    return btpan_pan_allowed() && !g_pan.pan_failed;
}

static void btpan_sync_target(void)
{
#ifdef BSP_BT_CONNECTION_MANAGER
    /* CM rereads both role targets on timeout and after profile events.
     * Clearing a target also gates already-armed CM timers. */
    bt_cm_set_profile_target(btpan_pan_connect_allowed() ? BT_CM_PAN : 0, BT_LINK_PHONE, 0);
    bt_cm_set_profile_target(btpan_earphone_allowed() ? BT_CM_A2DP | BT_CM_AVRCP : 0,
                             BT_LINK_EARPHONE, 0);
#endif
    bt_audio_set_enabled(btpan_earphone_allowed());
}

/*---------------------------------------------------------------------------*/
/* 状态计算与通知 */
/*---------------------------------------------------------------------------*/
static btpan_state_t btpan_current_state(void)
{
    if (!g_pan.initialized || !g_pan.enabled || g_reset.status.paused || g_suspend.held)
        return BTPAN_STATE_OFF;

    if (!g_pan.stack_ready)
        return BTPAN_STATE_INITIALIZING;

    if (btpan_pan_allowed() && g_pan.pan_connected)
        return BTPAN_STATE_NETWORK_READY;

    if (g_pan.bt_connected)
        return BTPAN_STATE_CONNECTED;

    return BTPAN_STATE_READY;
}

static void btpan_notify_state(void)
{
    btpan_state_t state = btpan_current_state();

    if (state == g_pan.last_state)
        return;

    g_pan.last_state = state;
    network_changed();
}

/*---------------------------------------------------------------------------*/
/* 内部工具函数 */
/*---------------------------------------------------------------------------*/
static rt_bool_t btpan_has_peer_addr(void)
{
    uint8_t index;

    for (index = 0; index < sizeof(g_pan.bd_addr.addr); index++)
    {
        if (g_pan.bd_addr.addr[index] != 0x00 && g_pan.bd_addr.addr[index] != 0xFF)
            return RT_TRUE;
    }

    return RT_FALSE;
}

static void btpan_reset_mailbox(void)
{
    /* Keep control messages; invalidate only queued PAN work. */
    g_pan.generation = (g_pan.generation + 1) & PAN_GENERATION_MASK;
    g_pan.connect_pending = RT_FALSE;
}

/**
 * @brief 向 PAN 工作线程投递内部事件。
 */
static rt_err_t btpan_post(pan_msg_t event)
{
    if (g_pan.mailbox == RT_NULL)
        return -RT_ERROR;

    rt_ubase_t value = (rt_ubase_t)event;
    if (event == PAN_MSG_CONNECT_PAN)
        value |= (rt_ubase_t)g_pan.generation << PAN_MSG_BITS;
    return rt_mb_send(g_pan.mailbox, value);
}

static rt_err_t btpan_schedule_apply(void)
{
    if (!g_pan.apply_pending)
    {
        rt_err_t result = btpan_post(PAN_MSG_APPLY_ENABLED);
        /* A full mailbox already guarantees a worker wakeup. Every received
         * message checks these flags before dispatching its own operation. */
        if (result != RT_EOK && result != -RT_EFULL) return result;
    }
    g_pan.apply_pending = RT_TRUE;
    return RT_EOK;
}

static void btpan_stop_timer(void)
{
    if (g_pan.pan_connect_timer != RT_NULL)
        rt_timer_stop(g_pan.pan_connect_timer);
}

/**
 * @brief PAN 连接延迟定时器回调：配对/加密完成后稍后发起 PAN 连接。
 */
static void btpan_connect_timeout(void *parameter)
{
    (void)parameter;

    btpan_request_connect();
}

static void btpan_start_timer(void)
{
    if (!btpan_pan_connect_allowed()) return;
    if (g_pan.pan_connect_timer == RT_NULL)
    {
        g_pan.pan_connect_timer = rt_timer_create("connect_pan",
                                                  btpan_connect_timeout,
                                                  RT_NULL,
                                                  rt_tick_from_millisecond(PAN_TIMER_MS),
                                                  RT_TIMER_FLAG_SOFT_TIMER);
    }
    else
    {
        rt_timer_stop(g_pan.pan_connect_timer);
    }

    if (g_pan.pan_connect_timer != RT_NULL)
        rt_timer_start(g_pan.pan_connect_timer);
}

static rt_bool_t btpan_sdk_pan_active(void)
{
    if (!g_pan.stack_ready) return RT_FALSE;
    bts2_pan_inst_data *pan = getApp()->pan_inst_ptr;
    if (!pan) return RT_FALSE;
    for (unsigned i = 0; i < PAN_MAX_NUM; ++i)
        if (pan->pan_info_list[i].is_use &&
            pan->pan_info_list[i].state != PAN_DEV_DISCONNECTED_ST)
            return RT_TRUE;
    return RT_FALSE;
}

static void btpan_resume_pan(void)
{
    btpan_sync_target();
    if (!btpan_pan_connect_allowed() || !g_pan.stack_ready) return;
    if (g_pan.bt_connected || g_pan.pan_connected)
    {
        g_pan.restore_pending = RT_FALSE;
        if (!g_pan.pan_connected) btpan_start_timer();
    }
    else if (g_pan.restore_pending)
    {
        bt_cm_dev_acl_info_t peer;
        /* An off/on may overtake completion of the old ACL disconnect. */
        if (btpan_has_peer_addr() && btpan_cm_peer(g_pan.bd_addr.addr, &peer) &&
            peer.state >= BT_CM_ACL_STATE_CONNECTED) return;
        g_pan.restore_pending = RT_FALSE;
        if (!btpan_has_peer_addr()) return;
        BTS2S_BD_ADDR address;
        bt_addr_convert_to_bts((bd_addr_t *)&g_pan.bd_addr, &address);
        bt_cm_err_t result = bt_cm_connect_req(&address, BT_LINK_PHONE);
        if (result != BT_CM_ERR_NO_ERR && result != BT_CM_ERR_CONN_EXISTED)
            LOG_W("phone restoration failed: result=%d", result);
    }
}

static void btpan_fail_pan_connection(void)
{
    if (g_pan.pan_cancelled || !g_pan.enabled || g_pan.stop_bluetooth || g_pan.manual_disconnect ||
        g_reset.status.paused || (g_suspend.held && g_suspend.started_work)) return;
    g_pan.pan_failed = RT_TRUE;
    g_pan.restore_pending = RT_FALSE;
    g_pan.pan_connected = RT_FALSE;
    btpan_stop_timer();
    btpan_reset_mailbox();
    btpan_sync_target();
    btpan_notify_state();
}

static void btpan_disconnect_pan(void)
{
    if (g_pan.stack_ready)
    {
        bts2_pan_inst_data *pan = getApp()->pan_inst_ptr;
        g_pan.pan_draining = btpan_sdk_pan_active();
        g_pan.pan_cancelled = g_pan.pan_draining;
        if (pan)
            for (unsigned i = 0; i < PAN_MAX_NUM; ++i)
            {
                bts2_pan_info_t *peer = &pan->pan_info_list[i];
                if (peer->is_use && peer->state == PAN_DEV_CONNECTED_ST)
                {
                    uint8_t mac[6];
                    bt_addr_convert(&peer->bd_addr, mac);
                    bt_interface_disc_ext(mac, BT_PROFILE_PAN);
                }
            }
    }
    /* SDK bt_pan_disc cannot cancel SDP/CONNECTING. Keep the drain gate
     * until its terminal event, including a Bluetooth off/on before that event. */
    g_pan.pan_connected = RT_FALSE;
}

/*---------------------------------------------------------------------------*/
/* 蓝牙事件回调 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 处理蓝牙协议栈上报的公共事件与 PAN profile 事件。
 */
static int btpan_bt_event_locked(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len)
{
    if (g_suspend.held && type == BT_NOTIFY_COMMON &&
        (event_id == BT_NOTIFY_COMMON_ACL_CONNECTED || event_id == BT_NOTIFY_COMMON_ACL_DISCONNECTED))
    {
        g_suspend.progress = true;
        g_suspend.scan_confirmed = false;
        g_suspend.scan_started = false;
        btpan_post(PAN_MSG_SUSPEND);
    }
    if (!btpan_earphone_allowed() && data && data_len >= sizeof(bt_notify_profile_state_info_t) &&
        ((type == BT_NOTIFY_A2DP && event_id == BT_NOTIFY_A2DP_PROFILE_CONNECTED) ||
         (type == BT_NOTIFY_AVRCP && event_id == BT_NOTIFY_AVRCP_PROFILE_CONNECTED) ||
         (type == BT_NOTIFY_HFP_AG && event_id == BT_NOTIFY_AG_PROFILE_CONNECTED)))
    {
        bt_notify_profile_state_info_t *info = (bt_notify_profile_state_info_t *)data;
        if (info->res == BTS2_SUCC) bt_interface_disconnect_req(info->mac.addr);
    }
    if (!btpan_bluetooth_allowed() && type == BT_NOTIFY_COMMON)
    {
        if (!g_suspend.held && event_id == BT_NOTIFY_COMMON_SCAN_ENB_CFM_IND &&
            data && data_len && data[0])
        {
            if (g_reset.status.paused)
            {
                g_reset.scan_pending = RT_TRUE;
                btpan_post(PAN_MSG_RESET);
            }
            else btpan_schedule_apply();
        }
        if (event_id == BT_NOTIFY_COMMON_ACL_CONNECT_IND && data &&
            data_len >= sizeof(bt_notify_device_mac_t))
        {
            BTS2S_BD_ADDR address;
            bt_addr_convert_to_bts((bd_addr_t *)data, &address);
            hcia_send_rej_conn_request(&address, HCI_ERR_REJ_BY_RMT_NO_RES);
            return 0;
        }
        if (event_id == BT_NOTIFY_COMMON_ENCRYPTION ||
            event_id == BT_NOTIFY_COMMON_PAIR_IND)
        {
            if (!g_suspend.held && data && data_len >= sizeof(bt_notify_device_mac_t))
                bt_interface_disconnect_req(((bt_notify_device_mac_t *)data)->addr);
            return 0;
        }
        if (event_id == BT_NOTIFY_COMMON_KEY_MISSING) return 0;
    }

    /* ---- 蓝牙公共事件 ---- */
    if (type == BT_NOTIFY_COMMON && event_id == BT_NOTIFY_COMMON_SCAN_ENB_CFM_IND &&
        !g_suspend.held && g_pan.apply_pending)
        btpan_post(PAN_MSG_APPLY_ENABLED);
    if (type == BT_NOTIFY_COMMON)
    {
        rt_bool_t should_connect_pan = RT_FALSE;

        switch (event_id)
        {
        case BT_NOTIFY_COMMON_BT_STACK_READY:
            if (g_suspend.held && g_pan.stack_ready)
            {
                /* A newly initialized stack invalidates old outstanding requests. */
                g_suspend.scan_started = g_suspend.scan_failed = false;
                g_suspend.scan_pending = false;
                g_suspend.scan_write_pending = false;
                g_suspend.scan_confirmed = false;
                memset(g_suspend.acl_requests, 0, sizeof(g_suspend.acl_requests));
                memset(g_suspend.pan_requests, 0, sizeof(g_suspend.pan_requests));
                g_suspend.result = -RT_EBUSY;
                g_suspend.started = rt_tick_get();
            }
            g_suspend.progress = true;
            g_pan.stack_ready_pending = RT_TRUE;
            btpan_post(PAN_MSG_STACK_READY);
            break;

        case BT_NOTIFY_COMMON_CLOSE_COMPLETE:
            /* Only explicit SDK close paths produce this event. Sleep does not
             * close GAP; remember completion before allowing an explicit reopen. */
            g_suspend.controller_closed = true;
            if (g_suspend.held)
            {
                g_suspend.progress = true;
                btpan_post(PAN_MSG_SUSPEND);
            }
            else if (!g_suspend.held)
            {
                /* CM may close scans before delivering an old completion
                 * after rollback. Reapply the current user policy after it. */
                g_pan.apply_pending = RT_TRUE;
                btpan_post(PAN_MSG_APPLY_ENABLED);
            }
            break;

        case BT_NOTIFY_COMMON_ACL_CONNECTED:
        {
            bt_notify_device_acl_conn_info_t *info = (bt_notify_device_acl_conn_info_t *)data;
            if (!info || data_len < sizeof(*info)) break;
            btpan_device_t *device = btpan_device(info->mac.addr, true);
            if (device)
            {
                if (info->dev_cls) device->device_class = info->dev_cls;
                device->earphone = btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE;
            }
            /* Reject a connection that completes after the switch was turned off. */
            if (!g_suspend.held && info->res == BTS2_SUCC && (!btpan_bluetooth_allowed() ||
                (btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE && !btpan_earphone_allowed())))
                bt_interface_disconnect_req(info->mac.addr);
            else if (info->res == BTS2_SUCC && btpan_bluetooth_allowed() &&
                     btpan_link_type(info->mac.addr) == BT_LINK_PHONE &&
                     (!g_pan.bt_connected || !memcmp(g_pan.bd_addr.addr, info->mac.addr, 6)))
            {
                if (!g_pan.bt_connected)
                {
                    g_pan.pan_failed = RT_FALSE;
                    if (info->acl_dir) g_pan.manual_disconnect = RT_FALSE;
                }
                g_pan.bd_addr = info->mac;
                g_pan.bt_connected = RT_TRUE;
                g_pan.restore_pending = RT_FALSE;
                btpan_sync_target();
                if (g_devices.snapshot.busy && g_devices.snapshot.operation == BTPAN_OP_CONNECT &&
                    !memcmp(g_devices.target, info->mac.addr, 6))
                    btpan_devices_finish(RT_EOK);
                btpan_notify_state();
            }
            if (info->res == BTS2_SUCC && device && (!device->name || !device->name[0]) &&
                btpan_bluetooth_allowed() && (!device->earphone || btpan_earphone_allowed()))
            {
                BTS2S_BD_ADDR address;
                bd_addr_t mac;
                memcpy(mac.addr, info->mac.addr, sizeof(mac.addr));
                bt_addr_convert_to_bts(&mac, &address);
                gap_rd_rmt_name_req(bts2_task_get_app_task_id(), address);
            }
            break;
        }

        case BT_NOTIFY_COMMON_ACL_DISCONNECTED:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;
            if (!info || data_len < sizeof(*info)) break;

            if (!g_suspend.held && !btpan_bluetooth_allowed()) btpan_schedule_apply();
            /* CM still owns the disconnecting record during this callback. */
            if (memcmp(g_pan.bd_addr.addr, info->mac.addr, 6)) break;

            /* Remote termination also cancels restoration across sleep. */
            if (info->res == HCI_ERR_OETC_USER || info->res == HCI_ERR_OETC_LOW_RESRC ||
                info->res == HCI_ERR_OETC_POWERING_OFF)
            {
                g_pan.manual_disconnect = RT_TRUE;
                g_pan.restore_pending = RT_FALSE;
            }
            if (info->res != HCI_SUCC && info->res != HCI_ERR_OETC_USER &&
                info->res != HCI_ERR_CONN_TERM_LOCAL_HOST)
                LOG_W("pan ACL disconnected: reason=%d", info->res);
            g_pan.bt_connected = RT_FALSE;
            g_pan.pan_connected = RT_FALSE;
            btpan_reset_mailbox();
            btpan_stop_timer();
            g_pan.pan_draining = btpan_sdk_pan_active();
            btpan_sync_target();

            /* SDK CM owns timeout reconnection; stay available for the phone. */
            if (btpan_bluetooth_allowed() && g_pan.stack_ready && btpan_has_peer_addr())
            {
                btpan_scan_mode(3);
                if (g_pan.restore_pending) btpan_schedule_apply();
            }
            else if (!g_suspend.held && !btpan_bluetooth_allowed())
            {
                /* The connection manager may reopen scan after this callback. */
                btpan_schedule_apply();
            }

            btpan_notify_state();
            break;
        }

        case BT_NOTIFY_COMMON_ENCRYPTION:
        {
            bt_notify_device_mac_t *mac = (bt_notify_device_mac_t *)data;
            if (!mac || data_len < sizeof(*mac)) break;
            if (btpan_link_type(mac->addr) == BT_LINK_EARPHONE)
            {
                if (!btpan_earphone_allowed()) bt_interface_disconnect_req(mac->addr);
                break;
            }
            if (g_pan.bt_connected && memcmp(g_pan.bd_addr.addr, mac->addr, 6)) break;

            g_pan.bd_addr = *mac;
            should_connect_pan = RT_TRUE;
            break;
        }

        case BT_NOTIFY_COMMON_PAIR_IND:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;
            if (!info || data_len < sizeof(*info)) break;
            if (btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE)
            {
                if (info->res == BTS2_SUCC && !btpan_earphone_allowed())
                    bt_interface_disconnect_req(info->mac.addr);
                break;
            }
            if (g_pan.bt_connected && memcmp(g_pan.bd_addr.addr, info->mac.addr, 6)) break;

            if (info->res == BTS2_SUCC)
            {
                g_pan.bd_addr = info->mac;
                should_connect_pan = RT_TRUE;
            }
            else LOG_W("pan pairing failed: result=%d", info->res);
            break;
        }

        case BT_NOTIFY_COMMON_KEY_MISSING:
        {
            bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;
            if (!info || data_len < sizeof(*info)) break;

            LOG_W("pan key missing: result=%d", info->res);
            if (!memcmp(g_pan.bd_addr.addr, info->mac.addr, 6))
                memset(&g_pan.bd_addr, 0xFF, sizeof(g_pan.bd_addr));
            bt_cm_delete_bonded_devs_and_linkkey(info->mac.addr);
            break;
        }

        default:
            break;
        }

        /* 配对或加密完成后，启动延迟定时器发起 PAN 连接。 */
        if (should_connect_pan)
        {
            if (!btpan_bluetooth_allowed())
            {
                bt_interface_disconnect_req(g_pan.bd_addr.addr);
                return 0;
            }
            g_pan.bt_connected = RT_TRUE;
            g_pan.restore_pending = RT_FALSE;
            /* Phone connection completes independently of network sharing. */
            if (g_devices.snapshot.busy && g_devices.snapshot.operation == BTPAN_OP_CONNECT &&
                !memcmp(g_devices.target, g_pan.bd_addr.addr, sizeof(g_devices.target)))
                btpan_devices_finish(RT_EOK);
            btpan_notify_state();
            btpan_start_timer();
        }
    }
    /* ---- PAN profile 事件 ---- */
    else if (type == BT_NOTIFY_PAN)
    {
        bt_notify_profile_state_info_t *info = (bt_notify_profile_state_info_t *)data;
        if (!info || data_len < sizeof(*info)) return 0;
        int res = (info != RT_NULL) ? info->res : -1;

        switch (event_id)
        {
        case BT_NOTIFY_PAN_PROFILE_CONNECTED:
            if (res != BTS2_SUCC)
            {
                if (!memcmp(g_pan.bd_addr.addr, info->mac.addr, 6))
                    btpan_fail_pan_connection();
                break;
            }
            if (btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE ||
                (g_pan.bt_connected && memcmp(g_pan.bd_addr.addr, info->mac.addr, 6)))
            {
                bt_interface_disc_ext(info->mac.addr, BT_PROFILE_PAN);
                break;
            }
            if (!btpan_pan_allowed())
            {
                g_pan.pan_draining = RT_TRUE;
                g_pan.pan_cancelled = RT_TRUE;
                g_pan.pan_connected = RT_FALSE;
                btpan_sync_target();
                bt_interface_disc_ext(info->mac.addr, BT_PROFILE_PAN);
                if (!btpan_bluetooth_allowed())
                    bt_interface_disconnect_req(info->mac.addr);
                btpan_notify_state();
                break;
            }
            btpan_stop_timer();
            g_pan.connect_pending = RT_FALSE;
            g_pan.bd_addr = info->mac;
            g_pan.bt_connected = RT_TRUE;
            g_pan.pan_connected = RT_TRUE;
            g_pan.restore_pending = RT_FALSE;
            btpan_sync_target();
            btpan_notify_state();
            break;

        case BT_NOTIFY_PAN_PROFILE_DISCONNECTED:
        {
            if (memcmp(g_pan.bd_addr.addr, info->mac.addr, 6) && !g_pan.pan_draining) break;
            rt_bool_t resume = g_pan.pan_draining;
            if (res != BTS2_SUCC)
            {
                LOG_W("pan %s: result=%d", g_pan.pan_connected ? "disconnected" : "connection failed", res);
                if (!memcmp(g_pan.bd_addr.addr, info->mac.addr, 6)) btpan_fail_pan_connection();
            }
            g_pan.pan_connected = RT_FALSE;
            btpan_reset_mailbox();
            g_pan.pan_draining = btpan_sdk_pan_active();
            if (!g_pan.pan_draining) g_pan.pan_cancelled = RT_FALSE;
            if (resume) btpan_resume_pan();
            else btpan_sync_target();
            btpan_notify_state();
            break;
        }

        default:
            break;
        }
    }

    return 0;
}

static int btpan_bt_event_handle(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len)
{
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    int result = btpan_bt_event_locked(type, event_id, data, data_len);
    bt_audio_handle_event(type, event_id, data, data_len);
    btpan_devices_event(type, event_id, data, data_len);
    if (g_suspend.held && (type == BT_NOTIFY_PAN ||
        (type == BT_NOTIFY_COMMON && event_id == BT_NOTIFY_COMMON_INQUIRY_CMP)))
    {
        g_suspend.progress = true;
        btpan_post(PAN_MSG_SUSPEND);
    }
    rt_mutex_release(&g_pan_lock);
    return result;
}

/*---------------------------------------------------------------------------*/
/* 本机蓝牙 MAC 地址（NVDS 持久化，重启后生效）
 *
 * 开机协议栈就绪后将下面这组自定义地址写入 NVDS；
 * 写入后需【重启设备】才会生效（协议栈启动时才重新读取）。
 *---------------------------------------------------------------------------*/
/* 自定义本机蓝牙 MAC（直接修改数组即可；显示顺序：下标 0 为显示首字节，
   即 02:4C:56:45:50:44） */
static const uint8_t s_custom_bd_addr[6] = { 0x02, 0x42, 0x56, 0x45, 0x50, 0x44 };

/* NVDS 地址写入接口（声明见 bf0_sibles_nvds.h；实现位于 service/common/bf0_bt_nvds.c） */
extern uint8_t ble_nvds_update_address(bd_addr_t *addr, ble_common_update_type_t u_type, uint8_t is_flush);

void btpan_get_local_addr(char *buf, rt_size_t len)
{
    bd_addr_t addr;

    if (buf == RT_NULL || len < 18)
        return;

    buf[0] = '\0';

    if (ble_get_public_address(&addr) != 0)
        return;

    rt_snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
                addr.addr[0], addr.addr[1], addr.addr[2],
                addr.addr[3], addr.addr[4], addr.addr[5]);
}

/*---------------------------------------------------------------------------*/
/* 工作线程 */
/*---------------------------------------------------------------------------*/
static void btpan_connect_profile(void)
{
    if (!btpan_pan_connect_allowed() || !g_pan.stack_ready || !g_pan.bt_connected || g_pan.pan_connected) return;
    BTS2S_BD_ADDR address;
    bt_addr_convert_to_bts((bd_addr_t *)&g_pan.bd_addr, &address);
    bts2_pan_inst_data *pan = getApp()->pan_inst_ptr;
    for (unsigned i = 0; i < PAN_MAX_NUM; ++i)
    {
        bts2_pan_info_t *peer = &pan->pan_info_list[i];
        if (peer->is_use && bd_eq(&peer->bd_addr, &address) &&
            peer->state != PAN_DEV_DISCONNECTED_ST) return;
    }
    /* Incoming ACL links may have no profile event to start the PAN target. */
    bt_cm_err_t result = bt_cm_profile_connect(BT_CM_PAN, &address, BT_LINK_PHONE);
    if (result != BT_CM_ERR_NO_ERR)
    {
        LOG_W("pan connection request failed: result=%d", result);
        btpan_fail_pan_connection();
    }
}

/* Coalesce policy changes while the SDK write/read sequence is outstanding.
 * In particular, a no-op must not put the SDK's scan FSM into SETTING. */
static void btpan_scan_mode(uint8_t mode)
{
    if (bt_interface_get_scan_mode_fsm())
    {
        g_pan.apply_pending = RT_TRUE;
        return;
    }
    if (bt_interface_get_current_scan_mode() != mode)
        bt_interface_set_scan_mode(mode & 1, (mode >> 1) & 1);
}

static void btpan_apply_enabled(void)
{
    if (g_suspend.held) return;
    if (g_pan.stack_ready && !(g_suspend.controller_closed && btpan_bluetooth_allowed()))
        btpan_scan_mode(g_pan.enabled && !g_reset.status.paused ? 3 : 0);

    if (g_reset.status.paused || !g_pan.enabled || g_pan.stop_bluetooth)
    {
        btpan_stop_timer();
        btpan_reset_mailbox();
        btpan_stop_scan();
        if (g_devices.snapshot.busy) btpan_devices_finish(-RT_ERROR);
        if (g_pan.stack_ready)
        {
            bt_cm_device_manager_t *cm = bt_cm_get_env();
            for (unsigned i = 0; i < BT_CM_DEVICE_MAX_CONN; ++i)
            {
                rt_base_t level = rt_hw_interrupt_disable();
                bt_cm_dev_acl_info_t peer = cm->bt_devices[i];
                rt_hw_interrupt_enable(level);
                if (peer.is_use && peer.info.link_type == BT_LINK_EARPHONE)
                {
                    uint8_t mac[6];
                    bt_addr_convert(&peer.info.bd_addr, mac);
                    g_pan.earphone_draining = RT_TRUE;
                    bt_interface_disconnect_req(mac);
                }
            }
        }
        if (g_pan.stack_ready && btpan_has_peer_addr() &&
            (g_pan.bt_connected || g_pan.pan_connected))
        {
            bt_interface_disconnect_req(g_pan.bd_addr.addr);
        }
        btpan_disconnect_pan();
        g_pan.bt_connected = RT_FALSE;
        g_pan.pan_connected = RT_FALSE;
        g_pan.stop_bluetooth = RT_FALSE;
    }
    if (g_suspend.controller_closed && g_pan.stack_ready && btpan_bluetooth_allowed())
    {
        g_suspend.controller_closed = false;
        bt_interface_open_bt();
    }
    btpan_resume_pan();
    btpan_notify_state();
}

static void btpan_stack_ready(void)
{
    bd_addr_t addr;

    g_pan.stack_ready = RT_TRUE;

    /* 写入自定义本机蓝牙 MAC（NVDS 持久化，重启后生效） */
    if (!g_reset.status.paused && !g_suspend.held)
    {
        memcpy(addr.addr, s_custom_bd_addr, sizeof(s_custom_bd_addr));
        if (ble_nvds_update_address(&addr, BLE_UPDATE_ALWAYS, 1) != 0)
            LOG_E("set custom bd addr failed");
    }

    if (g_pan.local_name[0] != '\0') g_devices.name_pending = true;

    btpan_apply_enabled();
}

static void btpan_process_message(rt_uint32_t value)
{
    if (g_pan.earphone_draining && !btpan_earphone_active())
    {
        g_pan.earphone_draining = RT_FALSE;
        btpan_sync_target();
    }
    /* A full mailbox still contains work that will wake the worker. */
    if (g_pan.stack_ready_pending)
    {
        g_pan.stack_ready_pending = RT_FALSE;
        if (!g_pan.stack_ready)
        {
            btpan_stack_ready();
            g_pan.apply_pending = RT_FALSE;
        }
    }
    if (g_pan.apply_pending && !g_suspend.held)
    {
        g_pan.apply_pending = RT_FALSE;
        btpan_apply_enabled();
    }
    btpan_suspend_step();
    if (g_suspend.held) return;
    network_reset_step();
    btpan_devices_step();
    pan_msg_t event = (pan_msg_t)(value & ((1u << PAN_MSG_BITS) - 1));
    if (event == PAN_MSG_CONNECT_PAN &&
        (value >> PAN_MSG_BITS) != g_pan.generation) return;

    switch (event)
    {
    case PAN_MSG_STACK_READY:
        break;
    case PAN_MSG_CONNECT_PAN:
        g_pan.connect_pending = RT_FALSE;
        btpan_connect_profile();
        break;
    case PAN_MSG_APPLY_ENABLED:
        break;
    default:
        break;
    }
}

static int32_t btpan_worker_wait(void)
{
    bool polling = g_suspend.held ?
        (g_suspend.result == -RT_EBUSY) :
        (g_reset.status.paused || g_devices.snapshot.busy || g_pan.earphone_draining);
    return polling ? (int32_t)rt_tick_from_millisecond(NETWORK_RESET_POLL_MS) : RT_WAITING_FOREVER;
}

static void btpan_worker_entry(void *parameter)
{
    rt_uint32_t value = 0;

    (void)parameter;

    /* Settings can arrive before stack-ready during boot. */
    while (1)
    {
        if (rt_mb_recv(g_pan.mailbox, &value, btpan_worker_wait()) != RT_EOK)
            value = 0;

        rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
        btpan_process_message(value);
        rt_mutex_release(&g_pan_lock);
    }
}

/*---------------------------------------------------------------------------*/
/* 对外接口 */
/*---------------------------------------------------------------------------*/
/**
 * @brief 返回设备蓝牙 Class of Device，用于声明网络设备能力。
 */
uint32_t bt_get_class_of_device(void)
{
    return (uint32_t)BT_SRVCLS_NETWORK | BT_COMPCLS_PALMSIZED;
}

rt_err_t btpan_init(const char *device_name)
{
    rt_base_t init_level = rt_hw_interrupt_disable();
    if (g_pan.initialized)
    {
        rt_hw_interrupt_enable(init_level);
        return RT_EOK;
    }
    if (g_suspend.held || g_suspend.initializing)
    {
        rt_hw_interrupt_enable(init_level);
        return -RT_EBUSY;
    }
    g_suspend.initializing = true;
    rt_hw_interrupt_enable(init_level);

    memset(&g_pan, 0, sizeof(g_pan));
    memset(&g_devices, 0, sizeof(g_devices));

    if (device_name == RT_NULL || device_name[0] == '\0')
        device_name = "sifli_pan";

    rt_strncpy(g_pan.local_name, device_name, sizeof(g_pan.local_name) - 1);
    g_pan.enabled = RT_TRUE;
    g_pan.last_state = BTPAN_STATE_OFF;

#ifdef BSP_BT_CONNECTION_MANAGER
    btpan_sync_target();
#endif

    g_pan.mailbox = rt_mb_create("bt_app", 4, RT_IPC_FLAG_FIFO);
    if (g_pan.mailbox == RT_NULL)
    {
        g_suspend.initializing = false;
        return -RT_ENOMEM;
    }

    rt_err_t result = rt_mutex_init(&g_pan_lock, "pan_ctl", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mb_delete(g_pan.mailbox);
        g_pan.mailbox = RT_NULL;
        g_suspend.initializing = false;
        return result;
    }

    g_pan.worker = epd_thread_create_psram("pan_worker",
                                    btpan_worker_entry,
                                    RT_NULL,
                                    PAN_THREAD_STACK_SIZE,
                                    PAN_THREAD_PRIORITY,
                                    PAN_THREAD_TICK);
    if (g_pan.worker == RT_NULL)
    {
        rt_mb_delete(g_pan.mailbox);
        g_pan.mailbox = RT_NULL;
        rt_mutex_detach(&g_pan_lock);
        g_suspend.initializing = false;
        return -RT_ENOMEM;
    }

    g_pan.initialized = RT_TRUE;

    bt_interface_register_bt_event_notify_callback(btpan_bt_event_handle);
    rt_thread_startup(g_pan.worker);

    btpan_notify_state(); /* OFF -> INITIALIZING */
    sifli_ble_enable();
    g_suspend.initializing = false;

    return RT_EOK;
}

rt_err_t btpan_enable(bool enable)
{
    if (!g_pan.initialized)
        return -RT_ERROR;

    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    if (!!enable == !!g_pan.enabled)
    {
        rt_mutex_release(&g_pan_lock);
        return RT_EOK;
    }
    rt_bool_t previous = g_pan.enabled;
    rt_base_t level = rt_hw_interrupt_disable();
    g_pan.enabled = enable ? RT_TRUE : RT_FALSE;
    rt_err_t result = btpan_schedule_apply();
    if (result != RT_EOK) g_pan.enabled = previous;
    else
    {
        if (enable)
        {
            g_pan.manual_disconnect = RT_FALSE;
            g_pan.pan_failed = RT_FALSE;
            g_pan.restore_pending = RT_TRUE;
        }
        if (!enable)
        {
            g_pan.stop_bluetooth = RT_TRUE;
            g_pan.restore_pending = RT_FALSE;
            g_pan.earphone_draining = btpan_earphone_active();
            btpan_reset_mailbox();
        }
        /* Synchronous policy update closes the CM timer window before return.
         * Only physical disconnect/scan commands wait for the worker. */
        btpan_sync_target();
        g_pan.last_state = btpan_current_state();
    }
    rt_hw_interrupt_enable(level);
    if (result == RT_EOK)
    {
        if (!enable)
        {
            btpan_stop_timer();
        }
        network_changed();
    }
    rt_mutex_release(&g_pan_lock);
    return result;
}

static rt_err_t btpan_request_connect_locked(void)
{
    if (!g_pan.initialized || !btpan_pan_connect_allowed() || !g_pan.stack_ready || !g_pan.bt_connected)
        return -RT_ERROR;

    if (g_pan.pan_connected || g_pan.connect_pending)
        return RT_EOK;

    g_pan.connect_pending = RT_TRUE;
    if (btpan_post(PAN_MSG_CONNECT_PAN) != RT_EOK)
    {
        g_pan.connect_pending = RT_FALSE;
        return -RT_ERROR;
    }

    return RT_EOK;
}

rt_err_t btpan_request_connect(void)
{
    if (!g_pan.initialized) return -RT_ERROR;
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    rt_err_t result = btpan_request_connect_locked();
    rt_mutex_release(&g_pan_lock);
    return result;
}

btpan_state_t btpan_get_state(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    btpan_state_t state = btpan_current_state();
    rt_hw_interrupt_enable(level);
    return state;
}

bool btpan_is_ready(void)
{
    return g_pan.stack_ready && !g_suspend.held;
}

bool btpan_is_connected(void)
{
    btpan_state_t state = btpan_get_state();
    return state == BTPAN_STATE_CONNECTED || state == BTPAN_STATE_NETWORK_READY;
}

bool btpan_is_network_ready(void)
{
    return btpan_get_state() == BTPAN_STATE_NETWORK_READY;
}

static btpan_device_t *btpan_device(const uint8_t address[6], bool add)
{
    for (unsigned i = 0; i < g_devices.snapshot.count; ++i)
        if (!memcmp(address, g_devices.snapshot.devices[i].address, 6))
            return &g_devices.snapshot.devices[i];
    if (!add) return NULL;
    btpan_device_t *devices = rt_realloc(g_devices.snapshot.devices,
        ((size_t)g_devices.snapshot.count + 1) * sizeof(*devices));
    if (!devices)
    {
        g_devices.snapshot.truncated = true;
        g_devices.snapshot.error = -RT_ENOMEM;
        return NULL;
    }
    g_devices.snapshot.devices = devices;
    btpan_device_t *device = &g_devices.snapshot.devices[g_devices.snapshot.count++];
    memset(device, 0, sizeof(*device));
    memcpy(device->address, address, 6);
    return device;
}

static bool btpan_device_name_update(btpan_device_t *device, const char *name, size_t limit)
{
    size_t length = 0;
    while (length < limit && name[length]) ++length;
    if (!length || (device->name && strlen(device->name) == length &&
                    !memcmp(device->name, name, length))) return false;
    char *copy = rt_malloc(length + 1);
    if (!copy)
    {
        g_devices.snapshot.error = -RT_ENOMEM;
        g_devices.snapshot.truncated = true;
        return false;
    }
    memcpy(copy, name, length);
    copy[length] = 0;
    rt_free(device->name);
    device->name = copy;
    return true;
}

static void btpan_devices_refresh(void)
{
    for (unsigned i = 0; i < g_devices.snapshot.count; ++i)
    {
        btpan_device_t *device = &g_devices.snapshot.devices[i];
        device->paired = false;
        device->connected = btpan_bluetooth_allowed() && g_pan.bt_connected &&
                            !memcmp(device->address, g_pan.bd_addr.addr, 6);
        device->pan_connected = device->connected && g_pan.pan_connected;
    }
#ifdef BSP_BT_CONNECTION_MANAGER
    bt_cm_bonded_dev_t bonds;
    rt_base_t level = rt_hw_interrupt_disable();
    bonds = *bt_cm_get_bonded_dev();
    rt_hw_interrupt_enable(level);
    for (unsigned i = 0; i < BT_CM_MAX_BOND; ++i)
    {
        if (!bonds.info[i].is_use) continue;
        uint8_t address[6];
        bt_addr_convert(&bonds.info[i].bd_addr, address);
        btpan_device_t *device = btpan_device(address, true);
        if (device)
        {
            device->paired = true;
            device->earphone = btpan_link_type(address) == BT_LINK_EARPHONE;
        }
    }
    bt_cm_device_manager_t *cm = bt_cm_get_env();
    for (unsigned i = 0; i < BT_CM_DEVICE_MAX_CONN; ++i)
    {
        level = rt_hw_interrupt_disable();
        bt_cm_dev_acl_info_t peer = cm->bt_devices[i];
        rt_hw_interrupt_enable(level);
        if (!peer.is_use) continue;
        uint8_t address[6];
        bt_addr_convert(&peer.info.bd_addr, address);
        btpan_device_t *device = btpan_device(address, true);
        if (!device) continue;
        device->earphone = peer.info.link_type == BT_LINK_EARPHONE;
        device->connected = btpan_bluetooth_allowed() && peer.state == BT_CM_ACL_STATE_CONNECTED;
        device->pan_connected = device->connected && !device->earphone &&
            g_pan.pan_connected && !memcmp(address, g_pan.bd_addr.addr, 6);
    }
#endif
    if (btpan_bluetooth_allowed() && g_pan.bt_connected)
    {
        btpan_device_t *device = btpan_device(g_pan.bd_addr.addr, true);
        if (device) { device->connected = true; device->pan_connected = g_pan.pan_connected; }
    }
    ++g_devices.snapshot.revision;
}

static void btpan_devices_finish(rt_err_t error)
{
    g_devices.snapshot.busy = false;
    g_devices.snapshot.error = error;
    g_devices.command_pending = false;
    g_devices.unpair_wait = false;
    g_devices.unpair_verify = false;
    ++g_devices.snapshot.revision;
}

static void btpan_stop_scan(void)
{
    g_devices.cancel_scan = false;
    if (g_devices.snapshot.scanning && !g_devices.scan_stopping)
    {
        g_devices.scan_stopping = true;
        bt_interface_stop_inquiry();
    }
}

static void btpan_scan_finished(void)
{
    if (!g_devices.snapshot.scanning) return;
    g_devices.snapshot.scanning = false;
    g_devices.cancel_scan = g_devices.scan_stopping = false;
    g_devices.scan_barrier_pending = false;
    if (g_suspend.held)
    {
        g_suspend.progress = true;
        btpan_post(PAN_MSG_SUSPEND);
    }
    if (g_devices.snapshot.busy && g_devices.snapshot.operation == BTPAN_OP_SCAN)
        btpan_devices_finish(g_devices.snapshot.truncated ? -RT_ENOMEM : RT_EOK);
    else ++g_devices.snapshot.revision;
}

static void btpan_devices_step(void)
{
    if (g_suspend.held) return;
    if (g_devices.name_pending && g_pan.stack_ready)
    {
        bt_interface_set_local_name((int)strlen(g_pan.local_name), g_pan.local_name);
        g_devices.name_pending = false;
    }
    if (g_devices.refresh && g_pan.stack_ready)
    {
        g_devices.refresh = false;
        btpan_devices_refresh();
    }
    if (g_reset.status.paused || !g_pan.enabled)
    {
        btpan_stop_scan();
        if (g_devices.snapshot.busy) btpan_devices_finish(-RT_ERROR);
        return;
    }
    if (g_devices.cancel_scan) btpan_stop_scan();
    if (g_devices.snapshot.busy &&
        rt_tick_get() - g_devices.started >= rt_tick_from_millisecond(30000))
    {
        btpan_stop_scan();
        if (g_devices.snapshot.operation == BTPAN_OP_CONNECT)
        {
            if (btpan_link_type(g_devices.target) == BT_LINK_EARPHONE)
                g_pan.earphone_draining = btpan_earphone_active();
            else
            {
                g_pan.manual_disconnect = true;
                g_pan.restore_pending = RT_FALSE;
                btpan_reset_mailbox();
                btpan_stop_timer();
            }
            btpan_sync_target();
            bt_interface_disconnect_req(g_devices.target);
        }
        btpan_devices_finish(-RT_ETIMEOUT);
    }
    if (!g_devices.command_pending) return;
    g_devices.command_pending = false;
    BTS2S_BD_ADDR address;
    bt_addr_convert_to_bts((bd_addr_t *)g_devices.target, &address);
    bt_cm_link_type_t link_type = btpan_link_type(g_devices.target);
    switch (g_devices.snapshot.operation)
    {
    case BTPAN_OP_SCAN:
    {
        for (unsigned i = 0; i < g_devices.snapshot.count;)
        {
            btpan_device_t *device = &g_devices.snapshot.devices[i];
            if (!device->paired && !device->connected)
            {
                rt_free(device->name);
                --g_devices.snapshot.count;
                memmove(device, device + 1, (g_devices.snapshot.count - i) * sizeof(*device));
            }
            else { device->available = false; ++i; }
        }
        g_devices.snapshot.truncated = false;
        g_devices.snapshot.scanning = true;
        bt_start_inquiry_ex_t inquiry = {0};
        inquiry.max_timeout = 20;
        /* SDK: zero class and response limit discover all classes/counts. */
        if (bt_interface_start_inquiry_ex(&inquiry) != 0)
        { g_devices.snapshot.scanning = false; btpan_devices_finish(-RT_ERROR); }
        break;
    }
    case BTPAN_OP_CONNECT:
    {
        if (link_type == BT_LINK_PHONE)
        {
            g_pan.manual_disconnect = false;
            g_pan.restore_pending = RT_FALSE;
            bt_cm_dev_acl_info_t peer;
            if (!btpan_cm_peer(g_devices.target, &peer) && !g_pan.bt_connected)
                g_pan.pan_failed = false;
        }
        btpan_sync_target();
        bt_cm_err_t result = bt_cm_connect_req(&address, link_type);
        if (result == BT_CM_ERR_CONN_EXISTED)
        {
            if (link_type == BT_LINK_EARPHONE)
            {
                bt_cm_dev_acl_info_t peer;
                if (btpan_cm_peer(g_devices.target, &peer) && (peer.profiles_bit_mask & BT_CM_A2DP))
                    btpan_devices_finish(RT_EOK);
                /* CM owns any pending profile connection for this ACL. */
            }
            else if (!memcmp(g_pan.bd_addr.addr, g_devices.target, 6) && g_pan.bt_connected)
            {
                btpan_devices_finish(RT_EOK);
                btpan_connect_profile();
            }
            else btpan_devices_finish(-RT_EBUSY);
        }
        else if (result != BT_CM_ERR_NO_ERR) btpan_devices_finish(-RT_ERROR);
        break;
    }
    case BTPAN_OP_DISCONNECT:
    case BTPAN_OP_UNPAIR:
    {
        bt_cm_dev_acl_info_t peer;
        bool active = btpan_cm_peer(g_devices.target, &peer) ||
                      (!memcmp(g_pan.bd_addr.addr, g_devices.target, 6) && g_pan.bt_connected);
        if (link_type == BT_LINK_EARPHONE)
        {
            if (active) g_pan.earphone_draining = RT_TRUE;
        }
        else
        {
            g_pan.manual_disconnect = true;
            g_pan.restore_pending = RT_FALSE;
            g_pan.pan_cancelled = btpan_sdk_pan_active();
            btpan_reset_mailbox();
            btpan_stop_timer();
        }
        btpan_sync_target();
        if (active)
        {
            if (bt_interface_disconnect_req(g_devices.target) != 0)
                btpan_devices_finish(-RT_ERROR);
            break;
        }
        else if (g_devices.snapshot.operation == BTPAN_OP_DISCONNECT)
            btpan_devices_finish(RT_EOK);
        if (g_devices.snapshot.busy && g_devices.snapshot.operation == BTPAN_OP_UNPAIR)
        {
            g_devices.unpair_wait = true;
            bt_cm_delete_bonded_devs_and_linkkey(g_devices.target);
        }
        break;
    }
    default: break;
    }
}

static void btpan_devices_event(uint16_t type, uint16_t event, uint8_t *data, uint16_t size)
{
    if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_DISCOVER_IND)
    {
        if (!g_devices.snapshot.scanning || g_devices.cancel_scan || g_devices.scan_stopping ||
            !data || size < sizeof(bt_notify_remote_device_info_t)) return;
        bt_notify_remote_device_info_t *info = (bt_notify_remote_device_info_t *)data;
        btpan_device_t *device = btpan_device(info->mac.addr, true);
        if (device)
        {
            size_t length = info->name_size;
            if (length >= sizeof(info->bt_name)) length = sizeof(info->bt_name) - 1;
            btpan_device_name_update(device, info->bt_name, length);
            device->rssi = info->rssi;
            device->available = true;
            if (info->dev_cls) device->device_class = info->dev_cls;
            device->earphone = btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE;
        }
        ++g_devices.snapshot.revision;
        return;
    }
    if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_REMOTE_NAME_RSP)
    {
        if (!data || size < sizeof(bt_notify_rmt_name_t)) return;
        bt_notify_rmt_name_t *info = (bt_notify_rmt_name_t *)data;
        if (info->res != BTS2_SUCC) return;
        btpan_device_t *device = btpan_device(info->mac.addr, false);
        if (device && btpan_device_name_update(device, info->bt_name, sizeof(info->bt_name) - 1))
            ++g_devices.snapshot.revision;
        return;
    }
    if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_INQUIRY_CMP)
    {
        /* Complete in the raw GAP callback, after the SDK's own handler. */
        return;
    }
    if (type != BT_NOTIFY_COMMON && type != BT_NOTIFY_PAN &&
        !(type == BT_NOTIFY_A2DP && (event == BT_NOTIFY_A2DP_PROFILE_CONNECTED ||
                                     event == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED))) return;
    g_devices.refresh = true;
    btpan_post(PAN_MSG_DEVICES);
    if (!g_devices.snapshot.busy || !data || size < sizeof(bt_notify_device_base_info_t)) return;
    bt_notify_device_base_info_t *info = (bt_notify_device_base_info_t *)data;
    if (memcmp(g_devices.target, info->mac.addr, 6)) return;
    if (type == BT_NOTIFY_A2DP && event == BT_NOTIFY_A2DP_PROFILE_CONNECTED &&
        size >= sizeof(bt_notify_profile_state_info_t) &&
        ((bt_notify_profile_state_info_t *)data)->profile_role == AV_AUDIO_SRC &&
        g_devices.snapshot.operation == BTPAN_OP_CONNECT && btpan_link_type(info->mac.addr) == BT_LINK_EARPHONE)
        btpan_devices_finish(((bt_notify_profile_state_info_t *)data)->res == BTS2_SUCC &&
                             btpan_earphone_allowed() ? RT_EOK : -RT_ERROR);
    else if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_ACL_CONNECTED &&
             size >= sizeof(bt_notify_device_acl_conn_info_t) &&
             ((bt_notify_device_acl_conn_info_t *)data)->res != BTS2_SUCC &&
             g_devices.snapshot.operation == BTPAN_OP_CONNECT)
        btpan_devices_finish(-RT_ERROR);
    else if (type == BT_NOTIFY_PAN && event == BT_NOTIFY_PAN_PROFILE_CONNECTED &&
        size >= sizeof(bt_notify_profile_state_info_t) && g_devices.snapshot.operation == BTPAN_OP_CONNECT)
        btpan_devices_finish(((bt_notify_profile_state_info_t *)data)->res == BTS2_SUCC &&
            g_pan.pan_connected ? RT_EOK : -RT_ERROR);
    else if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_ACL_DISCONNECTED)
    {
        if (g_devices.snapshot.operation == BTPAN_OP_DISCONNECT) btpan_devices_finish(RT_EOK);
        else if (g_devices.snapshot.operation == BTPAN_OP_CONNECT) btpan_devices_finish(-RT_ERROR);
        else if (g_devices.snapshot.operation == BTPAN_OP_UNPAIR && !g_devices.unpair_wait && !g_devices.unpair_verify)
            g_devices.command_pending = true;
    }
    else if (type == BT_NOTIFY_COMMON && event == BT_NOTIFY_COMMON_PAIR_IND && info->res != BTS2_SUCC &&
             g_devices.snapshot.operation == BTPAN_OP_CONNECT)
        btpan_devices_finish(-RT_ERROR);
}

static void btpan_devices_query(uint16_t type, uint16_t event, uint8_t *data)
{
    if (type == BTS2M_GAP && event == BTS2MU_GAP_DISCOV_CFM && data)
    {
        uint8_t result = ((BTS2S_GAP_DISCOV_CFM *)data)->res;
        /* Rejected/duplicate requests do not end an existing inquiry. */
        if (result != BTS2_SUCC && result != BTS2_FAILED && result != BTS2_TIMEOUT) return;
        if (!g_devices.scan_stopping) btpan_scan_finished();
        else if (!g_devices.scan_barrier_pending)
        {
            /* Natural/timeout completion may replace or precede ESC. The
             * cached version reply traverses the same GAP/app FIFOs, after
             * the pending stop, so no old ESC can reach the next inquiry. */
            g_devices.scan_barrier_pending = true;
            gap_rd_local_version_req(bts2_task_get_app_task_id());
        }
        return;
    }
    if (type == BTS2M_GAP && event == BTS2MU_GAP_ESC_DISCOV_CFM && data)
    {
        /* SDK retries a failed cancel itself and emits no public completion. */
        if (g_devices.scan_stopping && !g_devices.scan_barrier_pending &&
            ((BTS2S_GAP_ESC_DISCOV_CFM *)data)->res == BTS2_SUCC)
            btpan_scan_finished();
        return;
    }
    if (type == BTS2M_GAP && event == BTS2MU_GAP_RD_LOCAL_VERSION_CFM && data)
    {
        /* This is a queue barrier; the cached reply's res is not initialized. */
        if (g_devices.scan_barrier_pending) btpan_scan_finished();
        return;
    }
    if (type != BTS2M_SC || !data || g_reset.status.paused || !g_devices.snapshot.busy) return;
    if (event == BTS2MU_SC_UNPAIR_CFM && g_devices.unpair_wait)
    {
        BTS2S_SC_UNPAIR_CFM *reply = (BTS2S_SC_UNPAIR_CFM *)data;
        uint8_t address[6];
        bt_addr_convert(&reply->bd, address);
        if (memcmp(address, g_devices.target, 6)) return;
        g_devices.unpair_wait = false;
        if (reply->res != BTS2_SUCC) { btpan_devices_finish(-RT_ERROR); return; }
        g_devices.unpair_verify = true;
        sc_rd_paired_dev_record_req(bts2_task_get_app_task_id());
    }
    else if (event == BTS2MU_SC_RD_PAIRED_DEV_RECORD_CFM && g_devices.unpair_verify)
    {
        BTS2S_SC_RD_PAIRED_DEV_RECORD_CFM *reply = (BTS2S_SC_RD_PAIRED_DEV_RECORD_CFM *)data;
        BTS2S_BD_ADDR address;
        bt_addr_convert_to_bts((bd_addr_t *)g_devices.target, &address);
        for (unsigned i = 0; i < reply->total_dev_num; ++i)
            if (bd_eq(&reply->bd[i], &address)) { btpan_devices_finish(-RT_ERROR); return; }
#ifdef BSP_BLE_NVDS_SYNC
        bt_cm_bonded_dev_t stored;
        uint16_t length = sizeof(stored);
        if (sifli_nvds_read(SIFLI_NVDS_TYPE_BT_CM, &length, (uint8_t *)&stored) != NVDS_OK ||
            length != sizeof(stored)) { btpan_devices_finish(-RT_ERROR); return; }
        for (unsigned i = 0; i < BT_CM_MAX_BOND; ++i)
            if (stored.info[i].is_use && bd_eq(&stored.info[i].bd_addr, &address))
            { btpan_devices_finish(-RT_ERROR); return; }
        btpan_devices_finish(RT_EOK);
#else
        btpan_devices_finish(-RT_ENOSYS);
#endif
        g_devices.refresh = true;
        btpan_post(PAN_MSG_DEVICES);
    }
}

rt_err_t btpan_snapshot(btpan_snapshot_t *out)
{
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!g_pan.initialized) return RT_EOK;
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    if (g_devices.refresh) btpan_post(PAN_MSG_DEVICES);
    *out = g_devices.snapshot;
    out->busy = out->busy || out->scanning;
    out->devices = NULL;
    size_t bytes = (size_t)out->count * sizeof(*out->devices);
    for (unsigned i = 0; i < out->count; ++i)
        bytes += g_devices.snapshot.devices[i].name ? strlen(g_devices.snapshot.devices[i].name) + 1 : 1;
    if (bytes)
    {
        out->devices = rt_malloc(bytes);
        if (!out->devices)
        {
            out->count = 0;
            rt_mutex_release(&g_pan_lock);
            return -RT_ENOMEM;
        }
        char *text = (char *)(out->devices + out->count);
        for (unsigned i = 0; i < out->count; ++i)
        {
            out->devices[i] = g_devices.snapshot.devices[i];
            if (!btpan_bluetooth_allowed())
                out->devices[i].connected = out->devices[i].pan_connected = false;
            out->devices[i].connecting = g_devices.snapshot.busy &&
                g_devices.snapshot.operation == BTPAN_OP_CONNECT &&
                !memcmp(out->devices[i].address, g_devices.target, 6);
            const char *name = out->devices[i].name ? out->devices[i].name : "";
            size_t length = strlen(name) + 1;
            memcpy(text, name, length);
            out->devices[i].name = text;
            text += length;
        }
    }
    out->supported = true;
    out->enabled = g_pan.enabled && !g_reset.status.paused;
    out->ready = g_pan.stack_ready && !g_suspend.held;
    rt_mutex_release(&g_pan_lock);
    return RT_EOK;
}

void btpan_snapshot_free(btpan_snapshot_t *snapshot)
{
    if (!snapshot) return;
    rt_free(snapshot->devices);
    memset(snapshot, 0, sizeof(*snapshot));
}

static rt_err_t btpan_device_request(btpan_operation_t operation, const uint8_t *address)
{
    if (!g_pan.initialized) return -RT_ERROR;
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    rt_err_t result = -RT_EBUSY;
    if (btpan_bluetooth_allowed() && g_pan.stack_ready &&
        !g_devices.snapshot.busy && !g_devices.snapshot.scanning && !g_pan.stop_bluetooth)
    {
        btpan_device_t *device = address ? btpan_device(address, false) : NULL;
        if (operation != BTPAN_OP_SCAN && (!device || (operation == BTPAN_OP_UNPAIR && !device->paired)))
            result = -RT_EINVAL;
        else if (operation == BTPAN_OP_CONNECT &&
                 (btpan_link_type(address) == BT_LINK_EARPHONE ? g_pan.earphone_draining :
                  (g_pan.pan_draining || (g_pan.bt_connected && memcmp(address, g_pan.bd_addr.addr, 6)))))
            result = -RT_EBUSY;
        else
        {
            result = btpan_post(PAN_MSG_DEVICES);
            if (result == -RT_EFULL) result = RT_EOK;
            if (result == RT_EOK)
            {
                g_devices.snapshot.operation = operation;
                g_devices.snapshot.busy = true;
                g_devices.snapshot.error = RT_EOK;
                ++g_devices.snapshot.revision;
                g_devices.command_pending = true;
                g_devices.started = rt_tick_get();
                if (address) memcpy(g_devices.target, address, 6);
            }
        }
    }
    rt_mutex_release(&g_pan_lock);
    return result;
}

rt_err_t btpan_scan(void) { return btpan_device_request(BTPAN_OP_SCAN, NULL); }
rt_err_t btpan_connect(const uint8_t address[6]) { return btpan_device_request(BTPAN_OP_CONNECT, address); }
rt_err_t btpan_disconnect(const uint8_t address[6]) { return btpan_device_request(BTPAN_OP_DISCONNECT, address); }
rt_err_t btpan_unpair(const uint8_t address[6]) { return btpan_device_request(BTPAN_OP_UNPAIR, address); }

rt_err_t btpan_cancel_scan(void)
{
    if (!g_pan.initialized) return -RT_ERROR;
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    rt_err_t result = RT_EOK;
    if (g_devices.snapshot.scanning && !g_devices.scan_stopping && !g_devices.cancel_scan)
    {
        result = btpan_post(PAN_MSG_DEVICES);
        if (result == -RT_EFULL) result = RT_EOK;
        if (result == RT_EOK) g_devices.cancel_scan = true;
    }
    else if (g_devices.snapshot.operation == BTPAN_OP_SCAN && g_devices.command_pending)
    {
        /* The queued scan has not reached the SDK yet. */
        btpan_devices_finish(RT_EOK);
    }
    rt_mutex_release(&g_pan_lock);
    return result;
}

rt_err_t btpan_set_local_name(const char *name)
{
    if (!name || !name[0]) return -RT_EINVAL;
    size_t length = 0;
    while (length <= BTPAN_LOCAL_NAME_MAX_BYTES && name[length]) ++length;
    if (length > BTPAN_LOCAL_NAME_MAX_BYTES) return -RT_EINVAL;
    /* Validate scalar UTF-8, rejecting controls, overlong encodings and surrogates. */
    for (size_t i = 0; i < length;)
    {
        unsigned char c = (unsigned char)name[i++];
        unsigned count;
        uint32_t cp, minimum;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return -RT_EINVAL; continue; }
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; minimum = 0x10000; }
        else return -RT_EINVAL;
        if (count > length - i) return -RT_EINVAL;
        while (count--) { c = (unsigned char)name[i++]; if ((c & 0xc0) != 0x80) return -RT_EINVAL; cp = (cp << 6) | (c & 63); }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -RT_EINVAL;
    }
    if (!g_pan.initialized) return -RT_ERROR;
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    rt_err_t result = btpan_post(PAN_MSG_DEVICES);
    if (result == -RT_EFULL) result = RT_EOK;
    if (result == RT_EOK && strcmp(g_pan.local_name, name))
    { memcpy(g_pan.local_name, name, length + 1); g_devices.name_pending = true; }
    rt_mutex_release(&g_pan_lock);
    return result;
}

/* SDK weak policy hooks run in the Bluetooth task, including before READY. */
uint8_t bt_is_auto_request_connect(void)
{
    return !g_suspend.held && !g_reset.status.paused &&
           (!g_pan.initialized || btpan_bluetooth_allowed());
}

void bt_sc_io_capability_rsp(BTS2S_BD_ADDR *address)
{
    bt_io_capability_rsp(address, !bt_is_auto_request_connect() ? IO_CAPABILITY_REJECT_REQ :
                         IO_CAPABILITY_NO_INPUT_NO_OUTPUT, FALSE, TRUE);
}

void bt_pincode_indi(bts2_app_stru *app)
{
    if (!bt_is_auto_request_connect() || !app->pin_code_len)
        sc_passkey_rsp(FALSE, &app->pair_bd, 0, app->input_str, TRUE, TRUE);
    else
        sc_passkey_rsp(TRUE, &app->pair_bd, app->pin_code_len, app->pin_code, TRUE, TRUE);
}

static rt_bool_t network_reset_running(void)
{
    return g_reset.status.state >= NETWORK_RESET_WAIT_STACK &&
           g_reset.status.state <= NETWORK_RESET_CLEARING;
}

/* Before PAN initialization only the boot thread may use the reset API. */
static rt_base_t network_reset_lock(void)
{
    if (!g_pan.initialized) return rt_hw_interrupt_disable();
    rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
    return -1;
}

static void network_reset_unlock(rt_base_t level)
{
    if (level == -1) rt_mutex_release(&g_pan_lock);
    else rt_hw_interrupt_enable(level);
}

static bool btpan_ble_activity_active(void)
{
    if (g_suspend.ble_scanning || g_suspend.ble_initiating) return true;
    for (unsigned i = 0; i < sizeof(g_suspend.ble_activities); ++i)
        if (g_suspend.ble_activities[i]) return true;
    return false;
}

static bool btpan_ble_links_active(void)
{
#ifdef BSP_BLE_CONNECTION_MANAGER
    for (unsigned index = 0; index < CM_CONN_INDEX_EMPTY; ++index)
    {
        uint8_t state = connection_manager_get_connection_state((uint8_t)index);
        if (state != CM_CONN_INDEX_ERROR && state != CONNECTION_STATE_DISCONNECTED) return true;
    }
#endif
    return false;
}

/* Connecting records count too: close_bt's no-connected-ACL fast path clears
 * CM state before a pending page/SDP operation has actually finished. */
static bool btpan_suspend_links(bool disconnect)
{
    bool active = false;
    bt_cm_device_manager_t *cm = bt_cm_get_env();
    for (unsigned i = 0; i < BT_CM_DEVICE_MAX_CONN; ++i)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        bt_cm_dev_acl_info_t peer = cm->bt_devices[i];
        rt_hw_interrupt_enable(level);
        btpan_drain_request_t *request = &g_suspend.acl_requests[i];
        if (!peer.is_use || peer.state == BT_CM_ACL_STATE_DISCONNECTED)
        {
            if (disconnect) request->phase = 0;
            continue;
        }
        active = true;
        if (!disconnect || peer.state == BT_CM_ACL_STATE_DISCONNECTING) continue;
        unsigned phase = (unsigned)peer.state + 1;
        if (request->phase == phase && bd_eq(&request->address, &peer.info.bd_addr)) continue;
        request->address = peer.info.bd_addr;
        request->phase = phase;
        int result;
        if (peer.state == BT_CM_ACL_STATE_CONNECTING)
            result = gap_cancel_connect_req(&peer.info.bd_addr);
        else
        {
            uint8_t mac[6];
            bt_addr_convert(&peer.info.bd_addr, mac);
            result = bt_interface_disconnect_req(mac);
        }
        if (result != 0) g_suspend.result = -RT_ERROR;
    }
    if (btpan_sdk_pan_active())
    {
        active = true;
        if (disconnect)
        {
            g_pan.pan_draining = RT_TRUE;
            g_pan.pan_cancelled = RT_TRUE;
            g_pan.pan_connected = RT_FALSE;
            bts2_pan_inst_data *pan = getApp()->pan_inst_ptr;
            for (unsigned i = 0; i < PAN_MAX_NUM; ++i)
            {
                rt_base_t level = rt_hw_interrupt_disable();
                bts2_pan_info_t peer = pan->pan_info_list[i];
                rt_hw_interrupt_enable(level);
                btpan_drain_request_t *request = &g_suspend.pan_requests[i];
                if (!peer.is_use || peer.state != PAN_DEV_CONNECTED_ST)
                {
                    request->phase = 0;
                    continue;
                }
                unsigned phase = (unsigned)peer.id + 1;
                if (request->phase == phase && bd_eq(&request->address, &peer.bd_addr)) continue;
                request->address = peer.bd_addr;
                request->phase = phase;
                uint8_t mac[6];
                bt_addr_convert(&peer.bd_addr, mac);
                if (bt_interface_disc_ext(mac, BT_PROFILE_PAN) != 0) g_suspend.result = -RT_ERROR;
            }
        }
    }
#ifdef BSP_BLE_CONNECTION_MANAGER
    if (!g_suspend.resume_pending && !g_reset.ble_ready) return true;
#endif
    return active || (!g_suspend.resume_pending && btpan_ble_links_active());
}

static void btpan_suspend_restore(rt_err_t result)
{
    g_suspend.held = false;
    g_suspend.resume_pending = false;
    g_suspend.resume_posted = false;
    g_suspend.restoring = false;
    g_suspend.result = result;
    g_suspend.scan_confirmed = false;
    if (g_suspend.started_work)
    {
        g_pan.pan_draining = btpan_sdk_pan_active();
        g_pan.earphone_draining = btpan_earphone_active();
    }
    btpan_sync_target();
    /* A suspended phone gets one restoration request. */
    if (result == RT_EOK)
    {
        btpan_resume_pan();
        if (g_suspend.controller_closed) g_pan.apply_pending = RT_TRUE;
        if (g_pan.apply_pending) btpan_post(PAN_MSG_APPLY_ENABLED);
    }
    else
    {
        g_pan.restore_pending = RT_FALSE;
        g_pan.apply_pending = RT_FALSE;
    }
}

static void btpan_suspend_step(void)
{
    if (!g_suspend.held) return;
    if (g_suspend.result == -RT_EBUSY &&
        rt_tick_get() - g_suspend.started >= rt_tick_from_millisecond(BTPAN_SUSPEND_TIMEOUT_MS))
        g_suspend.result = -RT_ETIMEOUT;
    if (g_suspend.resume_pending && !g_suspend.started_work)
    {
        btpan_suspend_restore(RT_EOK);
        return;
    }
    if (g_suspend.result != -RT_EBUSY && g_suspend.result != RT_EOK && !g_suspend.progress) return;
    g_suspend.progress = false;
    if (!g_pan.stack_ready) return;
    if (!g_suspend.restoring)
    {
        if (!g_suspend.resume_pending &&
            (bt_audio_is_active() || btpan_ble_activity_active() || btpan_ble_links_active()))
        {
            g_suspend.result = -RT_ERROR;
            return;
        }
        g_suspend.started_work = true;
        /* Inquiry/ACL cleanup is independent of an outstanding scan write. */
        btpan_stop_scan();
        if (g_devices.snapshot.busy) btpan_devices_finish(-RT_ERROR);
        bool links = btpan_suspend_links(true);
        if (links || g_devices.snapshot.scanning || g_devices.scan_barrier_pending)
        {
            g_suspend.scan_confirmed = false;
            g_suspend.scan_started = false;
            return;
        }
    }
    /* CM can reopen scans and recreate a reconnect record after its ACL
     * notification. Verify only after its records and inquiry have drained. */
    if (bt_interface_get_scan_mode_fsm() || g_suspend.scan_pending) return;
    if (g_suspend.scan_failed)
    {
        if (!g_suspend.resume_pending) return;
        if (g_suspend.restoring)
        {
            /* A completed failure releases the hold; it is not an in-flight timeout. */
            btpan_suspend_restore(-RT_ERROR);
            return;
        }
        g_suspend.restoring = true;
        g_suspend.expected_scan = g_pan.enabled && !g_reset.status.paused ? 3 : 0;
        g_suspend.scan_failed = false;
        g_suspend.scan_started = g_suspend.scan_pending = g_suspend.scan_write_pending = true;
        g_suspend.result = -RT_EBUSY;
        g_suspend.started = rt_tick_get();
        /* Failed reads also overwrite the SDK cache. Issue one native write,
         * even if that invalid cache equals the requested restoration mode. */
        gap_wr_scan_enb_req(bts2_task_get_app_task_id(),
                           g_suspend.expected_scan & 1, (g_suspend.expected_scan >> 1) & 1);
        return;
    }
    if (!g_suspend.scan_confirmed)
    {
        if (g_suspend.scan_started) return;
        g_suspend.scan_started = true;
        g_suspend.scan_pending = true;
        if (bt_interface_get_current_scan_mode() != g_suspend.expected_scan)
        {
            g_suspend.scan_write_pending = true;
            btpan_scan_mode(g_suspend.expected_scan);
        }
        else if (!g_suspend.restoring)
        {
            /* A no-op setter emits no command. Read once to verify quiet. */
            gap_rd_scan_enb_req(bts2_task_get_app_task_id());
        }
        else
        {
            g_suspend.scan_pending = false;
            btpan_suspend_restore(RT_EOK);
        }
        return;
    }
    if (g_suspend.restoring)
    {
        btpan_suspend_restore(RT_EOK);
        return;
    }
    g_pan.bt_connected = g_pan.pan_connected = RT_FALSE;
    g_pan.pan_draining = g_pan.earphone_draining = RT_FALSE;
    g_pan.pan_cancelled = RT_FALSE;
    g_suspend.result = RT_EOK;
    if (g_suspend.resume_pending)
    {
        /* CM may temporarily use mode 0/2 for its connections. Restore the
         * platform's on/off policy once those connections have drained. */
        uint8_t mode = g_pan.enabled && !g_reset.status.paused ? 3 : 0;
        if (mode == 0)
        {
            btpan_suspend_restore(RT_EOK);
            return;
        }
        g_suspend.restoring = true;
        g_suspend.expected_scan = mode;
        g_suspend.scan_started = g_suspend.scan_confirmed = false;
        g_suspend.result = -RT_EBUSY;
        g_suspend.started = rt_tick_get();
        btpan_post(PAN_MSG_SUSPEND);
    }
}

rt_err_t btpan_suspend(void)
{
    rt_base_t level = network_reset_lock();
    rt_err_t result = RT_EOK;
    if (g_suspend.held && g_suspend.result != RT_EOK && g_suspend.result != -RT_EBUSY)
        result = g_suspend.result;
    else if (g_suspend.initializing || g_suspend.resume_pending)
        result = -RT_EBUSY;
    else if (g_suspend.held)
        result = g_suspend.result == -RT_EBUSY || g_suspend.result == RT_EOK ? RT_EOK : g_suspend.result;
    else if (g_reset.status.paused ||
             g_suspend.scan_pending ||
             bt_audio_is_active() || btpan_ble_activity_active())
        result = -RT_ERROR;
    else
    {
        if (g_pan.initialized) result = btpan_post(PAN_MSG_SUSPEND);
        if (result == -RT_EFULL) result = RT_EOK;
        if (result == RT_EOK)
        {
            g_suspend.held = true;
            g_suspend.started = rt_tick_get();
            g_suspend.result = g_pan.initialized ? -RT_EBUSY : RT_EOK;
            g_suspend.started_work = g_suspend.restoring = false;
            g_suspend.progress = false;
            g_suspend.expected_scan = 0;
            memset(g_suspend.acl_requests, 0, sizeof(g_suspend.acl_requests));
            memset(g_suspend.pan_requests, 0, sizeof(g_suspend.pan_requests));
            g_suspend.scan_started = g_suspend.scan_pending = g_suspend.scan_write_pending = false;
            g_suspend.scan_confirmed = g_suspend.scan_failed = false;
            if (g_pan.initialized)
            {
                if (g_pan.bt_connected && !g_pan.manual_disconnect && !g_pan.pan_failed)
                    g_pan.restore_pending = RT_TRUE;
                btpan_reset_mailbox();
                btpan_stop_timer();
                btpan_sync_target();
                btpan_notify_state();
            }
        }
    }
    network_reset_unlock(level);
    return result;
}

rt_err_t btpan_suspend_result(void)
{
    rt_base_t level = network_reset_lock();
    if (g_suspend.held && g_suspend.result == -RT_EBUSY &&
        rt_tick_get() - g_suspend.started >= rt_tick_from_millisecond(BTPAN_SUSPEND_TIMEOUT_MS))
        g_suspend.result = -RT_ETIMEOUT;
    if (g_suspend.held && g_suspend.result == RT_EOK && g_pan.initialized &&
        (bt_audio_is_active() || btpan_ble_activity_active() ||
         btpan_suspend_links(false) || g_devices.snapshot.scanning || !g_suspend.scan_confirmed))
        g_suspend.result = -RT_ERROR;
    rt_err_t result = g_suspend.held ? g_suspend.result :
                      (!g_pan.initialized && !g_suspend.initializing ? RT_EOK : -RT_ERROR);
    network_reset_unlock(level);
    return result;
}

rt_err_t btpan_resume(void)
{
    rt_base_t level = network_reset_lock();
    rt_err_t result = RT_EOK;
    if (g_suspend.held && !g_pan.initialized)
        g_suspend.held = false;
    else if (g_suspend.held && !g_suspend.resume_posted)
    {
        /* Terminal holds have no polling timer. Preserve intent on IPC failure
         * and retry the wakeup on every call; the coordinator retries errors. */
        g_suspend.resume_pending = true;
        g_suspend.progress = true;
        result = btpan_post(PAN_MSG_SUSPEND);
        if (result == -RT_EFULL) result = RT_EOK;
        if (result == RT_EOK) g_suspend.resume_posted = true;
    }
    network_reset_unlock(level);
    return result;
}

rt_err_t btpan_resume_result(void)
{
    rt_base_t level = network_reset_lock();
    rt_err_t result = !g_suspend.held ? g_suspend.result :
        (g_suspend.result == RT_EOK ? -RT_EBUSY : g_suspend.result);
    network_reset_unlock(level);
    return result;
}

static void network_reset_fail(network_reset_error_t error)
{
    g_reset.status.error = error;
    g_reset.status.state = NETWORK_RESET_FAILED;
}

static void network_reset_check_timeout(void)
{
    if (network_reset_running() &&
        (rt_tick_t)(rt_tick_get() - g_reset.started) >=
        rt_tick_from_millisecond(NETWORK_RESET_TIMEOUT_MS))
        network_reset_fail(NETWORK_RESET_ERROR_TIMEOUT);
}

/* Called under the PAN mutex. SDK disconnect requests are not completions;
 * retain the gate until its connection records report no remaining links. */
static rt_bool_t network_reset_disconnect(void)
{
    rt_bool_t active = RT_FALSE;
#ifdef BSP_BT_CONNECTION_MANAGER
    if (bt_cm_get_conn_num())
    {
        active = RT_TRUE;
        bt_cm_disconnect_req();
    }
#endif
    if (btpan_sdk_pan_active())
    {
        active = RT_TRUE;
        btpan_disconnect_pan();
    }
#ifdef BSP_BLE_CONNECTION_MANAGER
    if (!g_reset.ble_ready) return RT_TRUE;
    /* The SDK's get_all_connected_index omits RESOLVING links. Query all
     * valid indices so these links also have to finish disconnecting. */
    for (unsigned index = 0; index < CM_CONN_INDEX_EMPTY; ++index)
    {
        uint8_t state = connection_manager_get_connection_state((uint8_t)index);
        if (state == CM_CONN_INDEX_ERROR || state == CONNECTION_STATE_DISCONNECTED)
            continue;
        active = RT_TRUE;
        if (state != CONNECTION_STATE_DISCONNECTING)
            connection_manager_disconnect((uint8_t)index);
    }
#endif
    return active;
}

static void network_reset_clear(void)
{
    network_reset_error_t error = NETWORK_RESET_ERROR_NONE;
    g_reset.clear_started = RT_TRUE;
#ifdef BSP_BT_CONNECTION_MANAGER
    /* sc_clean_all_link_key -> sc_db_remove synchronously updates flash.
     * Its void return hides write failures, so query the SC database afterward. */
    bt_cm_delete_bonded_devs();
    g_reset.status.classic_bonds = NETWORK_RESET_PENDING;
    g_reset.status.classic_targets = NETWORK_RESET_UNCONFIRMED;
#ifdef BSP_BLE_NVDS_SYNC
    uint8_t stored[SIFLI_NVDS_KEY_LEN_BT_CM];
    uint16_t length = sizeof(stored);
    g_reset.status.classic_store_status =
        sifli_nvds_read(SIFLI_NVDS_TYPE_BT_CM, &length, stored);
    if (g_reset.status.classic_store_status == NVDS_OK &&
        length == sizeof(bt_cm_bonded_dev_t))
    {
        unsigned index;
        for (index = 0; index < length && stored[index] == 0; ++index) {}
        if (index == length)
            g_reset.status.classic_targets = NETWORK_RESET_CONFIRMED;
    }
    if (g_reset.status.classic_targets != NETWORK_RESET_CONFIRMED)
    {
        g_reset.status.classic_targets = NETWORK_RESET_COMPONENT_FAILED;
        error = NETWORK_RESET_ERROR_CLASSIC_STORAGE;
    }
#else
    error = NETWORK_RESET_ERROR_CLASSIC_UNCONFIRMED;
#endif
#else
    g_reset.status.classic_bonds = NETWORK_RESET_UNCONFIRMED;
    error = NETWORK_RESET_ERROR_SDK_UNSUPPORTED;
#endif

#if defined(BSP_BLE_CONNECTION_MANAGER) && !defined(BLE_CM_BOND_DISABLE)
    uint8_t result = connection_manager_delete_all_bond();
    g_reset.status.ble_store_status = result;
    if (result == CM_STATUS_OK)
        g_reset.status.ble_bonds = NETWORK_RESET_CONFIRMED;
    else if (result == CM_FLASH_PEDING)
    {
        /* NVDS completion has a type, but no request ID. A preceding bond
         * write can emit the same event; it cannot certify this deletion. */
        g_reset.status.ble_bonds = NETWORK_RESET_UNCONFIRMED;
        g_reset.erase_unconfirmed = RT_TRUE;
        error = NETWORK_RESET_ERROR_BLE_PENDING;
    }
    else
    {
        g_reset.status.ble_bonds = NETWORK_RESET_COMPONENT_FAILED;
        error = NETWORK_RESET_ERROR_BLE_STORAGE;
    }
#endif
    if (error != NETWORK_RESET_ERROR_NONE) network_reset_fail(error);
    else
    {
        g_reset.classic_query_pending = RT_TRUE;
        sc_rd_paired_dev_record_req(bts2_task_get_app_task_id());
    }
}

static void network_reset_step(void)
{
    if (!g_reset.status.paused) return;
    network_reset_check_timeout();
    if (!g_pan.stack_ready) return;
    if (g_reset.scan_pending)
    {
        g_reset.scan_pending = RT_FALSE;
        bt_interface_set_scan_mode(FALSE, FALSE);
    }
    if (g_reset.status.state == NETWORK_RESET_WAIT_STACK)
    {
#ifdef BSP_BLE_CONNECTION_MANAGER
        if (!g_reset.ble_ready) return;
#endif
        g_reset.status.state = NETWORK_RESET_DISCONNECTING;
    }
    if (network_reset_disconnect()) return;
    if (g_reset.status.state == NETWORK_RESET_DISCONNECTING)
    {
        g_pan.bt_connected = RT_FALSE;
        g_pan.pan_connected = RT_FALSE;
        g_pan.pan_draining = RT_FALSE;
        g_pan.pan_cancelled = RT_FALSE;
        g_reset.status.state = NETWORK_RESET_CLEARING;
        /* Expose the phase before doing persistent work on the next wakeup. */
        return;
    }
    if (g_reset.status.state == NETWORK_RESET_CLEARING)
    {
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
        if (g_reset.status.wlan_credentials != NETWORK_RESET_CONFIRMED)
        {
            rt_err_t result;
            if (!g_reset.wlan_reset_started)
            {
                result = wlan_service_reset_begin();
                if (result != RT_EOK)
                {
                    g_reset.status.wlan_credentials = NETWORK_RESET_COMPONENT_FAILED;
                    network_reset_fail(NETWORK_RESET_ERROR_WLAN_STORAGE);
                    return;
                }
                g_reset.wlan_reset_started = RT_TRUE;
            }
            result = wlan_service_reset_result();
            if (result == -RT_EBUSY) return;
            if (result != RT_EOK)
            {
                g_reset.status.wlan_credentials = NETWORK_RESET_COMPONENT_FAILED;
                network_reset_fail(NETWORK_RESET_ERROR_WLAN_STORAGE);
                return;
            }
            g_reset.status.wlan_credentials = NETWORK_RESET_CONFIRMED;
        }
#endif
        if (!g_reset.clear_started) network_reset_clear();
        else if (g_reset.status.classic_bonds == NETWORK_RESET_CONFIRMED)
            g_reset.status.state = NETWORK_RESET_SUCCEEDED;
    }
}

static int network_reset_bt_event(uint16_t type, uint16_t event, uint8_t *data,
                                  uint32_t context)
{
    (void)context;
    /* This raw callback also sees audio traffic. Only discovery and bond
     * replies use PAN state; unrelated events must not wait on its mutex. */
    if (!data || !((type == BTS2M_GAP &&
        (event == BTS2MU_GAP_DISCOV_CFM || event == BTS2MU_GAP_ESC_DISCOV_CFM ||
         event == BTS2MU_GAP_RD_LOCAL_VERSION_CFM || event == BTS2MU_GAP_RD_SCAN_ENB_CFM ||
         event == BTS2MU_GAP_WR_SCAN_ENB_CFM)) ||
        (type == BTS2M_SC && (event == BTS2MU_SC_UNPAIR_CFM ||
                             event == BTS2MU_SC_RD_PAIRED_DEV_RECORD_CFM))))
        return 0;
    if (g_pan.initialized)
    {
        rt_mutex_take(&g_pan_lock, RT_WAITING_FOREVER);
        if (type == BTS2M_GAP && event == BTS2MU_GAP_WR_SCAN_ENB_CFM &&
            g_suspend.held && g_suspend.scan_write_pending)
        {
            const BTS2S_GAP_WR_SCAN_ENB_CFM *reply = (const void *)data;
            uint8_t mode = (reply->inquiry_scan ? 1 : 0) | (reply->page_scan ? 2 : 0);
            /* The native WR handler queues RD even on failure. Wait for that
             * readback; earlier reads cannot complete this outstanding write. */
            if (mode == g_suspend.expected_scan) g_suspend.scan_write_pending = false;
        }
        if (type == BTS2M_GAP && event == BTS2MU_GAP_RD_SCAN_ENB_CFM &&
            g_suspend.held && !g_suspend.scan_write_pending)
        {
            const BTS2S_GAP_RD_SCAN_ENB_CFM *reply = (const void *)data;
            bool mismatch = g_suspend.scan_pending && reply->scan_enb != g_suspend.expected_scan;
            g_suspend.scan_pending = false;
            if (reply->res != BTS2_SUCC || mismatch)
            {
                g_suspend.scan_confirmed = false;
                g_suspend.scan_failed = true;
                g_suspend.result = -RT_ERROR;
                g_suspend.progress = true;
            }
            else
            {
                g_suspend.scan_failed = false;
                g_suspend.scan_confirmed = g_suspend.scan_started &&
                    reply->scan_enb == g_suspend.expected_scan;
                if (!g_suspend.scan_confirmed) g_suspend.scan_started = false;
                g_suspend.progress = true;
            }
            /* The SDK clears its FSM after the public notification. Continue
             * on the worker, never submit another write inside that callback. */
            btpan_post(PAN_MSG_SUSPEND);
        }
        btpan_devices_query(type, event, data);
        if (g_suspend.held &&
            (event == BTS2MU_GAP_DISCOV_CFM || event == BTS2MU_GAP_ESC_DISCOV_CFM))
        {
            g_suspend.progress = true;
            btpan_post(PAN_MSG_SUSPEND);
        }
        rt_mutex_release(&g_pan_lock);
    }
    if (type != BTS2M_SC || event != BTS2MU_SC_RD_PAIRED_DEV_RECORD_CFM || !data)
        return 0;
    rt_base_t level = network_reset_lock();
    if (g_reset.classic_query_pending)
    {
        BTS2S_SC_RD_PAIRED_DEV_RECORD_CFM *reply = (BTS2S_SC_RD_PAIRED_DEV_RECORD_CFM *)data;
        g_reset.classic_query_pending = RT_FALSE;
        if (g_reset.status.state == NETWORK_RESET_CLEARING)
        {
            if (reply->total_dev_num == 0)
                g_reset.status.classic_bonds = NETWORK_RESET_CONFIRMED;
            else
            {
                g_reset.status.classic_bonds = NETWORK_RESET_COMPONENT_FAILED;
                network_reset_fail(NETWORK_RESET_ERROR_CLASSIC_STORAGE);
            }
        }
        if (g_pan.initialized) btpan_post(PAN_MSG_RESET);
    }
    network_reset_unlock(level);
    return 0;
}
BT_EVENT_REGISTER_LOW(network_reset_bt_event, NULL);

#ifdef BSP_BLE_CONNECTION_MANAGER
/* No BLE advertising owner exists in the PAN service. Observe external radio
 * activities and refuse suspension until their owner stops them. */
static void btpan_ble_activity_event(uint16_t event, const uint8_t *data, uint16_t length)
{
    if (!data || !length) return;
    if (event == BLE_GAP_START_ADV_CNF || event == BLE_GAP_START_PERIODIC_ADV_SYNC_CNF)
    {
        if (length >= 2 && data[1] == 0)
            g_suspend.ble_activities[data[0] / 8] |= (uint8_t)(1u << (data[0] % 8));
    }
    else if (event == BLE_GAP_ADV_STOPPED_IND || event == BLE_GAP_PERIODIC_ADV_SYNC_STOPPED_IND)
        g_suspend.ble_activities[data[0] / 8] &= (uint8_t)~(1u << (data[0] % 8));
    else if (event == BLE_GAP_SCAN_START_CNF && data[0] == 0)
        g_suspend.ble_scanning = true;
    else if (event == BLE_GAP_SCAN_STOPPED_IND)
        g_suspend.ble_scanning = false;
    else if (event == BLE_GAP_CREATE_CONNECTION_CNF && data[0] == 0)
        g_suspend.ble_initiating = true;
    else if (event == BLE_GAP_CREATE_CONNECTION_STOP_IND || event == BLE_GAP_CONNECTED_IND)
        g_suspend.ble_initiating = false;
}

static int network_reset_ble_event(uint16_t event, uint8_t *data, uint16_t length,
                                   uint32_t context)
{
    (void)data;
    (void)length;
    (void)context;
    rt_base_t level = network_reset_lock();
    if (event == BLE_POWER_ON_IND)
    {
        if (g_suspend.held && g_reset.ble_ready) g_suspend.result = -RT_ERROR;
        g_reset.ble_ready = RT_TRUE;
    }
    btpan_ble_activity_event(event, data, length);
    if (g_suspend.held && g_pan.initialized)
    {
        if (!g_suspend.resume_pending && btpan_ble_activity_active()) g_suspend.result = -RT_ERROR;
        else g_suspend.progress = true;
        btpan_post(PAN_MSG_SUSPEND);
    }
    if (g_reset.status.paused)
    {
        connection_manager_set_bond_ack(BOND_REJECT);
        if (g_pan.initialized) btpan_post(PAN_MSG_RESET);
    }
    network_reset_unlock(level);
    return 0;
}
/* Run after the SDK connection manager initializes and updates link state. */
BLE_EVENT_REGISTER_LOW(network_reset_ble_event, NULL);
#endif

static rt_err_t network_reset_hold_locked(void)
{
    g_reset.status.paused = true;
    rt_err_t wlan_result = RT_EOK;
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
    wlan_result = wlan_service_reset_hold();
#endif
    g_pan.earphone_draining = btpan_earphone_active();
    g_reset.scan_pending = RT_TRUE;
#ifdef BSP_BLE_CONNECTION_MANAGER
    connection_manager_set_bond_ack(BOND_REJECT);
#endif
    btpan_sync_target();
    if (g_pan.initialized)
    {
        btpan_reset_mailbox();
        btpan_stop_timer();
        rt_err_t result = btpan_post(PAN_MSG_RESET);
        btpan_notify_state();
        if (result != RT_EOK && result != -RT_EFULL) return result;
    }
    return wlan_result;
}

rt_err_t network_reset_hold(void)
{
    rt_base_t level = network_reset_lock();
    if (g_suspend.held)
    {
        network_reset_unlock(level);
        return -RT_EBUSY;
    }
    rt_err_t result = network_reset_hold_locked();
    network_reset_unlock(level);
    return result;
}

rt_err_t network_reset_start(void)
{
    rt_base_t level = network_reset_lock();
    if (g_suspend.held || network_reset_running() || g_reset.erase_unconfirmed || g_reset.classic_query_pending
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
        || (g_reset.wlan_reset_started && wlan_service_reset_result() == -RT_EBUSY)
#endif
       )
    {
        network_reset_unlock(level);
        return -RT_EBUSY;
    }
    memset(&g_reset.status, 0, sizeof(g_reset.status));
    g_reset.status.state = NETWORK_RESET_WAIT_STACK;
    g_reset.status.paused = true;
    g_reset.status.classic_bonds = NETWORK_RESET_PENDING;
    g_reset.status.classic_targets = NETWORK_RESET_PENDING;
#if defined(BSP_BLE_CONNECTION_MANAGER) && !defined(BLE_CM_BOND_DISABLE)
    g_reset.status.ble_bonds = NETWORK_RESET_PENDING;
#endif
    g_reset.started = rt_tick_get();
    g_reset.clear_started = RT_FALSE;
    g_reset.wlan_reset_started = RT_FALSE;
    /* CM's init reads its own saved peer, but preserves these profile targets.
     * Both roles must be gated before sifli_ble_enable can start the stack. */
    rt_err_t result = network_reset_hold_locked();
    memset(&g_pan.bd_addr, 0, sizeof(g_pan.bd_addr));
    if (result != RT_EOK) network_reset_fail(NETWORK_RESET_ERROR_CONTROL);
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
    g_reset.status.wlan_credentials = NETWORK_RESET_PENDING;
#elif defined(RT_USING_WIFI)
    g_reset.status.wlan_credentials = NETWORK_RESET_UNCONFIRMED;
    network_reset_fail(NETWORK_RESET_ERROR_WLAN_UNSUPPORTED);
#endif
    network_reset_unlock(level);
    return result;
}

void network_reset_get_status(network_reset_status_t *status)
{
    if (!status) return;
    rt_base_t level = network_reset_lock();
    network_reset_check_timeout();
    *status = g_reset.status;
    network_reset_unlock(level);
}

rt_err_t network_reset_resume(void)
{
    rt_base_t level = network_reset_lock();
    if (network_reset_running() || g_reset.classic_query_pending
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
        || (g_reset.wlan_reset_started && wlan_service_reset_result() == -RT_EBUSY)
#endif
       )
    {
        network_reset_unlock(level);
        return -RT_EBUSY;
    }
    if (g_reset.erase_unconfirmed)
    {
        network_reset_unlock(level);
        return -RT_ENOSYS;
    }
    if (!g_reset.status.paused)
    {
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
        wlan_service_resume_for_reset();
#endif
        network_reset_unlock(level);
        return RT_EOK;
    }
    if (g_pan.initialized)
    {
        rt_err_t result = btpan_schedule_apply();
        if (result != RT_EOK)
        {
            network_reset_unlock(level);
            return result;
        }
    }
    g_reset.status.paused = false;
#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
    wlan_service_resume_for_reset();
#endif
#ifdef BSP_BLE_CONNECTION_MANAGER
    connection_manager_set_bond_ack(BOND_ACCEPT);
#endif
    if (g_pan.initialized)
    {
        btpan_sync_target();
        btpan_notify_state();
    }
    network_reset_unlock(level);
    return RT_EOK;
}
