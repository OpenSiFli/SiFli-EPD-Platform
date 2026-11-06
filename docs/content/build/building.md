---
title: "构建与部署"
---

# 构建与部署

本页固件与应用命令在 `SiFli-EPD-Platform` 平台源码根目录的 PowerShell 中执行，该目录包含 `src/`、`project/`、`modules/` 和 `SiFli-SDK/`。当前平台构建使用 `sf32lb57-epd-gyd7n6_hcpu`，硬件配置位于项目板级目录（源码：`boards/sf32lb57-epd-gyd7n6/`）。送审中的 SDK 独立板型不参与当前平台构建，两者的用途见[两套板级目录](../contributing/sdk-board.md#两套板级目录)。

## 开发工具与执行目录

| 工作 | 工具要求 | 执行目录与入口 |
| --- | --- | --- |
| 构建固件和动态应用 | SDK 安装脚本提供的 Python、SCons 与 Arm GNU 工具链。使用已有 PNG，不需要 Node.js。 | 平台源码根目录；本页的 `scons` 命令。 |
| 从 SVG 生成图标 | Node.js 22.x、npm 和 `sharp` 0.34.4。 | 平台源码根目录；[图标生成](../development/application-resources.md#重新生成平台图标)。 |

## 准备环境

新电脑先完成[开发工具安装](../getting-started/getting-started.md#安装开发环境)。已有 SDK 环境时，在平台源码根目录执行：

```powershell
git submodule update --init --recursive
. .\SiFli-SDK\export.ps1
python tools/prepare_build.py
```

`export.ps1` 激活已经安装的 SDK、Python 和 Arm GNU 工具链。准备脚本执行以下检查与安装：

1. 比较 SDK 的 HEAD 与主仓库暂存区记录的子模块提交。SDK 不匹配时终止。
2. 从 `components/sdcard/conan.lock` 读取依赖，在本地创建 SD 卡组件包，并核对实际生成的包修订号与项目依赖声明、锁文件是否一致。缺少基础配方 `sf-pkg-base` 时需要访问 Conan 配置中的 `artifactory` 源。
3. 在 `project/` 执行 `sdk.py sf-pkg install`，生成组件 Kconfig 后再解析板级配置。

SDK 或 SD 卡组件更新后重新执行准备脚本。普通增量构建直接运行 SCons。SDK 使用项目记录的子模块版本，公共仓库地址见源码根目录的 `.gitmodules`；克隆必须能取得该提交及其递归子模块。

## 构建平台

板级、项目配置与生成文件之间的关系见[构建配置](build-configuration.md)。

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards -j8
```

输出位于 `project/build_sf32lb57-epd-gyd7n6_hcpu/`，包括主固件、配套 ACPU 和启动镜像、公共资源、`fs_root.bin` 及下载脚本。下载脚本同时引用源码中的 `src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin`。默认不编译应用；平台构建不需要单词词库、Node.js 或 `sharp`。

文件系统镜像由 SDK 的 `tools/mklfsimg/mklfsimg.exe` 生成，使用可选的 `disk/` 初始内容；`disk/` 不存在时从空文件系统开始，再叠加 `PREINSTALL` 应用。普通下载清单不包含 `fs_root.bin`。

## 构建应用包

先完成一次平台构建，再执行：

```powershell
scons -C modules APPS=hello -j8
scons -C modules APPS=weather,books -j8
scons -C modules APPS=all FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
scons -C modules APPS=netdemo,cfgdemo FIRMWARE=build_sf32lb57-epd-gyd7n6_hcpu -j8
```

| 参数 | 含义 |
| --- | --- |
| `APPS` | 逗号分隔的应用 ID；`all` 选择九个配套应用，省略或 `none` 不构建应用。 |
| `FIRMWARE` | `project/` 下的固件目录名或绝对路径；默认是 `build_sf32lb57-epd-gyd7n6_hcpu`。 |
| `WORDS_DICTIONARY` | 可选的 WDB 词库绝对路径，只在选择 `words` 时使用。 |

应用集合见下表。普通新应用按[开发指南](../development/application-development.md#创建新应用)放在 `modules/<id>/`，直接使用 `APPS=<id>` 构建。音乐需要固件启用 `AUDIO_LOCAL_MUSIC` 和 `PKG_LIB_FLAC`；AI 对话使用平台键盘提供拼音输入，富文本显示需要 `LV_USE_SPAN`。

### 应用选择对照

| 应用 | ID | `APPS=all` / `PREINSTALL=all` | 八应用发布预装 |
| --- | --- | --- | --- |
| 书架 | `books` | 包含 | 包含 |
| 天气 | `weather` | 包含 | 包含 |
| AI 对话 | `ai_demo` | 包含 | 包含 |
| 内容看板 | `board` | 包含 | 包含 |
| 日历 | `calendr` | 包含 | 包含 |
| 图库 | `gallery` | 包含 | 包含 |
| 音乐 | `music` | 包含 | 包含 |
| 单词 | `words` | 包含 | 不包含 |
| 触摸示例 | `hello` | 包含 | 不包含 |
| 笔记 | `notes` | 不包含，需显式选择 | 包含 |
| 网络示例 | `netdemo` | 不包含，需显式选择 | 不包含 |
| 配置示例 | `cfgdemo` | 不包含，需显式选择 | 不包含 |

`all` 的九应用名单由 源码：`tools/native_apps.py` 的 `select_apps()` 定义；八应用预装组合由[发布命令](release.md#构建八个预装应用)显式指定。`notes`、`netdemo`、`cfgdemo` 和新增应用通过具体 ID 选择，例如 `APPS=notes,calendr`。

`PREINSTALL` 本身会构建选中的应用并放入文件系统镜像，无需再写同一组 `APPS`。两项同时指定时构建二者并集，只预装 `PREINSTALL` 中的应用。

所选固件目录必须包含 `main.elf`、`rtconfig.h`、`cconfig.h`、`rtua.py` 和 `app-profile.json`。应用通过 `native_module.configure(repo, firmware, module_dir)` 继承该固件的配置和包含路径，由 SDK `ua.BuildLibrary` 编译、链接；平台在打包前检查模块格式、固件导入符号和主应用入口。二进制接口兼容规则见[应用开发](../development/application-development.md#二进制兼容与公共符号)。

安装包输出到 `project/build_<board>/app-packages/<id>.sap`。统一入口编译模块、转换图标并打包资源；单独运行模块目录的构建只生成中间产物。修改应用源码或资源后重新选择该应用构建，无需重新编译主固件，未选择应用的旧输出不会更新。

单词应用使用已有 PNG 和预生成词库，普通构建由 Python 解压、校验词库，由 SDK 转换图标。词库生成与打包脚本位于单词工具目录（源码：`modules/words/tools/`）。平台和应用的翻译表由普通构建自动生成，见[多语言](../development/multilingual.md)。

平台和应用也可一次构建：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards APPS=weather,books -j8
```

## 安装与预装

在设备网页上传 `.sap` 可直接安装。也可以把 `.sap` 复制到 TF 卡任意可浏览目录，通过设备“设置 → 应用管理 → 应用安装”选择文件及安装目标。首次安装选择内部存储或 TF 卡，更新沿用原安装位置，要求应用版本递增且数据版本兼容，详见[安装包](../development/application-development.md#安装包与存储)。

```powershell
# 构建并将天气和书架放入出厂镜像
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards PREINSTALL=weather,books -j8

# 将出厂镜像加入下载清单
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

`PREINSTALL` 接受与 `APPS` 相同的 ID；`all` 的范围见[应用选择对照](#应用选择对照)。可选 `disk/` 目录中的文件也会进入镜像；所有预装内容必须装入分区表规定的 5 MiB 内部文件系统。

运行 `project\build_sf32lb57-epd-gyd7n6_hcpu\uart_download.bat` 烧录。`STORAGE_IMAGE=1` 或 `STORAGE_IMPORT=<备份绝对目录>` 会将文件系统镜像加入下载清单，烧录后覆盖内部文件系统。备份导入格式见下节。初始化后重新构建时省略这两项，恢复保留用户数据的普通下载清单。

SDK 制镜工具使用 Windows 系统 ANSI 编码处理文件名。非 UTF-8 环境中的非 ASCII 文件名可能无法生成设备所需的 UTF-8 路径，构建会报错；可使用英文预置文件名，文件正文仍可包含中文。

## 导入已有文件系统备份

`STORAGE_IMPORT` 接受一个已有备份目录：

```text
backup/
    migration.json
    ble.bin
    flash/
        apps/
        data/
        system/
```

`migration.json` 的 `format` 为 1，`files` 是相对路径到 SHA-256 的映射，覆盖 `flash/` 下全部文件和 `ble.bin`。`ble.bin` 必须为 16 KiB。`tools/lfs_image.py` 校验文件集合、摘要和长度后，从 `flash/` 按当前分区表重建 LittleFS 镜像。

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards STORAGE_IMPORT=C:/epd-backup PREINSTALL=weather -j8
```

导入构建同时将 `fs_root.bin` 和备份中的 `ble.bin` 加入下载清单。备份已有同 ID 应用目录时，预装包不覆盖该目录；使用设备应用安装流程更新旧包。镜像必须能装入当前 5 MiB 文件系统。烧录时覆盖对应分区，完成后省略 `STORAGE_IMPORT` 重新生成普通下载清单。

## 应用开发与交付

[应用开发](../development/application-development.md)说明如何创建原生应用、使用公共接口和生成 `.sap`。

保存发布对应的 `main.elf`、应用 `.so.nostrip`、源码和配置，以便使用[故障定位指南](../troubleshooting/dynamic-app-debugging.md)解释设备现场。
