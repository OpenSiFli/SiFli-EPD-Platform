# 标准受管应用示例

这个完整动态应用展示公共图片、自定义异步任务、容量申请和调整、取消收尾，以及计数状态保存。默认 ID 为 `memdemo`，没有平台应用名单或预装身份依赖。安装后点击 **Start calculation**，应用先处理 1 KiB，再扩大到 4 KiB；成功校验和为 **522240**。**Cancel calculation** 或离开页面会取消任务。

## 构建为普通应用

先完成平台固件构建并激活 SDK。在平台源码根目录执行：

```powershell
. ./SiFli-SDK/export.ps1
Copy-Item docs/examples/managed_app modules/memdemo -Recurse
scons -C modules APPS=memdemo -j4
```

目标目录应尚不存在。产物为 `project/build_sf32lb57-epd-gyd7n6_hcpu/app-packages/memdemo.sap`。可以按普通应用安装到 Flash 或 TF 卡。入口图标由通用构建器转换为 EZIP，页面通过 `storage_app_path(..., STORAGE_APP_CODE, "icon.ezip")` 查找安装后的图像。

改成其他 ID 时，只需修改 `app.json` 的 `id`、`app_id.h` 的 `DEMO_APP_ID`，并使用相同名称的模块目录。ID 为 1～7 个小写字母、数字或下划线。SConstruct 从清单读取输出模块名。

也可只构建隔离副本，不创建 `modules/` 目录：

```powershell
python docs/examples/managed_app/build.py --output C:/temp/managed-example-build --id memdemo --id mx731
```

输出目录应尚不存在。脚本复制现有固件的配置、导出符号输入和 profile，使用相同 `BuildLibrary`、导入检查器及 SAP 打包器生成两个应用；不构建固件。`--firmware` 可指定配套固件目录，`result.json` 记录源码哈希、模块大小及包哈希。

## 代码与生命周期

| 文件 | 接入内容 |
| --- | --- |
| `app_main.c` | 普通页面入口、公共图片显示与释放、任务启动和取消、完成维护入口。 |
| `managed_job.c/.h` | accepted TASK、受管 operation、容量 acquire/resize/release、失败及取消结算。 |
| `demo_state.c/.h` | 从 hello 模板沿用的计数 checkpoint；成功业务完成后记录计数。 |

图片在 ONRESUME 调用 `epd_app_image_set()`，在 ONPAUSE/ONSTOP 调用 `epd_app_image_release()`；控件删除也由公共组件处理。相同来源的控件复用底层像素，各自保留使用引用。应用不实现图片预算或缓存淘汰器。

自定义任务通过 `epd_app_task_group_create()` 和 `epd_app_task_begin()` 接受工作，选择 TASKS，任务结束后保留 STATE。容量来自当前 TASK 上下文中的 `epd_resource_context_begin()`。返回 ACCEPTED 只表示登记成功，维护入口必须等到 ADMITTED 且生效修订号匹配，才能提交计算 operation。

首次 operation 完成后，`epd_resource_context_update()` 申请旧缓冲与新缓冲共存的物理峰值。扩容先分配新缓冲，成功后再释放旧缓冲；准入拒绝或实际分配失败都不会提前破坏旧内容。两段计算各有 operation，但属于同一个 accepted TASK；operation 的执行范围使用其子 TASK，避免单段完成撤回父任务的有效容量。工作线程每次最多处理 128 字节，通过同一 operation 的 yield 继续计算。

取消只请求 operation 停止，不立即释放缓冲。维护入口等到 FINISHED，再释放缓冲并调用 `epd_resource_activity_end()`；待 ENDED 后 forget 活动，关闭操作范围，最后结束 TASK 并释放任务组。所有收尾都不申请新的业务缓冲。页面关闭后，应用入口创建的维护 timer 继续完成这些步骤，且不再访问页面对象；结算完成后 timer 自行删除。50 ms timer 用于观察状态，没有重复提交容量申请或通用失败重试。

成功计数使用 hello 的 checkpoint 格式和恢复流程。普通图片使用者不需要实现 checkpoint；这里的 checkpoint 只服务于示例自己的计数业务。

## 针对性主机验证

```powershell
python tests/managed_example/run.py
python tests/managed_example/run.py --negative-old-revision
```

主机夹具编译示例任务、真实 operation、resource/runtime、background 和内存管理核心，并保留生产 operation 的 task fact 同步逻辑。覆盖完成、排队前/排队后/运行中/扩容中取消、容量拒绝、扩容拒绝、实际分配失败及未准入修订不可执行。

图片用例执行示例原有 ONRESUME/ONPAUSE/ONSTOP 分支及完整公共 `app_image.c`，验证两个控件的共享与独立引用、页面关闭取消任务，以及 Flash/TF 安装路径。LVGL 绘制、图像解码、存储和 RTOS 调度使用主机端口；ARM 构建另外编译完整页面代码。两个应用 ID 在两种 tick 频率下采用相同断言，最终验证任务、活动、物理占用、未用额度及应用 root 引用均结清。反证测试仅修改临时编译副本，证明测试能检出未准入扩容被提前执行。
