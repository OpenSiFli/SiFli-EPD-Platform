---
title: "动态应用开发"
---

# 动态应用开发

动态应用是通过 SDK 加载的 ARM 共享模块，页面使用 app_fwk 与 LVGL 9，公共服务通过固件导出的函数调用。应用源码放在平台的 `modules/<id>/`，使用配套 SiFli SDK 的 SCons 和 `ua.BuildLibrary` 编译，再由平台检查模块并生成 `.sap` 安装包。

首次创建应用可先完成[第一个应用](first-application.md)的安装和更新流程。具体函数的参数与所有权见[接口参考](../reference/api-reference.md)，清单字段和安装包布局分别见[应用清单](../reference/app-manifest.md)与[SAP 格式](../reference/sap-format.md)。

## 开发环境

准备平台源码、项目记录的 SiFli SDK 和 Arm GNU 工具链，按[构建说明](../build/building.md#准备环境)激活环境并完成一次平台构建。后续应用构建读取所选固件目录中的 `main.elf`、`rtconfig.h`、`cconfig.h`、`rtua.py` 和 `app-profile.json`。

本页命令在 `SiFli-EPD-Platform` 平台源码根目录执行，其中包含 `modules/`、`project/` 和 `SiFli-SDK/`。

修改应用源码或资源后，只需重新构建该应用。修改平台公共接口或固件配置后，先重新构建平台，再构建使用这些变化的应用。头文件提供函数声明，应用实际可调用的函数以目标固件的 RT-Thread 导出表为准。

## 最小工程与入口

以 `modules/hello/` 为入门示例。工程包含 `app.json`、`SConstruct`、`SConscript`、`rtconfig.py`、C 源码、可选的 `i18n/strings.json` 及资源。`SConstruct` 调用 SDK 构建模块，`SConscript` 描述应用源码、依赖和编译选项；`tools/native_apps.py` 负责统一检查和打包。

下面用新应用 `myapp` 展示首版源码清单，后续代码中的 ID 与文件名沿用这个名称。现有触摸示例的版本和配置见其清单（源码：`modules/hello/app.json`）。

```json
{
  "abi": 1,
  "id": "myapp",
  "version": 1,
  "data_version": 1,
  "name": "我的应用",
  "default_locale": "zh_cn",
  "localized_names": {"zh_cn": "我的应用", "en_us": "My App"},
  "background": false
}
```

当前 `RT_NAME_MAX=8`，动态应用 ID 使用 1～7 个小写英文字母、数字或下划线，不得与内置应用重复；`all` 和 `none` 保留给构建选项。目录名、清单 ID 与程序文件名 `<id>.so` 保持一致。显示名称最多 47 个 UTF-8 字节。

模块包含 `platform/epd_app.h`，导出应用描述和入口：

```c
EPD_APP_DEFINE("myapp");

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext(
        "myapp", page_message, NULL, sizeof(page_data_t));
}
```

`page_message` 是应用定义的页面回调，`page_data_t` 保存该页面的私有状态。框架分配并清零页面内存，回调通过 `gui_app_this_page_memory()` 取得；模块初始化函数用于业务初始化，页面注册放在 `app_main()`。

按下节创建工程后，在平台源码根目录构建：

```powershell
scons -C modules APPS=myapp FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
```

安装文件输出至 `project/build_sf32lb57-epd-gyd7n6_hcpu/app-packages/myapp.sap`。安装方式见下文，配置及预装方式见[构建说明](../build/building.md)。

## 创建新应用

例如创建 ID 为 `myapp` 的新应用，完整复制命令见[第一个应用](first-application.md#复制工程)：

1. 将 `modules/hello/` 的源码、构建脚本、`rtconfig.py`、`exports.map`、翻译表和资源复制到 `modules/myapp/`，不复制 `output/` 等构建产物。
2. 将 `app.json` 的 `id` 改为 `myapp`，设置名称、版本和本地化名称；同步修改 `EPD_APP_DEFINE()`、页面注册及导航中的应用 ID。
3. 将 `SConstruct` 中的模块名称改为 `myapp.so`，在 `SConscript` 中维护实际源码、公共组件、包含目录和应用编译选项。

`SConstruct` 沿用以下入口：

```python
from pathlib import Path
import sys
from SCons.Script import ARGUMENTS
from ua import BuildLibrary
import rtconfig

repo = Path(Dir('#').abspath).parents[1]
firmware = repo / 'project' / ARGUMENTS.get('FIRMWARE', 'build_sf32lb57-epd-gyd7n6_hcpu')
sys.path.insert(0, str(repo / 'tools'))
from native_module import configure
configure(repo, firmware, Dir('#').abspath)
BuildLibrary('myapp.so', 'SConscript', 'app', BSP_ROOT=str(firmware),
             RTT_ROOT=str(repo / 'SiFli-SDK' / 'rtos' / 'rtthread'))
```

`native_module.configure(repo, firmware, module_dir)` 加载固件的 `rtua.py`，准备应用包含路径和多语言配置。编译及链接由 SDK 的 `BuildLibrary` 完成。新应用通过 ID 直接选择，无需修改中心应用名单：

```powershell
scons -C modules APPS=myapp FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
```

生成的模块位于 `modules/myapp/output/myapp.so`，保留调试信息的模块为 `myapp.so.nostrip`，安装文件为所选固件目录下的 `app-packages/myapp.sap`。调试时保存对应固件的 `main.elf`、模块 `.so.nostrip`、源码和配置，操作见[动态应用故障定位](../troubleshooting/dynamic-app-debugging.md)。

应用专用资源放在 `modules/<id>/assets/` 或 `res/`，专用生成脚本放在该应用的 `tools/`。平台根目录 `assets/` 保存平台界面、网页和系统提示音等资源。修改应用的 `assets/icon.svg` 或 `icon.source.json` 后，先按[图标生成](application-resources.md#重新生成平台图标)安装 Node.js 与 `sharp`，再运行 `node tools/generate_app_icons.cjs --apps myapp`。

普通应用的 `assets/` 和 `res/` 会递归合并到包内 `res/`，保留各自目录内的相对路径。同路径且内容相同的文件只保留一份；内容不同、路径仅大小写不同或文件与目录占用同一路径时，构建报错。`assets/icon.png` 单独交给 SDK 转换为包根目录的 `icon.ezip`，同时需要 `assets/icon.LICENSE.txt` 和 `assets/icon.source.json`。天气图片、单词词库和书架附属库继续使用各自的专用打包规则。

## 示例选择

| 示例 | 可参考的实现 |
| --- | --- |
| `modules/hello/` | 中英文页面、语言版本更新、第二页跳转及返回；页面暂停和销毁时处理定时器。 |
| `modules/netdemo/` | 受管网络任务读取 `network_snapshot()`，通过 `epd_service_wait()` 等待网络变化或停止；页面显示网络来源及变化序号，暂停时停止刷新，退出时释放页面定时器。示例不发送 HTTP 请求。 |
| `modules/cfgdemo/` | 通过 `storage_app_path(..., STORAGE_APP_DATA, "settings.bin")` 定位配置，使用 `storage_record_save/load()` 保存和加载带版本、校验值的计数记录；保存成功后更新计数，记录缺失或无效时使用零计数，保存失败显示错误，存储切换时不提交保存。 |
| `modules/ai_demo/` | AI 流式对话、会话持久化、取消和语音朗读，见 [AI 服务接入](../services/ai-services.md)。 |

网络和配置示例需要显式选择：

```powershell
scons -C modules APPS=netdemo,cfgdemo FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
```

`APPS=all`、八应用预装以及需显式选择的应用见[应用选择对照](../build/building.md#应用选择对照)。新增应用用具体 ID 构建，出厂预装由 `PREINSTALL` 指定。

## 接口与头文件

`platform/epd_app.h` 提供应用描述、页面控件、输入和基础字体接口。其他能力按需包含对应头文件，不由这个入口间接引入。

| 应用需要的能力 | 头文件 |
| --- | --- |
| 受管后台任务与摘要 | `app_service.h` |
| 存储状态、卡会话和文件记录 | `storage.h`、`storage_file.h` |
| 网络快照与事件订阅 | `network.h` |
| 时间查询和格式化 | `system_time.h` |
| SRAM／PSRAM 分配 | `epd_memory.h` |
| 应用图片资源 | `platform/app_image.h` |
| 键盘、拼音编辑与输入模式 | `platform/epd_keyboard.h` |
| 通知、媒体会话与声音设置 | `platform/epd_system.h`、`platform/epd_sound.h` |
| 语言、锁屏和设置查询 | `platform/epd_locale.h`、`platform/epd_lock.h`、`ui_settings.h` |
| 自建字体和派生字形缓存 | `platform/epd_app_font.h` |
| 阅读视图与后台准备 | `modules/common/reader/reader.h`，接入方式见[公共组件](common-components.md#应用阅读组件) |
| 附属动态库 | `platform/epd_app_library.h` |
| 文件处理者发现与内容交接 | `platform/app_content.h` |

对应的 `*_internal.h` 供常驻平台使用，包含初始化、主循环推进、模块引用维护、存储交接以及网卡注册等声明。应用通过公开服务提交工作，不接管这些流程。SDK 的 app_fwk、LVGL 和 RT-Thread 接口继续使用 SDK 头文件；构建时由导入检查确认目标固件实际导出了所调用的函数。

## 页面生命周期

| 消息或接口 | 应用的处理 |
| --- | --- |
| `GUI_APP_MSG_ONSTART` | 创建控件、页面定时器及页面专属资源。 |
| `GUI_APP_MSG_ONRESUME` | 读取业务快照，检查语言、尺度和内容区域，恢复前台更新。 |
| `GUI_APP_MSG_ONPAUSE` | 暂停页面定时器和前台订阅，保存需要恢复的状态。 |
| `GUI_APP_MSG_ONSTOP` | 删除自建定时器、解除订阅、释放页面自有资源。框架释放 screen 和页面内存。 |
| `gui_app_create_page_for_app_ext()` | 创建属于本应用的子页。 |
| `gui_app_goback()`、`gui_app_goback_to_page()` | 返回上一页或本应用已有页面。 |
| `gui_app_self_exit()` | 退出本应用页面。 |

页面回调和 LVGL 操作在 UI 线程运行。后台线程复制业务状态供页面读取，不保留页面对象、页面内存或临时 Intent 指针。触摸回调提交导航请求后返回，让框架完成页面切换。

暂停页面仍保留 screen 和页面内存；框架因运行应用数量上限而停止页面后，重新进入需要从业务状态恢复。详细停止顺序见[后台生命周期](application-lifecycle.md)。

## 界面、输入和字体

通过 `epd_app_header()` 声明普通页面，通过 `epd_app_set_immersive()` 声明沉浸页面。`epd_ui_content_area()` 返回扣除系统栏后的内容区域；导航显隐和界面尺度都可能改变区域尺寸。

`epd_ui_px()` 换算设计尺寸，`epd_ui_touch()` 保持至少 56 像素的触摸区域；屏幕尺寸、图片原始尺寸和阅读字号使用原值。应用检查 `epd_ui_scale_revision()`，在页面恢复或版本变化时重排，并保留输入、选择及业务状态。五档尺度为 90%、95%、100%、110%、120%。

`epd_app_font_role()` 提供当前尺度的角色字体。`epd_app_font_style()` 提供按需样式字体。系统字体共享字形缓存；应用在 UI 线程调用字体和布局接口。多语言源表、格式化和语言版本检查见[多语言开发](multilingual.md)。

KEY1 由系统处理锁屏与电源菜单。前台页面使用 `epd_app_input_listen(owner, callback, data)` 监听 KEY2、KEY3，并用 `epd_app_input_unlisten(owner)` 解除；owner 删除时自动注销。回调在 UI 线程执行，长按三秒触发一次，长按后松开不再触发短按。

公共按钮自动加入受管输入组。自定义控件可用 `epd_app_input_group()` 获取组；弹窗建立独立输入组并使用 `epd_app_input_back()` 指定返回按钮。组随 owner 删除，应用不重复释放。完整手势和媒体接口见[触摸界面](touch-ui.md)。

界面使用静态白底，按业务数据变化更新控件。页面切换和同页正文更新均沿用全刷周期；仅已登记状态栏的局部变化不累计周期。显式全刷才重置周期。刷新模式、波形和面板供电由平台管理。

## 受管后台任务

模块通过 `epd_background_t` 描述受管后台任务。清单声明 `background: true` 并导出 `epd_app_background` 时，平台在应用发现后自动启动该服务；`background: false` 的应用也可以按需通过 `app_service_start()` 启动任务。描述符类型在 `src/services/app_service.h` 定义：

| 成员 | 契约 |
| --- | --- |
| `run` | 平台创建的工作线程入口，停止后关闭请求和文件并返回。 |
| `stack_size`、`priority` | 栈为 2～32 KiB，优先级为 20 到 `RT_THREAD_PRIORITY_MAX-1`。 |
| `network` | 启用统一网络事件订阅。 |
| `ui_process` | UI 线程中的有限步骤；停止阶段完成页面及 UI 资源清理后返回 `true`。 |
| `flush` | 非阻塞保存屏障；保存尚未完成或失败时返回 `false`。 |
| `command` | 可选命令入口，调用期间平台保持模块引用。 |

`run` 和 `ui_process` 至少提供一个。工作线程使用 `epd_service_wait()` 等待，检查 `epd_service_cancelled()`；网络事件为 `EPD_SERVICE_NETWORK`，停止事件为 `EPD_SERVICE_STOP`。需要 UI 分步处理时通过 `epd_service_ui_request()` 提交期限。

页面退出只释放页面引用，后台任务继续持有模块。更新、卸载、拔卡和关机等待任务、在途调用及 UI 清理完成，再释放服务引用。应用自己的子线程和回调必须在该过程完成前退出。

接收持续任务前调用 `epd_service_sleep_acquire()`，成功后才启动工作。申请对同一服务幂等；多个任务由服务汇总，全部工作及清理结束后调用 `epd_service_sleep_release()`。该锁阻止墙纸系统休眠，设备驱动仍负责自己的 SDK PM 约束。见[电源管理](../services/power-management.md)。

## 安装包与存储

打包工具生成最终 `app.json`、`files.sha256` 和 `<id>.so`，收集资源及可选私有库，再生成单文件 `.sap` 安装包。最终清单还包含包格式、ELF 装载跨度和文件索引摘要。源码清单不能直接作为安装包使用。

| 内容 | 内部存储 | TF 卡 |
| --- | --- | --- |
| 程序及资源 | `/flash/apps/<id>/` | `/sdcard/.epd/apps/<id>/` |
| 业务数据 | `/flash/data/<id>/` | `/sdcard/.epd/data/<id>/` |
| 可重建缓存 | `/flash/cache/<id>/` | `/sdcard/.epd/cache/<id>/` |

可以在设备网页上传 `.sap` 直接安装，也可以将 `.sap` 放在 TF 卡任意可浏览目录，在设备“设置 → 应用管理 → 应用安装”中浏览并选择文件。安装源的位置与安装目标分别选择。

首次安装选择内部存储或 TF 卡；更新沿用原卷，要求更高的 `version`，已有非零 `data_version` 必须与新包一致。卸载删除程序和缓存，保留业务数据；重装后应用自行校验数据格式。

安装器接收 `.sap` 文件，在应用退出后检查包信息，将文件解包到目标卷的 `.app-install`。解包时计算并核对每个文件的摘要；后续暂存包检查复用这些结果，继续检查清单、ABI 和必需文件，通过后用目录改名发布。写入过程处理短写、长度检查和 `fsync` 错误。

更新时先记录应用 ID，再将旧目录改名为 `.app-old`，最后发布新目录。启动或插卡时，若日志有效、旧目录仍在且正式安装目录缺失，安装器恢复旧目录；正式目录已存在时清理旧目录。未完成的暂存包会被清理，不继续复制。完整流程见[应用生命周期](application-lifecycle.md#更新事务)。

包内相对路径最多 159 个 UTF-8 字节、八层，完整路径小于 256 字节。文件摘要流式计算。`.sap` 归档最大为 1 GiB，最多包含 8192 条文件记录，其中两条用于 `app.json` 和 `files.sha256`。实际可安装容量取决于目标卷空间；当前内部文件系统为 5 MiB。

应用使用 `storage_app_path()` 查询同一安装卷上的程序、数据和缓存路径，使用 `storage_record_save/load()` 保存有版本和 CRC 的记录。短时文件操作在 `storage_lock()/storage_unlock()` 内检查 `storage_path_available()`；不得持锁等待 UI。

改变安装位置时先卸载再安装。目标卷没有数据、另一卷仅有遗留数据且没有同名安装时，安装器复制、核对并发布数据，成功后清理源数据。目标已有数据时不合并或覆盖。缓存不跨卷迁移。

## TF 卡会话

任务开始时保存卡会话，后续访问检查会话有效性。拔卡立即失效旧会话并禁止新访问，平台停止所有动态应用，因为内部安装的应用也可能读取 TF 文件。任务关闭句柄、页面和后台释放后才卸载文件系统。

等待释放超过 35 秒时继续等待原使用者，保持 TF 禁用及模块引用；检测到稳定的重新插卡后，可以发起一次新的释放请求。释放确认后仍有句柄、卸载失败或 SDK 插拔请求超时，SD 组件进入持续故障隔离，须重启恢复；普通探卡或挂载错误可通过重新插拔重试。详见SD 卡组件源码（源码：`components/sdcard/`）。

插卡挂载后，设备询问是否加载卡内应用。确认后完成数据准备、发布入口并启动声明的后台服务；取消仍可访问普通文件。新卡获得新会话，旧请求不能改用新会话继续写入。

## 私有库、设置与内容交接

私有库放在包的 `codecs/<name>.so`，用 `tools/package_app.py --library` 加入清单。库名为 1～7 个字符，SDK 按文件名识别已加载模块。UI 线程调用 `epd_app_library_open()`、`epd_app_library_symbol()` 取得接口，worker 使用接口执行任务；先结束任务、释放库结果，再在 UI 线程调用 `epd_app_library_close()`。

清单声明 `settings: true` 后，系统通过 `page=settings` Intent 打开应用设置。声明 `lockscreen: true` 后，平台通过 `lockscreen=1` 启动锁屏入口；应用长按解锁完成时调用 `epd_lock_unlock()`。运行数量、存储交接及回退条件见[触摸界面](touch-ui.md)。

文件处理者通过清单声明类型，接收路径及存储会话后重新校验，见[内容交接](content-handlers.md)。简短摘要使用 `epd_service_publish_summary()` 发布副本；消费者用 `epd_app_summary_revision()`、`epd_app_summary_read()` 读取。摘要容量含结尾零共 96 字节，服务停止后不可读取。

## 公共服务

- [网络](../services/wifi.md)：通过 `network_snapshot()` 取得路由与 generation，在途请求检查网络变化。
- [AI](../services/ai-services.md)：在受管 worker 中调用 `epd_ai_perform()`，由应用持有请求、历史与取消令牌。
- [语音](../services/speech-services.md)：使用系统输入面板，或用 `epd_speech_start()` 创建应用会话，消费并释放事件。
- 计算接口（源码：`src/platform/epd_compute.h`）：查询能力和 ready 状态，管理跨核会话及本地回退。
- 声音接口 `platform/epd_sound.h`：快照读取只访问内存；音量修改在线程上下文提交，滑块松手时提交一次，失败后按快照恢复显示。

## 二进制兼容与公共符号

`EPD_APP_DEFINE()` 定义应用 ID 和 `EPD_APP_ABI`。ABI 表示函数调用和公共数据结构的二进制约定；清单与模块描述的 ABI 必须兼容，所有导入符号必须由目标固件导出。函数参数、公共结构布局或影响布局的配置发生不兼容变化时递增 ABI，再使用更新后的平台源码与配套固件重建应用。新增接口保留旧契约时，旧应用可以继续使用。

`build_id` 记录构建配置，不要求应用与运行固件的标识相等。应用构建使用所选固件的配置和 `app-profile.json`；接口兼容要求符号、参数、结构布局及相关编译配置同时满足目标固件的 ABI 契约。

SDK 生成 `.so` 后，平台在发布 `.sap` 前检查 ARM 32 位小端共享 ELF 的可加载段，并将导入符号与 `main.elf` 的 RT-Thread 导出表逐项核对。主应用还必须导出 `app_main` 和 `epd_app_info`；附属动态库检查格式和导入，不要求应用入口。任一检查失败时不发布新包。设备安装和加载时继续执行清单、摘要及 ABI 校验。

公共导出主要位于 `src/ui/platform/epd_app.c`、`app_runtime.c` 及 SDK 的 RTM 导出表。动态模块与固件共享地址空间，清单目录与受管任务不提供进程隔离。模块使用兼容的 Arm GNU 工具链、SDK 和 LVGL 配置构建。

大缓冲使用 `epd_app_alloc/epd_app_realloc` 指定内存池，并以 `epd_app_free` 释放；显式 PSRAM 请求不回退 SRAM。页面停止时释放页面自有对象；后台任务及私有库结束后释放对应模块引用；字体和图像在控件与绘制引用解除后释放。
