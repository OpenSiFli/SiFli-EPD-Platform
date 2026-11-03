---
title: "构建配置"
---

# 构建配置

平台使用 SiFli SDK 的 SCons 和 Kconfig 构建。Kconfig 定义可选功能及依赖，`.conf` 文件选择功能，构建目录中的 `.config` 和 `rtconfig.h` 是解析结果。

## 配置从哪里来

SDK 的 ResolveBoardConfig（源码：`SiFli-SDK/tools/build/building.py`） 先组装 SDK、板型、组件和项目的 Kconfig，再依次读取下列配置。后面的显式设置覆盖前面的设置，最终值仍受 Kconfig 依赖约束。

| 顺序 | 输入 | 维护内容 |
| --- | --- | --- |
| 1 | `boards/sf32lb57-epd-gyd7n6/hcpu/board.conf` | 板上外设、引脚相关驱动和基础能力 |
| 2 | `project/proj.conf` | 项目通用功能选择 |
| 3 | `project/<chip>/proj.conf`，存在时读取 | 芯片系列的项目差异 |
| 4 | `project/sf32lb57-epd-gyd7n6_hcpu/proj.conf` | 此板型上的平台配置 |

源码：`project/Kconfig.proj` 定义平台选项和默认值；板级 Kconfig（源码：`boards/sf32lb57-epd-gyd7n6/`） 声明硬件依赖。组件准备生成 `project/sf-pkgs/Kconfig.conandeps`，因此首次解析前需运行 `tools/prepare_build.py`。

持久修改写入对应的 `.conf` 或 Kconfig 源文件，再执行[平台构建命令](building.md#构建平台)。`build_<board>/rtconfig.h`、`.config`、链接脚本及分区头文件会重新生成。查看实际编译值时读取这些生成文件；提交配置变更时保留上游输入文件的修改。

## 分区和双核构建

源码：`project/sf32lb57-epd-gyd7n6_hcpu/ptab.json` 同时规定 Flash、HCPU RAM、ACPU RAM 和 PSRAM 区域。SDK 据此生成链接与下载信息。镜像超出分区时应检查映射文件和资源大小；修改分区还会影响用户存储、镜像地址及配套核心的共享内存。

启用 `EPD_COMPUTE_ACPU` 时，主构建通过 SDK `AddChildProj` 构建 源码：`project/acpu/`，共享同一份分区表。计算服务的选择和准入由 源码：`project/sf32lb57-epd-gyd7n6_hcpu/compute.json` 与组件选择表（源码：`project/compute-components.json`）生成。HCPU 与 ACPU 使用配套构建产物；功能和协议的修改需要一起重新构建、部署。

## 动态应用继承什么

`scons -C modules APPS=<id> FIRMWARE=<固件目录>` 选择目标固件。平台从该目录的 `rtconfig.h`、`cconfig.h`、`rtua.py` 和 `app-profile.json` 取得 SDK/LVGL 配置、包含路径及编译参数，交给 SDK `ua.BuildLibrary`。链接后的导入符号再与同目录的 `main.elf` 核对。

应用自己的 `app.json` 描述名称、版本、后台能力和文件类型；它不选择主固件中的驱动。应用的 `SConscript` 决定编入哪些应用文件和公共源码组件。修改 LVGL 配置、公共类型布局或固件导出接口后，先构建固件，再重建受影响应用，见[接口兼容规则](../development/application-development.md#二进制兼容与公共符号)。

`APPS`、`PREINSTALL`、`STORAGE_IMAGE` 是构建命令参数，分别选择应用编译、出厂预装和文件系统烧录；这些参数不写入设备运行设置。Wi-Fi、亮度、账号等运行设置由设备服务存储，见[设备管理](../getting-started/device-management.md)。
