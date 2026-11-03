---
title: "网络服务"
---

# 网络服务

应用通过 `src/services/net/network.h` 读取统一路由，不依赖 WLAN 或蓝牙的私有状态。EPD DevKit 使用官方 D40-N 驱动提供 WLAN STA，蓝牙 PAN 提供另一条上行链路。

## 路由与请求

`network_snapshot()` 返回当前来源、地址、可用状态和变化序号 `generation`。平台始终优先使用已就绪的 WLAN；WLAN 不可用而 PAN 已就绪时使用 PAN，两者都不可用时报告离线。设置页面没有手动固定上行来源的入口。路由选择不负责主动建立 PAN，也不会因业务请求失败反复连接 PAN。

`ready` 表示链路、地址和网关满足平台条件。网络任务保存开始时的 generation，连接期间检查变化，在路由或地址改变后结束旧请求。DNS、连接、TLS 和 HTTP 错误由具体请求处理。

平台在 TCP/IP 线程更新默认网卡和 DNS，各来源分别保存 DNS。STA 设备为 `w0`，PAN 为 `b0`。受管服务通过 `network: true` 接收网络事件；独立订阅者在 `network_unsubscribe()` 返回后再释放事件对象，见[生命周期](../development/application-lifecycle.md)。

## WLAN 异步接口

接口位于 `src/services/net/wlan_service.h`。UI 提交命令，worker 执行 SDK 调用和保存。`RT_EOK` 表示受理成功，最终结果通过 `wlan_service_snapshot()` 的 `busy`、`operation`、`error` 读取。每份快照包括动态网络列表，使用后调用 `wlan_service_snapshot_free()`，空列表也须释放。

| 入口 | 用法与条件 |
| --- | --- |
| `wlan_service_capabilities()` | 查询扫描、认证、热点、并发、节电、休眠等能力。 |
| `scan()`、`scan_network()` | 普通或指定 SSID、BSSID、信道的扫描。以下短名均带 `wlan_service_` 前缀。 |
| `connect()`、`connect_ssid()` | 连接指定网络或隐藏 SSID；密码为 NULL 复用保存凭据，空字符串表示无密码。 |
| `cancel_scan()`、`cancel_connect()` | 提交取消，底层操作返回前仍为忙；连接取消后暂停自动重连，直至用户重新连接、更改自动连接设置或重开 WLAN。 |
| `set_autoconnect()`、`forget()` | 保存自动连接偏好或删除网络，保存错误进入快照。 |
| `set_powersave(0/1)` | STA 分别关闭低功耗或启用时钟门控。 |
| `set_ipv4()`、`get_ipv4()` | DHCP 或静态地址、掩码、网关和两项 DNS。 |
| `hotspot_start_security()`、`hotspot_stop()` | 按查询到的 AP 认证能力启动或停止热点。 |
| `concurrent_start()`、`concurrent_stop()` | STA 上行与本地 AP 同时运行。 |
| `hotspot_clients()`、`hotspot_deauth()` | 查询客户端 MAC 或断开客户端；输出容量不足返回 `-RT_EFULL`。 |

热点、静态 IP 和并发能力由接口提供，系统 WLAN 页面主要提供扫描、连接和已保存网络管理。P2P 能力位为零，扩展命令返回 `-RT_ENOSYS`。

热点入口要求 SSID、密码长度分别小于 `RT_WLAN_SSID_MAX_LENGTH`、`RT_WLAN_PASSWORD_MAX_LENGTH`。当前 EPD 板型两项配置均为 32，因此 SSID 最多 31 字节，加密热点密码为 8～31 字节；开放热点不设置密码。热点快照的 channel 是请求信道。AP 使用 `w1`、地址 `192.168.169.1/24`，不参与上行选择，不提供 NAT；与 STA 网段冲突时停止 AP 并保留 STA。

## 保存与休眠

凭据使用 SDK 格式保存在 `/flash/config/wlan.cfg`，最多十个网络；自动重连偏好位于 `wlan-auto.bin`，IPv4 配置位于 `wlan-ipv4.bin`。网络重置清理这些记录。重置须等 SDK 旧保存任务和连接操作结束，超时后仍保持业务停止状态，直到旧操作结束才能恢复或重试。

`wlan_service_suspend()` 异步关闭 WLAN；`suspend_result()` 为 `-RT_EBUSY` 时仍在处理，结束错误会取消本次系统休眠。关闭成功后确认 SDIO 请求、IRQ、锁和 DMA 空闲，封闭总线访问，停止卡时钟，并通过 SDK 电源接口拉低模块使能脚。该路径适用于 SF32LB57x 非 OOB 中断、GPIO 直接控制模块使能的配置。恢复使用 `wlan_service_resume()`，重新上电、选卡和加载无线固件；保留 WLAN 开关及自动连接偏好，尝试恢复休眠前已经建立的连接。

## 硬件与实现

D40-N 使用 SDMMC1：PA12～PA17 对应 D2、D3、CLK、CMD、D0、D1；PA8 供电、PA9 PDN。TF 卡使用独立的 SDMMC2。启用 `AIC_WLAN_AIC8800D40N`、`AIC8800_USE_RT_WLAN` 和四位 SDIO，详细引脚见板级配置（源码：`boards/sf32lb57-epd-gyd7n6/`）。

实现入口为 `boards/sf32lb57-epd-gyd7n6/bsp_wifi.c`、`components/wlan/src/wlan_rt_adapter.c`、`src/services/net/wlan_service.c`、`wlan_ipv4.c`、`wlan_ap_network.c`。应用使用上层网络快照，设备控制由这些组件完成。
