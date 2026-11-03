---
title: "扩展平台"
---

# 扩展平台

动态应用调用公共能力；平台负责这些能力的实现、资源协调和固件导出。扩展前先确认需求是应用自己的逻辑、可复用源码组件，还是必须由常驻固件统一管理的服务。

## 新增公共接口

以现有 `epd_app_*` 接口为例，修改需要经过以下位置：

1. 在对应公共头文件声明参数、返回值、调用线程及资源所有权。通用 UI 接口位于 `src/ui/platform/`，服务和计算接口分别在 `src/services/`、`src/platform/`。
2. 在所属实现文件完成逻辑，内部状态和调度入口留在内部头文件；动态应用使用公开并已导出的函数。
3. 通过 SDK `RTM_EXPORT(function)` 加入固件导出表。现有公共导出可参考 源码：`src/ui/platform/epd_app.c`，其他能力在各自实现文件导出。
4. 核对 源码：`tools/app_profile.py` 中参与应用构建配置的输入；新接口所属头文件或配置需要进入相应指纹。生成的 `app-profile.json` 由构建维护。
5. 重建主固件，再构建使用该接口的主应用及相关私有库，检查导入结果，并运行对应测试。

接口导入检查由 源码：`tools/check_app_imports.py` 读取实际固件 RTM 导出表完成。C 编译成功只证明声明可见，导入检查才能确认目标固件提供符号。公共接口使用方式同步加入[接口参考](../reference/api-reference.md)和对应主题指南。

应用自己的协议解析等能力可以放入 `modules/common/`，由应用 `SConscript` 编入模块。此类组件无需在固件增加同名导出；现有例子见[公共组件](../development/common-components.md)。

## 新板型与显示适配

板级引脚、触摸、前光、音频和无线配置位于 源码：`boards/sf32lb57-epd-gyd7n6/`。项目级内存分区位于 源码：`project/sf32lb57-epd-gyd7n6_hcpu/ptab.json`。新增板型时为其建立相应 board 配置及项目配置，再用 `--board` 和 `--board_search_path` 选择；配置合成顺序见[构建配置](../build/build-configuration.md)。

当前面板实现位于 源码：`src/boards/epd_e0470a03_57x/`。面板尺寸、刷新分块、温度及波形解析与硬件一起工作。替换面板时，需要配套调整驱动、时序、波形文件、显示尺寸及波形占用的 Flash 分区。

生成的下载脚本引用面板目录下的 `waveform/epd_waveform.bin`，当前烧录地址为 `0x12AC0000`。更改分区表后，资源地址、链接布局和下载脚本一起重新生成。应用通过 `epd_app_request_full_refresh()` 请求全刷，面板供电和波形由驱动管理。

## HCPU 与 ACPU

HCPU 负责 UI、应用和服务，ACPU 执行配置允许的计算任务。平台构建将 ACPU 作为子工程生成，并使用同一项目分区表。两核产物、协议和配置一起发布，构建入口见[BUILDING.md](../build/building.md)。

源码：`project/compute-components.json` 列出参与构建的计算组件。每个组件的 `compute.json` 描述名称、接口版本、公共头文件、两核源码和远端操作表；PCM 组件（源码：`components/compute_pcm/compute.json`）提供一个完整例子。

新增计算组件时建立清单和两端实现，将组件加入项目选择，再通过 源码：`tools/compute_build.py` 所用的构建规则生成注册及配置。应用通过 `epd_compute_query()` 检查版本、能力和是否可接收任务，再打开会话；共享缓冲的清理与失效操作见[公共组件](../development/common-components.md)。

板型的 源码：`project/sf32lb57-epd-gyd7n6_hcpu/compute.json` 控制执行策略。当前 Anyka 3A 项的 `validated=false` 且 `allow_unvalidated=false`，板级策略禁止运行该项。启用新能力需要在对应硬件验证计算结果、取消、并发和共享内存行为，再更新准入配置。

## 合入前验证

修改后构建主固件及调用方应用，保留编译和导入检查结果。涉及显示、音频、网络或电源时还需检查相应硬件路径；涉及安装和存储时检查内部存储与 TF 卡。描述结果时列出实际验证的范围，以及仍需要特定硬件或服务的部分。
