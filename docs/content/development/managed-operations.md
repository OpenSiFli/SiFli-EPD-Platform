---
title: "受管异步操作与冷状态"
# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
---

# 受管异步操作与冷状态

异步操作通过 `epd_operation.h` 提交。平台在声明的执行线程运行计算，在声明的接收线程交付结果，在独立的清理范围结束收尾。`epd_cold_state.h` 接受已经序列化的有限字节，使用现有存储组件异步保存和加载。应用不创建平台调度线程，也不调用平台初始化或 dispatch。

## 动态模块可调用的接口

| 接口 | 用途 |
| --- | --- |
| `epd_operation_scope_open/get/close/release` | 创建和查询操作范围；关闭后拒绝新操作；操作全部收尾后释放范围。 |
| `epd_operation_runtime_executors` | 取得平台 worker、GUI 接收线程和 worker 清理线程。 |
| `epd_operation_submit/cancel/yield/complete` | 提交、请求取消、分步让出执行机会和确认实际执行结束。 |
| `epd_operation_get/retry_delivery/release` | 查询唯一结果记录、在接收方恢复容量后重试投递、释放观察句柄。 |
| `epd_operation_owner_drained` | 只读检查指定 owner 及其后代的 operation 是否已收尾。其他服务和硬件需要各自确认。 |
| `epd_cold_state_put/load/get/copy` | 提交序列化状态、异步加载、查询状态和复制有限结果。 |
| `epd_cold_state_retry/release/abandon` | 显式重试；释放成功记录或失败的加载；主动放弃允许丢弃的可选保存。 |
| `epd_cold_state_take/blob_release` | 将失败保存的字节和 owner 引用交给明确的恢复责任人，在保存或再次交接后释放。 |
| `epd_checkpoint_register/register_edits/changed/get` | 注册应用模型的捕获、恢复和拆除回调；需要合并加载期间的新编辑时使用 register_edits；编辑前登记脏修订；查询保存及恢复结果。 |
| `epd_checkpoint_restore/retry/unregister` | 干净模型重新加载；显式重试失败操作；无必需脏数据时请求拆除 provider。 |
| `epd_app_background_subscribe/unsubscribe` | 在当前应用的有效页面或受管 worker 范围订阅后台状态快照，并解除订阅。 |

以上 31 个接口通过 `RTM_EXPORT` 导出。`epd_operation_runtime_init/register/ui_pump/worker_pump/wake/next_timeout_ticks/close_owner/owner_drained`、`epd_operation_poll/next_timeout/dispatch/close_owner`、`epd_cold_state_init/dispatch` 和 `epd_cold_runtime_*` 仅供常驻平台调用，不导出给动态模块。平台扩展的注册函数和生命周期事实发布函数也由常驻代码调用。

## 一个可以直接编译的组件示例

示例源码位于 [operation_example.c](../../examples/managed_operation/operation_example.c)，配套类型位于 [operation_example.h](../../examples/managed_operation/operation_example.h)。将两者作为应用组件编译；源文件只调用上表中的公共 operation/coldstate 接口及 RT tick 查询。

`operation_example_start()` 把最多 256 字节的输入复制到调用方提供的固定对象，每次计算最多 32 字节，然后通过 `yield()` 继续同一个已接受的操作。示例不在 worker 中访问 LVGL 对象，也不在清理时申请内存。

任务 owner 来自真实的受管业务。应用在自己的有效上下文中使用 `app_background.h`：

```c
/* 将这些字段放入能覆盖整个任务寿命的应用对象。 */
epd_app_task_group_t group;
epd_memory_owner_t task;
operation_example_t *job = &application->job; /* 初始值为全零。 */

if (epd_app_task_group_create(&group) != EPD_MEMORY_OK)
    return false;
if (epd_app_task_begin(group, &task) != EPD_MEMORY_OK) {
    epd_app_task_group_release(group);
    return false;
}
if (!operation_example_start(job, task, instance_generation,
                             input, input_size, timeout_ticks)) {
    epd_app_task_end(task);
    epd_app_task_group_release(group);
    return false;
}
```

异步提交函数返回后，`job`、任务句柄及模块代码必须继续有效；应用将它们放在受管应用对象中。需要离开页面后继续该业务时，通过公开的后台策略接口选择 `EPD_APP_BG_TASKS`，并将 `group` 作为业务集合传入。

在现有的应用完成事件或维护入口中调用 `operation_example_finish()`。返回 `true` 表示 worker、接收回调和清理已经结束，此时才能读取最终结果、结束 `task`、释放 `group` 并销毁 `job`。页面关闭后，这个收尾入口仍需覆盖已接受任务的寿命。调用 `operation_example_cancel()` 只发出取消请求，之后仍按同一结束流程等待；不要在取消函数返回后立即释放输入对象或模块代码。

同一 TASK/SERVICE owner 可以承载多个 operation。平台按 owner token 汇总首次执行意图和仍在执行的操作，在共享 worker 发布任务事实；scope 的业务代次仅用于结果校验。其他执行线程在首次运行前等待一次事件握手。异步等待和 `yield()` 保留任务资格；取消后仍在访问数据的操作也继续计入，直到 `complete()` 已确认结束且 `run` 回调已经返回。系统 owner 和普通 APP/PAGE owner不会因为提交了操作而获得持续任务资格。

## 页面状态通知与任务收尾

页面需要显示后台策略、保存结果或实际留存状态时，通过 `services/app_background.h` 的 `epd_app_background_subscribe()` 接收完整快照。平台复用共享 operation worker 和 GUI 接收通道；普通页面只提供通知函数及页面上下文。注册、退订和关闭的最小示例见[后台状态订阅](application-memory.md#后台状态订阅)。

通知按应用账户和实例代次校验，可合并为最新状态，并在 IRQ 临界区外进入原 scope 执行。GUI 页面通知可更新自己的控件；TASK/SERVICE 通知在共享 worker 上执行。空闲订阅不占用在途 operation，不产生持续业务任务。页面退出时在原执行器退订，随后释放页面对象；生命周期关闭会使排队的旧通知失效，并等待已经开始的回调返回。

后台快照描述整个应用的策略和实际状态。某次计算的结果对象、operation 观察句柄、资源活动和 TASK 仍按各自完成协议收尾。[标准受管应用示例](../../examples/managed_app/README.md) 中的 `managed_job_poll()` 负责这些步骤，页面关闭后仍需由应用寿命的维护入口推进。给示例增加后台状态显示时，可在页面对象中增加独立订阅，并在 ONSTART／ONSTOP 注册和退订，保留原有任务收尾入口。

## 结果所有权与独立清理范围

成功提交后，`user` 的存活责任交给 operation，直到 `dispose_user()` 返回。提交失败时，所有权仍属于调用者。`complete()` 返回 `true` 时接管结果；重复完成返回 `false`，第二个结果仍由调用者负责。

`receive()` 返回 `true` 才取得结果所有权。接收队列满时返回 `false`，结果留在 operation 的固定槽位中；接收方出现空位后调用 `retry_delivery()`。平台不靠反复轮询接收队列重试。

`release(operation)` 只放弃观察句柄，不取消执行，也不提前释放结果。执行中取消或超时以后，执行者仍须在所有访问结束后调用 `complete()`。过时或已关闭范围的结果在声明的清理线程销毁。已经开始的接收回调属于在途访问，关闭范围后仍须等待其返回。

清理范围必须与业务操作范围不同。需要在父 owner 关闭后分配存储工作区的组件，应在关闭前取得核心 CLEANUP scope 下的 LIVE credit，并将该 credit 作为清理执行 token。单纯进入一个已关闭的 owner 不授予分配额度。示例只在原 TASK owner 下建立两个不同的 operation scope，因为示例的清理完全不分配内存。

## 小型冷状态的提交与恢复

`operation_example_save_position()` 将来源版本和位置编码为 8 个小端字节。`spec.key` 是稳定业务身份，例如 `myapp/document-position`；`format_version` 声明数据格式，`revision` 是同一记录单调递增的数据修订号，`instance_generation` 是本次请求的实例代次。revision 不循环复用；需要新修订纪元时使用新 key。

```c
epd_cold_state_spec_t spec = {
    .key = "myapp/document-position",
    .cleanup_owner = accepted_cleanup_credit,
    .instance_generation = instance_generation,
    .format_version = 1,
    .revision = position_revision,
    .optional = false
};
epd_cold_state_t state;
bool accepted = operation_example_save_position(&spec, source_version,
                                                 position, &state);
```

提交只复制有限字节，不在提交线程执行文件 I/O。成功提交不等于保存成功。使用 `get()` 区分排队、工作中、成功和失败；工作结束会唤醒现有 GUI 主循环。必须保存的数据失败后，槽位继续持有字节和清理 owner，`release()` 和 `abandon()` 都不会丢掉它。存储恢复或明确的重试事件调用 `retry()`，即使原业务范围已经关闭也保留同一保存责任。

已持久化的新修订不会被旧修订覆盖。相同修订和相同内容的重试直接确认成功；相同修订对应不同内容时报告冲突。状态采用现有存储后端的临时文件、同步写入和原子替换流程发布，不将旧对象的指针写入 Flash。

加载时使用新实例的代次调用 `epd_cold_state_load()`。完成后，`operation_example_restore_position()` 同时检查加载状态、请求代次、精确字节长度和来源版本；检查失败不会修改输出位置。恢复者只在验证成功后重新创建对象或句柄。取消本次恢复时，等待加载结束并释放本次加载结果；已持久化的原记录继续有效。

不要为已有业务数据库再存一份完整副本。settings 继续使用原 `settings.bin` 格式，经 operation 进入共享 worker 保存；冷状态容器适合必要的少量恢复字段。

## 常驻后台生命周期的交接

应用通过 `epd_cold_runtime.h` 注册一个 checkpoint provider。provider 的模型具有应用寿命；页面的 `ONSTOP` 可以销毁页面，capture 仍能从模型复制有限字节。模型、回调代码及 `user` 保持有效，直到 GUI 线程的 detach 回调返回。服务停止时会销毁的对象不适合作为 provider 的 `user`。

```c
epd_checkpoint_spec_t spec = {
    .key = "position", .format_version = 1, .source_version = document_version,
    .maximum_bytes = 8, .optional = false, .user = model,
    .capture = capture_position, .restore = restore_position, .detach = detach_model
};
if (!epd_checkpoint_register(&spec, &model->checkpoint))
    return false;
epd_app_background_policy_t policy = {EPD_APP_BG_STATE, 0, EPD_APP_BG_RELEASE};
if (epd_app_background_set(&policy, NULL) != EPD_MEMORY_OK) {
    epd_checkpoint_unregister(model->checkpoint);
    return false;
}
/* 每次编辑：登记成功后才修改模型。 */
if (epd_checkpoint_changed(model->checkpoint))
    model->position = new_position;
```

注册自动提交异步恢复。平台将应用身份与相对 key 拼接，只用于存储查找。恢复同时检查格式、来源版本和新实例 token；旧实例仍注册同一身份时，新注册会被拒绝。恢复期间的新编辑优先，平台读取旧记录的修订基线，但不会把旧值应用到新模型。restore 回调先验证和重建，成功后再发布对象；失败时保留加载字节。显式调用 `epd_checkpoint_retry()` 才会再次尝试。保存失败同样保留字节和责任，关闭请求不能丢掉必需数据。

生产示例 `modules/hello/hello_state.c` 使用该协议保存 4 字节计数。打开应用保持默认 RELEASE，修改计数时才选择 STATE。`hello.c` 的页面只读取模块模型，关闭页面不影响 checkpoint；新实例恢复完成后，页面既有刷新入口更新计数。复制这个应用即可复用 provider 的完整调用方式。

后台策略和实际状态是两个不同的事实。选择 STATE 后，内存足够时保留模型；压力回收或显式关闭需要卸载时，常驻 loader 按以下顺序完成交接：

1. 在 GUI 线程调用 `app_background_prepare_close(root, &terminal)`。后台管理先停止服务接收新工作并等待实际结束，再请求 `epd_cold_runtime_checkpoint(root, policy_revision, dirty_revision)`。
2. 共享 GUI pump 捕获模型，串行 worker 保存。非 OK 时保留模块、模型及重试入口；主循环继续推进。保存中的新编辑、前台重开或策略变更使旧结果无法通过关闭门槛。干净且已保存的模型仅策略变化时复用已有记录。
3. prepare 返回 OK 表示当前关闭门槛已满足，并且后台已封闭新编辑。必须保存的数据需同代保存成功；可选数据仅在当前修订的保存因 I/O 或容量不足失败、且获准丢弃时允许按 RELEASED 退出。丢弃路径不推进 `saved_revision`，失败结果保留在快照和后台状态通知中。调用 `epd_cold_runtime_close_owner(root)`，继续 pump，直到 `epd_cold_runtime_owner_busy(root)` 返回 false。两个接口均不内联执行模块回调；detach 只在 GUI 执行，且返回前 busy 始终为真。
4. 随后关闭并排空 operation/resource。它们的关闭入口会关闭核心 owner，因此必须晚于 cold detach。调用模块 cleanup，清理 LVGL 资源，再检查所有实际访问及依赖已经结束。
5. 使用当前策略修订调用 `app_background_report(root, revision, terminal)`，结束后台绑定，核对最终模块引用，再执行 `dlclose`。同代 checkpoint 已持久化的 STATE 对应终态 COLD；获准丢弃的可选失败保存对应 RELEASED。停止及终态通知合并后仍保留该保存失败原因。
6. 再次打开时创建新 root，绑定后台身份，再注册新的 provider。注册自动加载旧字节，成功后在 GUI 重建本次实例。

没有 provider 的 STATE 在内存充足时保持 RETAINED。压力回收时，平台等待持续任务结束、业务服务完成保存并停止，确认当前修订已保存后卸载实例，终态为 RELEASED。下一次打开通过应用正常启动流程读取已有业务存储。保存失败或仍有脏数据时保留实例；这条路径不生成额外的冷状态副本。

这些后台报告函数属于常驻生命周期接线，不是普通应用自行确认卸载的接口。冷状态的 `SAVED` 只表示字节已持久化，不表示页面、硬件访问或动态模块已经退出。

## RELEASE 后完成已有脏数据保存

服务的普通业务通过 `epd_service_reclaim_hold()` 接受任务。显式 RELEASE 关闭普通任务准入后，已有脏数据的保存使用 `epd_service_cleanup_hold()` 和 `epd_service_cleanup_release()`。平台在该服务的 cleanup owner 内调用 `flush`，回调取得清理授权后只发布保存请求并唤醒 worker。清理授权同时保护服务、模块代码和必要的休眠阻止，不创建后台 TASKS。

worker 使用 `epd_service_owner_acquire(service, true)` 取得清理 owner，并在执行编码及存储前进入该 owner；离开上下文后交还清理授权。`modules/common/app_cleanup.h` 提供该上下文交接，`app_work.h` 合并同一业务模型的普通任务和清理责任。调用这些 helper 时使用业务自己的互斥锁。

保存失败时保留原模型及 dirty 状态，交还本次授权，由后续 `flush` 重新申请并唤醒。进入清理上下文失败也遵循同样的交还规则；已排队的保存仍需保留并能再次唤醒。平台限制失败重试间隔为 500 毫秒至 5 秒，成功保存的确认可以立即执行。新增数据修改调用 `epd_service_mark_dirty()`，同一失败修订的重试不重复标记新修订。只有保存结果对应当前修订、所有实际访问和清理授权均已结束，平台才允许停止和卸载服务。

notes、AI 会话、音乐队列、日历关闭草稿和 words 词库修改沿用各自原有存储格式，并使用该交接流程完成退出保存。

持续业务的 worker 使用 `epd_service_background_cancelled(service)` 查询后台显式 RELEASE/STATE 是否要求结束当前业务。策略变更、前后台切换以及显式 TASKS 结束后的策略转换都会唤醒现有服务；worker 按最新状态处理，前台及默认后台留存不会触发这项协作停止。该查询不会最终停止服务，也不封闭必要保存。music 先停止播放和自动续曲，再排空已接受的数据修改并保存；写入失败继续保留队列模型，由后续 cleanup 授权重试。媒体命令的新准入被拒绝时，已接受的保存仍会执行并交还本次清理授权。

## 容量和线程约定

operation 使用 32 个操作槽位和 64 个范围槽位；coldstate 使用 8 个作业槽位，每份状态最多 4096 字节，持久化记录最多 16 个固定文件。checkpoint 使用 16 个 provider 槽位，其业务字节上限为 4084，另外 12 字节保存来源及长度封套。管理数组和 GUI 捕获缓冲位于保留型 PSRAM 段，并在组件冷启动时显式清零。原子替换暂存文件由存储后端有界管理。容量不足时拒绝新的提交，已接受工作的取消和收尾继续使用原有槽位。

平台只启动一个共享 worker。扩展 deadline 及 `rt_event_recv()` 使用 RT tick；来源为毫秒的扩展先向上取整转换。无工作和截止时间时永久等待事件。GUI 定期执行平台 pump；回调线程身份和 owner 上下文在运行前校验。全局发布临界区只更新有限元数据，业务回调和文件 I/O 都在临界区之外执行。
