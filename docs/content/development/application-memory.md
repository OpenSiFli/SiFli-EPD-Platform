---
title: "应用内存"
---

# 应用内存

平台使用片上 SRAM 和外部 PSRAM。SRAM 容纳内核对象及有硬件约束的缓冲；图片、文本、网络响应和应用的大块工作区优先放入 PSRAM。各池的空闲量取决于已加载应用和正在执行的任务，分区大小见[平台架构](../architecture/architecture.md)。

## 默认归属

应用使用标准构建入口并包含 `platform/epd_app.h` 后，平台为每个运行实例建立内存账户。页面回调、受管服务和子线程在对应账户的执行上下文中运行；普通分配由分配器记录所属账户，释放或扩容时沿用原分配的归属。

LVGL 事件、定时器、动画和异步回调在注册时记录所属上下文。相同回调函数由不同应用或页面注册，仍分别计入各自账户。对象的构造、销毁、延迟布局及绘制使用对象创建时的归属。平台在回调执行期间保留模块引用，回调删除自身后也会等待执行返回，再允许卸载模块。

标准应用不需要在上述入口手动切换账户。公共图片、字体、阅读和音频组件负责登记自身缓存与在途工作，使用这些组件的应用复用同一套管理方法。

## 选择和释放内存

公共接口位于源码 `src/platform/epd_memory.h`，也由 `platform/epd_app.h` 包含：

```c
#include <stdbool.h>
#include "platform/epd_memory.h"

static bool process_buffer(size_t capacity)
{
    unsigned char *buffer = epd_app_alloc(capacity, EPD_APP_PSRAM);
    if (!buffer) return false;
    /* 在本次任务中使用 buffer。 */
    epd_app_free(buffer);
    return true;
}
```

`epd_app_alloc()` 返回调用者拥有的内存，失败返回 `NULL`。显式请求 PSRAM 不会回退到 SRAM。扩容使用 `epd_app_realloc()`，内存区域参数必须与原分配一致；先将返回值存入临时指针，成功后再更新原指针。释放使用 `epd_app_free()`，它接受 `NULL`。

| 对象来源 | 释放方式 |
| --- | --- |
| `epd_app_alloc/realloc` | `epd_app_free` |
| `lv_malloc` | `lv_free` |
| LVGL 控件 | 在 UI 线程删除控件；子对象随父对象删除 |
| `epd_app_font_create`、`epd_app_reading_font_create` | 去除控件和绘制引用后调用 `epd_app_font_destroy`；`fallback` 输出为空，缺字后备由字体组件管理 |
| HTTP、AI、语音的结果对象 | 使用对应 `clear` 或 `event_release` 接口 |
| SDK 带标签的缓存分配 | 使用该 SDK 分配器配套的释放接口 |

跨异步任务传递数据时明确谁保留、谁释放。页面内存、局部数组和事件借用指针不能在其有效期结束后继续交给 worker 使用；可复制数据或通过受管服务保留状态。

## 线程与硬件缓冲

`epd_thread_create_psram()` 将线程栈放入保留型 PSRAM，控制块仍使用 SRAM。正常退出或删除后，内核空闲线程回收两部分内存。动态应用优先通过[受管后台服务](application-lifecycle.md)创建任务，使任务退出与模块卸载关联。

DMA、跨核共享内存和显示缓冲还需要满足对齐、缓存一致性与设备可访问范围。计算任务使用 `epd_compute_buffer_alloc()`，图像或音频使用对应组件的缓冲契约；单纯改用 PSRAM 分配不能替代这些条件。

## 分配失败与用量

`epd_app_psram_available()` 返回应用可用 PSRAM 的瞬时统计；UI 线程可通过 `epd_app_ui_available()` 查看 LVGL 私有堆的可用量。统计值不是内存预留，也不保证同样大小的连续块可以分配。

每次分配失败均应结束本次操作或降低可重建缓存用量，并释放本次已经取得的资源。复现持续增长时，对比进入页面前、退出页面后和 worker 完成后的 `list_memheap`，结合[日常调试](../troubleshooting/development-debugging.md)检查所有权。

## 后台留存

应用通过 `epd_app_background_set()` 表达退出前台后的需求，平台统一决定停止、保留和回收时机：

| 策略 | 运行行为 |
| --- | --- |
| `EPD_APP_BG_RELEASE` | 默认策略。离开前台后停止工作，等待在途任务和清理完成，再释放运行实例。 |
| `EPD_APP_BG_STATE` | 内存充足时保留实例。压力回收时，等待任务结束和必要状态保存，再卸载；登记了 checkpoint 的应用从冷状态恢复，使用已有业务存储的应用通过正常启动重新加载。 |
| `EPD_APP_BG_TASKS` | 允许指定任务组中已接受的工作继续执行，任务结束后采用 `after_tasks` 指定的 RELEASE 或 STATE 策略。 |

例如，应用需要在退出页面后保留编辑状态，可以在提交修改前设置：

```c
epd_app_background_policy_t policy = {
    .mode = EPD_APP_BG_STATE,
    .group = EPD_APP_TASK_GROUP_NONE,
    .after_tasks = EPD_APP_BG_RELEASE
};
uint32_t revision;
epd_memory_result_t result = epd_app_background_set(&policy, &revision);
```

返回 `EPD_MEMORY_OK` 表示请求已接受，停止和保存随后由平台推进。`epd_app_background_get()` 可一次读取当前快照；需要持续显示后台状态的页面使用下述订阅接口。

应用自行实现持续业务时，通过 `epd_app_task_group_create()` 建立任务组，在工作被接受后调用 `epd_app_task_begin()`，再选择 TASKS 策略。所有工作、数据访问和清理结束后调用 `epd_app_task_end()`，最后释放任务组。空闲工作线程不计为持续任务。后台策略本身不申请休眠锁，也不提高线程优先级；持续工作的电源需求仍按[电源管理](../services/power-management.md)处理。

## 后台状态订阅

包含 `services/app_background.h`，在页面的 GUI 回调中调用 `epd_app_background_subscribe()`。平台捕获当前开放的 PAGE 范围、所属应用账户和实例代次，在 GUI 线程进入原范围后调用通知函数。worker 提交的状态变化也通过该通道送达页面，应用无需为后台状态增加定时查询。

注册成功后安排异步初始快照；投递前发生的新变化可以合并到同一次通知。事件中的 `state` 是完整快照，包含已接受策略、有效策略和实际状态。回调可以直接用最新快照更新页面，不依赖逐条收到中间状态。

| 事件字段 | 用法 |
| --- | --- |
| `instance_generation` | 标识本次后台绑定实例；重新绑定后采用新代次。 |
| `state.revision`、`state.applied_revision` | 分别表示已接受的策略修订，以及实际状态所应用的策略修订。可以与 `epd_app_background_set()` 返回的修订关联。 |
| `state.actual` | 查看当前为 FOREGROUND、RUNNING、RETAINED、STOPPING、COLD 或 RELEASED。 |
| `reason`、`result` | 表示本次快照的变化来源和结果。来源包括 INITIAL、POLICY、FOREGROUND、TASKS、ACTUAL、PERSISTENCE、STOP、REJECTED。 |
| `state.dirty_revision`、`state.saved_revision`、`state.last_result` | 查看当前数据修订、已确认保存的修订和后台处理结果。 |
| `sequence` | 实例内单调递增并在上限饱和的提交序号。到达上限后，状态变化仍可触发通知。 |

下面的最小示例将订阅句柄和最新快照放入页面对象。`background_view_t` 初始值为全零；实际页面可在 `background_notify()` 中更新已有控件。

```c
#include "services/app_background.h"

typedef struct {
    epd_app_background_subscription_t subscription;
    epd_app_background_event_t latest;
    bool has_state;
} background_view_t;

static void background_notify(const epd_app_background_event_t *event,
                              void *context)
{
    background_view_t *view = context;
    view->latest = *event;
    view->has_state = true;
}

static epd_memory_result_t background_view_start(background_view_t *view)
{
    if (view->subscription) return EPD_MEMORY_OK;
    view->has_state = false;
    return epd_app_background_subscribe(background_notify, view,
                                        &view->subscription);
}

static bool background_view_stop(background_view_t *view)
{
    if (!view->subscription) return true;
    if (epd_app_background_unsubscribe(view->subscription) != EPD_MEMORY_OK)
        return false;
    view->subscription = 0;
    return true;
}
```

在 `GUI_APP_MSG_ONSTART` 创建页面对象后调用 `background_view_start()`，检查返回值；`EPD_MEMORY_NO_METADATA` 表示当前无法登记订阅，`EPD_MEMORY_CLOSED` 表示运行时或所属范围不可用。页面可以保持“状态暂不可用”的显示，按后续明确的页面重建或用户重试动作重新注册。

在 GUI 销毁该页面对象及控件前调用 `background_view_stop()`；返回 true 后才释放供通知使用的上下文。通知参数 `event` 仅在本次回调期间有效，需要保留时像示例一样复制值。成功退订阻止后续回调；回调内自退订时，当前调用正常执行至返回。

受管 TASK/SERVICE 也可以在平台 operation worker 上订阅，通知在该 worker 的原范围内执行，回调不访问 LVGL。注册与主动退订使用同一个投递执行器和同一应用账户；普通应用入口的 APP 范围或任意自建线程不作为订阅入口。平台关闭所属范围或解除实例绑定后，排队的旧通知失效；已经开始的回调仍由关闭排空流程等待返回，再允许销毁上下文或卸载代码。空闲订阅只保存弱引用，不保持后台任务，也不阻止实例退出。

`EPD_APP_BG_STATE` 的可选 checkpoint 若因 I/O 或存储容量不足保存失败，并获准丢弃，最终状态为 `RELEASED`，`saved_revision` 保持原值，失败结果保留在 `result` 和 `state.last_result` 中，包括合并后的停止或实际状态通知。`COLD` 表示已完成持久化交接；必须保存的数据失败时继续保留，等待明确重试。页面自身的范围关闭后不再接收终态，生命周期收尾仍由平台负责。

## 状态保存与恢复

需要在实例卸载后恢复业务状态的应用，通过 `epd_checkpoint_register()` 登记状态键、格式版本、数据来源版本及大小上限，并提供三个回调：

| 回调 | 应用负责的工作 |
| --- | --- |
| `capture` | 在 UI 线程将稳定业务状态写入给定缓冲，返回有效字节数。 |
| `restore` | 解析已读取的字节，成功后发布恢复后的业务状态。 |
| `detach` | 在任务结束且必要状态保存完成后，释放状态提供者保留的模型或视图引用。 |

平台在后台执行文件读写，回调不执行文件操作。登记成功后异步加载已有状态；应用在发布新修改前调用 `epd_checkpoint_changed()`，防止迟到的恢复覆盖新编辑。失败时通过 `epd_checkpoint_get()` 查看结果，通过 `epd_checkpoint_retry()` 显式重试。必要状态保存失败时继续保留数据和所属账户，成功后才完成回收。

保存内容是应用可重建的业务数据，例如选择、位置和计数。LVGL 对象、线程、指针及打开的文件句柄在恢复时重新创建。完整示例见 `modules/hello/hello_state.c`。

## 资源分级与回收

公共组件通过 `epd_resource_context_begin()` 登记资源活动，并声明最低容量和目标容量。平台根据当前页面及任务的真实状态判断可用等级：

| 等级 | 用途 |
| --- | --- |
| `EPD_RESOURCE_OPTIONAL` | 随时可重建的预取和远期缓存。 |
| `EPD_RESOURCE_NEARBY` | 服务于当前交互活动的相邻内容，需要关联该交互活动。 |
| `EPD_RESOURCE_CONTINUOUS` | 正在运行的持续任务所需资源。 |
| `EPD_RESOURCE_INTERACTIVE` | 当前页面交互所需资源。 |

登记成功后等待准入结果，再使用获准容量。公共组件按实际使用保护正在绘制、解码或播放的数据，统一回收器优先处理低等级且可重建的缓存。共享数据按实际占用计算一次；仍被使用、尚未保存或正在异步处理的数据等待对应工作完成后释放。组件继续维护自身缓存键和淘汰顺序，平台负责跨组件的容量协调。

组件需要填写资源请求的内存池编号时，通过 `epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &backend)` 查询应用 PSRAM 池。查询成功后再使用返回值；普通 SDK SRAM 分配可能跨池回退，因此该接口不将其映射为一个固定内存池。组件已有缓存通过 `epd_resource_cache_open()` 登记回收函数，再用 `epd_resource_cache_update_region()` 上报实际总量、可回收量和重建代价；关闭缓存时调用 `epd_resource_cache_close()`。这些接口由平台解析实际内存池，动态应用无需引用平台私有堆对象。

## 为当前操作申请容量

使用公共图片、阅读、字体或音频组件时，组件已经负责申请工作容量和释放闲置缓存。应用直接调用这些组件。只有应用新增自己的大块计算或解码组件时，才需要通过 `epd_activity_work.h` 为一次实际工作申请容量。

前台页面提交任务时，在页面回调中调用非阻塞入口：

```c
#include "epd_activity_work.h"

epd_activity_work_t *work = epd_activity_work_queue(
    EPD_RESOURCE_INTERACTIVE, 0, psram_peak, sram_peak,
    signal_existing_worker, NULL);
if (!work)
    return false;
/* 将 work 放入组件现有任务队列，随后唤醒 worker。 */
```

`psram_peak` 和 `sram_peak` 是本次新增分配同时存在时的物理峰值，包含分配器开销。已经由调用者持有的输入不重复申报。文件读取即使将正文放在 PSRAM，也需要为文件对象和路径留出 SRAM；可以用 `epd_resource_file_io_peak()` 估算标准文件读取的这一部分。分块处理只申报同时存在的缓冲，不按整个文件大小预留。

`signal_existing_worker` 只唤醒组件已有 worker，例如释放组件的事件量。平台在常驻工作线程调用该函数，回调不操作 LVGL。回调使用静态事件对象；页面指针不能作为会随页面销毁的回调上下文。

worker 通过 `epd_activity_work_ready(work)` 查询准入结果。返回 0 时等待唤醒，返回 -1 时结束本次请求，返回 1 后才进入获准的分配上下文：

```c
epd_activity_guard_t guard;
if (!epd_activity_work_enter(work, &guard)) {
    epd_activity_work_close(work);
    return;
}
/* 调用 epd_app_alloc 等配套分配器，并执行本段计算。 */
epd_activity_work_leave(&guard);
/* 确认所有执行者不再访问 work 后关闭；结果内存由接收者正常释放。 */
epd_activity_work_close(work);
```

分配仍可能因连续空间不足而失败，组件按本次请求的错误路径结束。不要将拒绝变成无限重提交。需要分步扩大容量时，在离开 guard 后调用 `epd_activity_work_prepare()`，并沿用同一任务和唤醒事件。

当前结果已经显示、还需要准备相邻内容时，调用 `epd_activity_work_settle()` 结束当前计算并归还未使用额度，再以 `epd_activity_work_activity(work)` 作为 NEARBY 请求的父活动。父活动保留到当前显示绑定结束；每次预取完成或取消都关闭对应的预取 work。平台依据真实前台页面、已接受任务及父活动判定资格，应用不能仅通过选择 INTERACTIVE 或 CONTINUOUS 为闲置缓存长期保留高优先级。

完整的队列和取消接线可以参照 `modules/books/document/document.c` 的文档任务，以及 `modules/gallery/gallery_decode.c` 的图像任务。后台保存和页面关闭的责任交接见[受管异步操作与冷状态](managed-operations.md)。这些机制不按应用名称授予优先级。
