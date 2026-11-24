---
title: "应用生命周期"
---

# 应用生命周期

## 核心概念

动态应用可以在前台显示页面，也可以在后台运行任务。平台会管理应用的加载、运行和卸载，确保资源正确释放。

## 网络访问

所有网络请求都通过统一的网络服务。不要自己去连接 Wi-Fi 或蓝牙，直接用 `network.h` 的接口就行。

### 检查网络状态

```c
#include "services/net/network.h"

// 检查网络是否可用
if (network_ready()) {
    // 可以发起网络请求
    make_http_request();
}

// 获取详细状态
network_snapshot_t state;
network_snapshot(&state);

if (state.ready) {
    // ready = true 表示有可用网络
    // source 表示用的是 Wi-Fi 还是蓝牙网络共享
    LOG_I("app", "Network ready, source=%d", state.source);
}
```

::: tip 网络选择策略
平台自动选择最优网络：
- 优先使用 Wi-Fi
- Wi-Fi 不可用时自动切换到蓝牙网络共享（如果已连接）
:::

### 监听网络变化

后台任务可以订阅网络变化事件：

```c
// 创建事件对象
struct rt_event network_event;
rt_event_init(&network_event, "myapp_net", RT_IPC_FLAG_FIFO);

// 订阅网络变化
uint32_t token = network_subscribe(&network_event, 1);
if (token == 0) {
    // 订阅失败
    return -1;
}

// 在工作线程中等待事件
while (!stopping) {
    rt_uint32_t events;
    rt_event_recv(&network_event, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                  RT_WAITING_FOREVER, &events);
    
    // 网络状态变化，重新检查
    network_snapshot_t state;
    network_snapshot(&state);
    
    if (state.ready) {
        // 网络恢复，重试请求
        retry_failed_requests();
    }
}

// 清理
network_unsubscribe(token);
rt_event_detach(&network_event);
```

::: warning 注意
`network_unsubscribe()` 返回后才能删除事件对象。这样能确保不会再收到事件通知。
:::

## 后台任务

应用可以在页面关闭后继续运行后台任务，比如下载数据、处理文件等。

### 创建后台服务

在 `app.json` 中声明：

```json
{
  "id": "myapp",
  "background": true
}
```

实现后台服务：

```c
static void my_service_run(epd_service_t *service) {
    LOG_I("myapp", "Service started");
    
    while (!epd_service_cancelled(service)) {
        // 等待事件（1 秒超时）
        uint32_t events = epd_service_wait(service, 1000);
        
        if (events & EPD_SERVICE_STOP) {
            // 收到停止请求
            break;
        }
        
        if (events & EPD_SERVICE_WAKE) {
            // 被唤醒，处理任务
            process_task();
            
            // 通知 UI 更新
            epd_service_ui_request(service, 100);
        }
        
        if (events & EPD_SERVICE_NETWORK) {
            // 网络状态变化
            handle_network_change();
        }
    }
    
    // 清理资源
    cleanup();
    LOG_I("myapp", "Service stopped");
}

// 导出服务
const epd_background_t epd_app_background = {
    .run = my_service_run,
    .stack_size = 8192,      // 线程栈大小（2-32 KiB）
    .priority = 10,          // 优先级（20 到 RT_THREAD_PRIORITY_MAX-1）
    .network = true,         // 接收网络变化事件
};
```
    .run = background_run,
    .stack_size = 4096,
    .priority = 24,
    .network = true
};
```

内部后台应用在开机目录枚举、安装/更新完成后启动，不需要先打开页面。
TF 卡应用在完成当前插卡会话的用户确认及数据迁移后允许运行。
显示入口图标本身仍不装载程序；声明常驻后台才会被服务管理器主动装载。
页面打开与后台各持有一个 SDK 模块引用。页面 ONSTOP 关闭页面上下文；后台是否继续执行取决于 RELEASE、STATE、TASKS 策略和当前任务。应用通过 `epd_app_background_set()` 声明需求，具体接口见[应用内存](application-memory.md#后台留存)。

停止顺序：阻止新打开 → UI 线程退出该应用页面 → 置停止标志并唤醒任务 →
任务取消请求、关闭资源并返回 → 等待在途模块命令返回 → 注销网络订阅 → UI 线程释放后台引用。
包装线程在公布完成后不再访问模块代码或服务对象，因此可以在 UI 线程执行最后一次 `dlclose()`。
不以 `rt_thread_delete()` 强杀仍在运行的模块任务，也不允许应用另留无人管理的线程或外部回调。
页面在生命周期回调中主动暂停业务并清理资源。页面关闭后，平台拒绝普通 LVGL 回调继续执行；在途工作结束后，UI 线程清理该上下文及其子上下文的对象、事件、定时器、动画、异步回调和输入订阅。对象析构在模块引用仍有效时执行。最终模块清理函数只执行一次，全部执行引用释放后才卸载代码。

### UI 线程分步任务

需要使用 LVGL 字体度量的任务不能与 UI 并发操作字体缓存。只需要 UI 分步工作的服务可以在
`epd_app_background` 中仅设置 `ui_process`，不设置 `run`、网络订阅、线程栈或优先级。
纯 UI 分步服务的描述符如下：

```c
const epd_background_t epd_app_background = {
    .ui_process = app_ui_process
};
```

平台每轮 UI 处理调用一次步骤函数；应用必须将工作拆成有限步骤，不能在其中阻塞等待网络或事件。
步骤函数签名为 `bool app_ui_process(epd_service_t *service, bool stopping)`。正常运行返回 `false`；停止阶段在工作线程及在途命令返回后，以 `stopping=true` 调用步骤函数。应用释放仍由界面持有的资源并返回 `true`，平台随后释放服务模块引用。`run` 与 `ui_process` 至少设置一个，也可同时设置，两部分共用服务槽和模块引用。

当前书架清单为 `background: false`，打开阅读页面时按需启动服务。文档 worker 响应文件请求，`rdview` worker 调度 ACPU 文档、字体、排版与页面合成；UI 步骤绑定完成结果并处理控件，不承担整章同步排版。页面退出后，服务按取消和关闭协议清理资源；不能根据后台描述符存在就认定应用会开机常驻。完整数据流见[阅读与双核计算](../architecture/reading-architecture.md)。

`modules/netdemo` 演示等待网络事件的受管服务及页面定时器，服务停止时统一解除网络订阅。`modules/hello` 演示页面、触摸、语言切换，以及通过状态保存接口恢复计数；示例不注册独立后台线程。

## 更新事务

用户在设备网页上传 `.sap`，或从“应用安装”浏览并选择存储中的 `.sap` 文件；TF 卡来源文件可放在任意可浏览目录。首次安装选择内部存储或 TF 卡，同 ID 的更新沿用原安装位置。
更新要求更高的应用版本；相同版本、降级、类型变化均拒绝。`build_id` 不参与更新版本判断。
更新前后 `data_version` 必须相同，不自动猜测数据迁移格式。卸载后重装时应用自行校验保留记录的版本。

安装准备阶段先请求页面退出及后台停止，最多等待 35 秒；超时返回“应用仍在使用，请重试”。
超时不修改应用文件，不强行回收模块；任务最终退出后可重新启动旧版本。
完整天气包与其他后台模块使用同一停止协议。
类型转换仅允许已知的旧天气资源包版本 1 升级为天气模块包版本 2 或更高版本，仍要求数据版本一致。

| 当前阶段 | 执行内容 |
|---|---|
| 准备 | 等待页面、后台任务和模块退出 |
| 检查 | 核对存储会话、应用版本、数据版本及目标卷空间 |
| 写入与校验 | `.sap` 解包到目标卷 `.app-install`，边写入边计算文件摘要并与包清单核对，再使用本次记录检查清单、ABI 和 ELF 元数据 |
| 完成处理 | 更新时先将旧目录改名 `.app-old`，再将暂存目录改名发布到 `apps/<id>` |
| 清理 | 清理旧包备份及 `cache/<id>` |
| 迁移数据 | 目标没有数据时，按下述规则从另一卷迁移 |
| 卸载 | 将已安装目录改名 `.app-remove`，删除该目录及缓存 |

安装来源统一为 `.sap` 文件，安装后的程序、资源和清单保存在应用目录中。
摘要读取和文件写入使用 32 KiB PSRAM 缓冲。写入处理短写，核对文件及整包长度，
检查 `fsync` 和文件操作结果。解包得到的文件摘要记录交给后续包验证复用，不再为同一安装重复完整读取所有文件；结构、长度、ABI 和 ELF 检查仍保留。
UI 显示实际阶段，校验和复制分别按该阶段的已处理字节数与总字节数显示百分比；
数据迁移有字节总量时也显示本阶段百分比，其他阶段只显示名称，不合成为总体进度。

内部临时目录位于 `/flash/`，TF 卡临时目录位于 `/sdcard/.epd/`。更新前，安装器将应用 ID 写入 `/flash/system/app-transaction.bin` 或 `/sdcard/.epd/app-transaction.bin` 并同步，然后将旧包改名为 `.app-old`，再将 `.app-install` 发布为正式目录。

启动或插卡时，安装器检查旧包：存在 `.app-old` 时读取日志，日志中的 ID 有效且正式目录缺失时，将旧包恢复到原位置；正式目录已存在时清理旧包。清理成功后删除未完成的 `.app-install`、`.app-remove` 和日志。存在旧包却无法读取有效日志，或恢复、清理失败时，返回错误并暂停对应卷的应用准备。未完成的复制不会续传。正常运行中发布 `rename` 失败，也会尝试当场移回旧目录。

安装、更新和卸载事务均保留对应卷的 `data/<id>`。更换安装位置的数据迁移在应用停止后单独完成：
目标已有数据时不覆盖，也不清理另一卷的源数据；目标为空时先复制并核对长度，再发布目录并清理源。可重建缓存不迁移。
数据迁移不保存恢复标记；目标尚未发布时，下一次迁移先丢弃当前应用对应的遗留暂存目录，再重新复制。
复制失败或拔卡时不发布未完成复制的包；存储事务无法恢复时保留对应卷的错误状态，不格式化存储。单个应用的数据迁移失败只限制该应用，其他正常应用仍可启动和卸载；失败应用的卸载入口也保留。插卡后的临时文件清理在后台执行，用户确认加载后才迁移数据、发布入口并启动后台应用。
拔卡时 UI 同时推进安装准备和服务退出，等安装线程关闭文件后才确认允许卸载文件系统。

运行模块及服务绑定原安装卷与插卡会话。拔卡立即使旧会话无效，迟到请求不转向下一张卡。
释放等待超过 35 秒时保留未结束模块，TF 卡继续处于等待释放状态；在 UI 回调能够返回的前提下，允许内置页面继续使用，不强制卸载仍在执行的代码。应用完成释放并提交确认后，组件可以继续退出流程。
等待释放期间检测到稳定的重新插卡后，组件可以发起一次新的释放请求。SDK 的 FAT 关闭失败可能遗留文件句柄。组件在卸载阶段确认仍有句柄、卸载失败或 SDK 请求超时后，会进入持续故障隔离；此时不强制卸载，也不挂载新卡，需要重启恢复。
SD 组件采用探卡 `1000 × 30 ms`、移除 `100 × 30 ms` 的等待预算；SDK 请求超时后保持卡隔离至重启。运行步骤和恢复条件见
[应用开发](application-development.md)及SD 卡组件源码（源码：`components/sdcard/`）。
