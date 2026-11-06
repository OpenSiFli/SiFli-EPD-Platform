# SiFli-EPD-Platform

SiFli-EPD-Platform 是面向 SiFli SF32LB57 墨水屏设备的应用平台，基于 SiFli SDK、RT-Thread 和 LVGL 9 构建。项目提供桌面、系统设置、文件管理、网络、音频、输入法和电源管理，并通过可独立安装的动态应用扩展阅读、天气、AI 对话、笔记等功能。

平台负责通用设备能力，动态应用负责各自的页面、内容和业务逻辑。应用以 `.sap` 安装包分发，可以安装到内部存储或 TF 卡，也可以在构建固件时预装到内部文件系统。

## 项目文档

在线文档：[SiFli EPD Platform 文档](https://docs.sifli.com/projects/epd-platform/)。

项目文档涵盖[设备使用](docs/content/user/user-guide.md)、[系统架构](docs/content/architecture/architecture.md)、[构建与部署](docs/content/build/building.md)和[应用开发](docs/content/development/first-application.md)。计算组件的接口与数据格式保留在各组件的 README 中，并在文档站构建时一同收录。

## 平台功能

| 分类 | 功能 |
| --- | --- |
| 桌面与交互 | 应用入口、状态栏、通知中心、控制中心、触摸与实体按键操作、中英文界面、界面缩放。 |
| 墨水屏显示 | 局部刷新与全刷调度、冷暖前光、墙纸、锁屏，以及休眠和关机时的墙纸显示。 |
| 输入与字体 | 全局中英文输入法、拼音候选、数字与符号输入、语音输入；系统界面使用 MiSans，阅读正文使用平台统一选择的静态 TrueType 字体，可导入外置字体。 |
| 文件与存储 | 内部 LittleFS 文件系统、TF 卡 FAT 文件系统、文件浏览、应用安装与卸载、TF 卡插拔处理、USB TF 卡读卡器。 |
| 无线连接 | WLAN 扫描与连接、AP 热点、蓝牙设备管理、蓝牙音频及蓝牙网络共享（PAN）。默认网络优先选择可用 WLAN，其次使用已经连接的 PAN。 |
| 网页管理 | 登录设备网页后管理 TF 卡文件、查看设备信息、调整平台设置、安装阅读字体，以及填写 AI、语音识别和语音合成配置。 |
| AI 与语音 | 多个 AI 服务配置、模型选择、应用授权、流式回复；语音转文字（STT / ASR）、文字转语音（TTS）、录音与文件转写接口。 |
| 时间与提醒 | 自动网络校时、手动日期时间、时区、12 / 24 小时制，以及供日历应用使用的常驻提醒服务。 |
| 电源管理 | 自动锁屏、系统休眠、按键唤醒、关机、电池监测，以及网络、触摸、音频等服务的休眠协调与恢复。 |

AI、在线语音和天气功能需要配置对应服务的账号或密钥。网页中的 AI 与语音配置属于平台服务；天气的服务配置由天气应用管理。

## 动态应用

| 应用 | 应用 ID | 主要功能 |
| --- | --- | --- |
| 书架 | `books` | 浏览和阅读 TXT、Markdown、EPUB、MOBI；章节目录、阅读进度、字体与排版设置、语音朗读和 AI 伴读。阅读流程包含后台排版、近页预备和缓存管理。 |
| 天气 | `weather` | 配置城市和天气服务，查看实时天气、预报与空气质量，并保存已获取的天气数据。 |
| AI 对话 | `ai_demo` | 模型选择、流式对话、会话历史、联网搜索选项、语音输入与朗读；可用能力取决于所配置服务。 |
| 内容看板 | `board` | 按配置组合展示平台内容摘要，形成独立的看板页面。 |
| 日历 | `calendr` | 年、月、周、日视图，日程、生日、纪念日、倒数日、重复事项、提醒，以及备份和 ICS 导入导出。 |
| 图库 | `gallery` | 浏览 PNG、JPG、JPEG 图片。 |
| 音乐 | `music` | 浏览和播放 MP3、WAV、FLAC，管理播放队列、收藏和播放进度，支持 M3U8 播放列表。 |
| 笔记 | `notes` | 笔记本分类、正文编辑、文字格式、待办、图片、录音、转写、AI 整理、搜索、回收站及导入导出。 |
| 单词 | `words` | 本地词库查询、单词学习及学习记录。 |

项目还包含 `hello`、`cfgdemo` 和 `netdemo` 示例应用，可用于了解触摸交互、配置保存和后台网络状态获取。

笔记正文和附件保存在 TF 卡 `/Notes` 中，日历事项保存在内部存储。应用的安装位置与业务数据的位置由各应用设计决定。详细说明见 [笔记](modules/notes/README.md) 和 [日历](modules/calendr/README.md)。

## 硬件与软件基础

当前工程支持的构建目标为 `sf32lb57-epd-gyd7n6_hcpu`。

| 项目 | 当前配置 |
| --- | --- |
| 主控 | SF32LB57GYD7N6。 |
| 显示 | E0470A03 墨水屏，界面分辨率为 684 × 1216，使用配套波形文件。 |
| 输入 | FT5446 触摸、实体按键。 |
| 存储 | NOR Flash、PSRAM、TF 卡。 |
| 连接 | Wi-Fi（AIC8800D40-N 驱动）、蓝牙、USB TF 卡读卡器。 |
| 音频 | 扬声器、PDM 麦克风、蓝牙音频输出。 |
| 软件 | SiFli SDK 2.5.x、RT-Thread、LVGL 9、lwIP、LittleFS、CherryUSB。 |

项目板级适配位于 `boards/`，墨水屏驱动与波形位于 `src/boards/`。引脚、屏幕供电和分区配置与这块硬件配套，其他板卡需要相应适配。

SDK 作为 Git 子模块放在 `SiFli-SDK/`，来自 [jingxbw-work/SiFli-SDK 的 EPD 分支](https://github.com/jingxbw-work/SiFli-SDK/tree/EPD)。该分支基于官方 `release/v2.5`，集成 D40-N 驱动及平台使用的字体、软件混音修复。来源和分支记录在 [.gitmodules](.gitmodules)，实际构建使用主仓库固定的 SDK 提交，依赖准备脚本会核对该版本。

字体修复保持 SDK 原有的 TinyTTF 公开接口及 TTF/CFF 支持。平台阅读组件自行限定字体格式、字形复杂度和内存预算，具体范围见[阅读字体组件](components/compute_reader_font/README.md)。

音频修复仅在 `SOFTWARE_TX_MIX_ENABLE` 开启时生效。当前板级项目已启用软件混音，可在音乐播放期间合成触摸提示音，并分别处理声音类型的音量和静音。

## 代码结构

```text
.
├── assets/                     # 平台图标、提示音、网页等资源
├── boards/                     # 项目板级配置与外设适配
├── components/                 # TF 卡、无线及计算组件
├── font/                       # 字体资源与许可
├── modules/                    # 动态应用
│   ├── books/                  # 书架与图书格式解析
│   ├── notes/                  # 笔记
│   ├── calendr/                # 日历
│   └── common/                 # 阅读、图像、HTTP、输入等公共组件
├── project/                    # 主固件构建、配置和分区
│   └── acpu/                   # ACPU 配套计算固件
├── SiFli-SDK/                  # SDK 子模块
├── src/
│   ├── boards/                 # 墨水屏驱动、供电与波形
│   ├── platform/               # AI、语音、图像、计算等平台接口
│   ├── services/               # 网络、存储、音频、阅读等后台服务
│   ├── thirdparty/             # 平台使用的第三方代码
│   └── ui/                     # 系统页面、控件与动态应用运行框架
└── tools/                      # 构建准备、资源生成、应用打包与镜像工具
```

HCPU 运行界面和平台服务；计算组件可通过统一接口把支持的任务交给 ACPU。组件清单位于 [project/compute-components.json](project/compute-components.json)，各组件的 `compute.json` 描述构建与执行能力。

动态应用使用平台导出的接口访问公共能力。应用入口、页面控件和生命周期接口集中在 [src/ui/platform](src/ui/platform)，AI、语音、录音、图像和日期等接口位于 [src/platform](src/platform)。构建工具会检查应用导入的符号是否由配套固件提供。

### 阅读数据流程

书架通过平台的组件式计算接口调用 ACPU。文件访问与界面操作由 HCPU 负责，主要阅读计算由 ACPU 执行：

| 工作 | 执行位置 |
| --- | --- |
| 读取书籍、字体和图片资源，保存阅读位置与缓存 | HCPU 后台服务负责存储访问，按计算任务请求供数。 |
| 解压与解析文档、图片解码、字体测量与字形生成、分页排版、页面像素合成 | ACPU 计算组件。 |
| 触摸、阅读菜单、应用切换，以及把完成的页面交给 LVGL 和墨水屏驱动 | HCPU 界面与显示服务。 |

当前阅读位置优先准备，后台继续预排和准备相邻页面，并复用章节、排版和字形缓存。EPUB 可以独立处理目标章节，TXT 使用源文件位置恢复阅读；这些任务共用计算调度和内存预算。

相关实现见 [文档转换](modules/books/compute/README.md)、[阅读字体](components/compute_reader_font/README.md)和[页面合成](components/compute_reader_render/README.md)。ACPU 算法随平台固件构建，动态应用通过配套接口调用。

## 编译

### 准备环境

当前资源转换和文件系统镜像工具使用 Windows 可执行程序。以下命令在 Windows PowerShell 中执行，需要先安装 Git 和 `uv`。进入仓库后，构建和烧录命令均从仓库根目录运行。

获取源码并进入仓库：

```powershell
git clone https://github.com/OpenSiFli/SiFli-EPD-Platform.git
Set-Location SiFli-EPD-Platform
```

首次准备 SDK 与工具环境：

```powershell
git submodule update --init --recursive
.\SiFli-SDK\install.ps1
```

每次打开新的构建终端，先激活 SDK 环境并指定项目板级目录：

```powershell
. .\SiFli-SDK\export.ps1
$env:SIFLI_SDK_BOARD_SEARCH_PATH = (Resolve-Path .\boards).Path
```

首次构建或依赖变更后，准备本地 TF 卡组件与项目软件包：

```powershell
python .\tools\prepare_build.py
```

该脚本会核对 SDK 提交、应用 `patches/SiFli-SDK/` 中的项目补丁，将 `components/sdcard` 创建为本地 Conan 软件包，并安装 `project/sf-pkg.yaml` 中声明的依赖。重复运行会跳过已应用的补丁；遇到冲突会停止并保留已有 SDK 修改。

已有工作副本更新源码时，同步主仓库固定的 SDK 版本：

```powershell
git pull --ff-only
git submodule update --init --recursive
```

SDK 更新后，在新的构建终端重新激活环境并运行 `tools/prepare_build.py`，再构建固件及所需应用。

### 构建平台固件

```powershell
python -m SCons -C project --board=sf32lb57-epd-gyd7n6_hcpu -j8
```

主要产物位于 `project/build_sf32lb57-epd-gyd7n6_hcpu/`：

| 产物 | 用途 |
| --- | --- |
| `main.elf`、`main.bin/` | HCPU 主固件及分区二进制。 |
| `acpu/acpu.bin` | ACPU 配套计算固件。 |
| `bootloader/bootloader.bin`、`ftab/ftab.bin` | 启动程序和 Flash 表。 |
| `fs_root.bin` | 内部 LittleFS 文件系统镜像。 |
| `app-profile.json` | 应用构建所需的配套固件信息。 |
| `app-packages/*.sap` | 本次选择构建的动态应用安装包。 |
| `uart_download.bat`、`download.bat` | 串口及 J-Link 下载脚本。 |

波形文件为 `src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin`，当前配置会将波形加入下载清单。分区由 `project/sf32lb57-epd-gyd7n6_hcpu/ptab.json` 定义。

### 构建动态应用

可以在编译平台时同时生成指定应用：

```powershell
python -m SCons -C project --board=sf32lb57-epd-gyd7n6_hcpu APPS=books,weather,ai_demo,board,calendr,gallery,music,notes -j8
```

已有配套平台固件时，也可以单独构建应用：

```powershell
python -m SCons -C modules APPS=books,notes -j8
```

`APPS` 使用逗号分隔应用 ID。独立构建默认读取上述固件输出目录中的 `main.elf`、配置头文件和 `app-profile.json`；使用其他构建目录时，通过 `FIRMWARE` 参数指定。

### 构建预装固件

以下命令将书架、天气、AI 对话、内容看板、日历、图库、音乐和笔记放入内部文件系统镜像：

```powershell
python -m SCons -C project --board=sf32lb57-epd-gyd7n6_hcpu APPS=books,weather,ai_demo,board,calendr,gallery,music,notes PREINSTALL=books,weather,ai_demo,board,calendr,gallery,music,notes STORAGE_IMAGE=1 -j8
```

| 参数 | 含义 |
| --- | --- |
| `APPS` | 选择需要生成 `.sap` 安装包的应用。 |
| `PREINSTALL` | 选择写入 `fs_root.bin` 的应用，同时构建这些应用。 |
| `STORAGE_IMAGE=1` | 将 `fs_root.bin` 加入本次生成的下载清单。 |

**烧录 `fs_root.bin` 会覆盖内部文件系统，包括原有账号、设置、应用和内部应用数据；TF 卡内容不受这一镜像影响。** 普通固件更新时，使用不带 `STORAGE_IMAGE=1` 的构建命令重新生成下载清单，保留设备的内部文件系统。

## 烧录与安装

### 固件烧录

连接设备 USB，关闭占用串口的程序，并关闭设备的 TF 卡读卡器功能。在已经激活 SDK 环境的终端运行：

```powershell
& .\project\build_sf32lb57-epd-gyd7n6_hcpu\uart_download.bat
```

按照脚本提示输入串口编号，例如设备为 `COM18` 时输入 `18`。使用 J-Link 时运行同目录的 `download.bat`。下载脚本包含本次构建的镜像路径和地址，直接使用构建输出时应保留其目录结构。

### 动态应用安装

1. 在设备上打开“设置 → 无线与连接 → USB → TF 卡读卡器”。
2. 将 `app-packages/` 中需要的 `.sap` 文件复制到电脑识别出的 TF 卡。
3. 复制完成后安全弹出磁盘，并在设备上关闭 TF 卡读卡器。
4. 进入“设置 → 应用管理 → 应用安装”，选中 `.sap` 文件，选择安装位置并完成安装。

TF 卡交给电脑作为读卡器使用期间，设备暂停访问卡上的文件和应用；关闭读卡器后恢复本地访问。动态应用使用配套固件的导出接口，平台接口发生变化后应重新构建相应安装包。

## 扩展应用

一个动态应用通常由以下文件组成：

| 文件 | 内容 |
| --- | --- |
| `app.json` | 应用 ID、名称、版本、后台运行能力及文件关联等声明。 |
| `SConstruct`、`SConscript`、`rtconfig.py` | 应用编译配置。 |
| `exports.map` | 模块导出符号定义。 |
| `i18n/strings.json` | 应用本地化文本。 |
| `assets/`、`res/` | 图标和其他资源。 |
| C 源文件与头文件 | 页面、数据模型和业务逻辑。 |

可以从 `modules/hello` 开始了解应用结构，再参考功能相近的应用接入平台服务。新增应用目录及 `app.json` 中的 ID 保持一致，通过 `APPS=<应用ID>` 构建。应用 ID 使用 1—7 位小写字母、数字或下划线。

应用安装包由 `tools/native_apps.py` 和 `tools/package_app.py` 生成，包含模块、资源、应用声明和完整性信息。应用业务代码放在各自的 `modules/<应用ID>/` 中，可复用能力通过平台公共接口提供。

## 许可证

[Apache License 2.0](LICENSE)
SDK、字体、图标及第三方组件保留各自的许可证，相关说明随对应源码或资源提供；MiSans 字体许可见 [font/MiSans-License.pdf](font/MiSans-License.pdf)。
