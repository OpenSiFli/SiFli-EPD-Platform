/**
 * @file bt_pan.h
 * @brief 蓝牙 PAN 联网服务 —— 对外接口（UI / 天气服务调用）
 *
 * 功能：设备开机后蓝牙可被手机发现、配对、连接；
 *       手机连接并共享网络后，设备作为 PAN 客户端（PANU）接入，
 *       为整机提供互联网出口（天气等数据请求走此链路）。
 *
 * 典型状态流转：
 *   OFF → INITIALIZING → READY（等待手机连接）
 *       → CONNECTED（手机已连接，链路建立）
 *       → NETWORK_READY（PAN profile 已连接；IP/网关通过 network 服务查询）
 *
 * 线程说明：
 *  - 所有接口线程安全，可在任意线程 / UI 线程调用；
 *  - 业务联网状态通过 network.h 的多订阅者事件接口获取。
 *
 * 命名说明：对外前缀 btpan_（SDK 中 bt_pan_ 前缀已被占用，避免符号冲突）。
 */
#ifndef __BT_PAN_H__
#define __BT_PAN_H__

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 蓝牙 PAN 服务状态 */
typedef enum
{
    BTPAN_STATE_OFF = 0,       /**< 未初始化 / 设置页已关闭蓝牙 */
    BTPAN_STATE_INITIALIZING,  /**< 蓝牙协议栈启动中 */
    BTPAN_STATE_READY,         /**< 蓝牙就绪，等待手机连接 */
    BTPAN_STATE_CONNECTED,     /**< 手机已连接（链路建立），网络未就绪 */
    BTPAN_STATE_NETWORK_READY, /**< PAN profile connected; query network for IP readiness. */
} btpan_state_t;

/* ==================== 生命周期 ==================== */

/**
 * @brief 初始化 PAN 服务（应用入口调用一次）
 * @param device_name 蓝牙设备名（手机端显示的名称），NULL 使用默认值
 * @return RT_EOK 成功
 */
rt_err_t btpan_init(const char *device_name);

/* Asynchronous platform Bluetooth quiescence, called from thread context.
 * suspend: RT_EOK accepts the request; rejects new work during restoration or
 * a latched failure. Audio/BLE activity must first be stopped by its owner.
 * suspend_result: RT_EOK means inquiry, ACL/PAN and scans are quiet, not LCPU
 * asleep; -RT_EBUSY means pending, other errors are terminal without progress.
 * resume: RT_EOK transfers restoration ownership to this service. Retry only
 * submission errors; accepted requests need not be repeated.
 * resume_result: RT_EOK means restored, -RT_EBUSY pending, otherwise failure.
 * A failed scan read permits one rollback write; a completed rollback failure
 * releases the hold with an error. Restore scans follow the current on/off setting.
 * A 30-second timeout gates new work until real completions or stack restart.
 * Ordinary sleep preserves preferences/bonds and does not close/open GAP.
 */
rt_err_t btpan_suspend(void);
rt_err_t btpan_suspend_result(void);
rt_err_t btpan_resume(void);
rt_err_t btpan_resume_result(void);

/**
 * @brief 蓝牙开关（对应设置页“蓝牙”开关；关闭会断开当前连接）
 * @param enable true 打开（恢复可被发现/连接），false 关闭
 * @return RT_EOK 表示请求已接受；连接控制在 PAN 工作线程执行。
 * @note PAN 随蓝牙总开关启停；重新打开仅提交一次手机连接恢复请求。
 *       关闭会停止连接请求，不关闭蓝牙控制器电源。
 *       network_reset_hold/start 暂停期间仍可设置蓝牙开关，解除暂停前不发起连接。
 */
rt_err_t btpan_enable(bool enable);

#define BTPAN_LOCAL_NAME_MAX_BYTES 60

typedef enum {
    BTPAN_OP_NONE, BTPAN_OP_SCAN, BTPAN_OP_CONNECT,
    BTPAN_OP_DISCONNECT, BTPAN_OP_UNPAIR
} btpan_operation_t;

typedef struct {
    uint8_t address[6];
    char *name;
    bool paired, available, connected, pan_connected;
    bool earphone; /* Active CM role, otherwise reported class or saved role. */
    uint32_t device_class; /* Latest nonzero class reported by the device. */
    bool connecting; /* An accepted connect request is pending for this device. */
    int rssi;
} btpan_device_t;

typedef struct {
    uint32_t revision;
    bool supported, enabled, ready, busy, scanning, truncated;
    btpan_operation_t operation;
    rt_err_t error;
    unsigned count;
    btpan_device_t *devices;
} btpan_snapshot_t;

/* Copies owned data; names are empty until reported by the remote device.
 * Connection means ACL; pan_connected means PAN, not DHCP/Internet readiness. */
/* A successful snapshot owns one allocation. Release it before overwriting.
 * Allocation failure returns -RT_ENOMEM; truncated/error report a discovery
 * allocation failure. There is no application-imposed device-count limit. */
rt_err_t btpan_snapshot(btpan_snapshot_t *out);
void btpan_snapshot_free(btpan_snapshot_t *snapshot);
rt_err_t btpan_scan(void);
/* Idempotent. Active inquiry stays busy until the SDK confirms it stopped. */
rt_err_t btpan_cancel_scan(void);
rt_err_t btpan_connect(const uint8_t address[6]);
rt_err_t btpan_disconnect(const uint8_t address[6]);
rt_err_t btpan_unpair(const uint8_t address[6]);

/* Nonempty, valid UTF-8, at most 60 bytes excluding NUL; never truncated.
 * Requires btpan_init. RT_EOK queues the SDK local-name/EIR update, including
 * before stack-ready. SDK has no correlated completion for this void setter.
 * The caller owns persistence and supplies the saved name at the next boot. */
rt_err_t btpan_set_local_name(const char *name);

/* ==================== 连接控制 ==================== */

/**
 * @brief 请求建立 PAN 连接，手机基础蓝牙连接必须已建立。
 * @note 本次手机连接的 PAN 请求失败后，不再发起请求，直到重新连接手机。
 */
rt_err_t btpan_request_connect(void);

/* ==================== 状态查询（供 UI 状态栏 / 设置页） ==================== */

/** 当前状态 */
btpan_state_t btpan_get_state(void);
/** 蓝牙协议栈是否就绪 */
bool btpan_is_ready(void);
/** 手机是否已连接（链路建立） */
bool btpan_is_connected(void);
/** PAN profile connected; this does not check DHCP, DNS or Internet access. */
bool btpan_is_network_ready(void);

/* ==================== 本机蓝牙 MAC 地址 ==================== */

/**
 * @brief 读取当前本机蓝牙 MAC 地址（格式化 "XX:XX:XX:XX:XX:XX"；buf 建议 ≥ 18 字节）
 */
void btpan_get_local_addr(char *buf, rt_size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BT_PAN_H__ */
