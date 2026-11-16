---
title: "首次运行"
---

# 首次运行

本指南从 Windows 开发环境开始，构建并烧录带有触摸示例的固件。目标板为 `sf32lb57-epd-gyd7n6_hcpu`，使用 SF32LB57、684×1216 墨水屏和 TF 卡。应用开发使用同一份平台源码及其 SiFli SDK 子模块。

## 安装开发环境

安装 Git 和 [uv](https://docs.astral.sh/uv/getting-started/installation/)。uv 用于安装 SDK 指定的 Python 环境和工具；Windows 可以使用：

```powershell
winget install --id=astral-sh.uv -e
```

重新打开 PowerShell，确认 `git --version` 和 `uv --version` 可以执行。克隆平台源码仓库 `SiFli-EPD-Platform` 后，进入包含 `src/`、`project/`、`modules/` 和 `SiFli-SDK/` 的根目录。本页后续命令均在这个平台源码目录执行：

```powershell
git submodule update --init --recursive
& .\SiFli-SDK\install.ps1
. .\SiFli-SDK\export.ps1
python tools/prepare_build.py
```

`install.ps1` 下载 SDK 锁定的工具和 Python 依赖；`export.ps1` 将已安装环境加入当前终端。每次打开新终端都要重新执行 `export.ps1`。首次安装和组件准备需要网络。若 PowerShell 禁止执行脚本，可在当前窗口执行 `Set-ExecutionPolicy -Scope Process Bypass` 后再运行脚本。

准备脚本核对 SDK 子模块提交，使用 uv 将字体压缩依赖安装到当前构建 Python 环境，创建本地 SD 卡组件包，并安装项目组件。SDK、Python 和组件依赖由仓库中的版本记录确定。脚本报错时保留第一处错误及所执行命令，处理方法见[日常调试](../troubleshooting/development-debugging.md)。

确认激活结果：

```powershell
Get-Command python,scons,arm-none-eabi-gcc,sftool
python --version
arm-none-eabi-gcc --version
$env:SIFLI_SDK
```

`SIFLI_SDK` 应指向平台源码中的 `SiFli-SDK`。普通固件与应用构建使用已有 PNG 图标，不需要 Node.js。图标生成的工具要求见[开发工具与执行目录](../build/building.md#开发工具与执行目录)。

## 构建带示例的固件

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards PREINSTALL=hello STORAGE_IMAGE=1 -j8
```

构建成功后，`project/build_sf32lb57-epd-gyd7n6_hcpu/` 包含主固件、配套 ACPU、启动镜像、资源、文件系统镜像和下载脚本；`app-packages/hello.sap` 是触摸示例安装包。`PREINSTALL=hello` 将示例放入出厂文件系统；`STORAGE_IMAGE=1` 将这个文件系统加入本次烧录，**覆盖设备内部文件系统**，TF 卡内容不在该镜像中。

已有设备只更新固件时省略 `STORAGE_IMAGE=1`。其他应用、预装组合及备份导入见[构建与部署](../build/building.md)。

## 连接与烧录

串口烧录和固件控制台均通过板子的 UART1 通道连接电脑，两者分时使用同一个 COM 口。板上引出接口与 USB 转串口电路的连接取决于板卡版本；按对应板卡的接口标识连接 UART1 下载/调试通道，在 Windows 设备管理器的“端口”中确认新增的 COM 编号。控制台参数为 **1000000 波特、8 位数据、无校验、1 位停止位**，下载速率由下载脚本设置。

TF 卡读卡器使用芯片的原生 USB 数据通道，电脑识别为磁盘。使用板上复合 USB 接口时，串口与磁盘可能通过同一根 USB 线出现，仍是两个不同设备；是否共用连接器以该板卡接线为准。

关闭占用该 COM 口的串口终端，运行：

```powershell
& .\project\build_sf32lb57-epd-gyd7n6_hcpu\uart_download.bat
```

按脚本提示填写串口编号。脚本使用 sftool 和本次生成的镜像地址；如果下载工具停在等待芯片连接，按板上复位键，使下载工具重新建立连接。完成后复位启动，用上述参数重新打开串口终端。

当前板型的镜像布局如下。表中路径相对于构建输出目录，`main.bin/` 是目录，包含 `ER_IROM1.bin` 和 `ER_IROM2.bin` 两个镜像文件；波形在源码中保存，发布打包时复制到交付目录。地址来自分区表（源码：`project/sf32lb57-epd-gyd7n6_hcpu/ptab.json`）及生成的下载脚本；更改分区表后使用重新生成的脚本。

| 镜像 | 地址 | 用途 |
| --- | --- | --- |
| `ftab/ftab.bin` | `0x12000000` | 启动镜像表 |
| `bootloader/bootloader.bin` | `0x12010000` | 引导程序 |
| `main.bin/ER_IROM1.bin` | `0x12020000` | HCPU 固件 |
| `main.bin/ER_IROM2.bin` | `0x12378000` | 固件公共资源 |
| `acpu/acpu.bin` | `0x129E0000` | 配套 ACPU 程序 |
| `epd_waveform.bin` | `0x12AC0000` | 面板波形，源码位于 `src/boards/epd_e0470a03_57x/waveform/` |
| `fs_root.bin` | `0x12B00000` | 4.75 MiB 内部文件系统 |
| `dfu/dfu.bin` | `0x12FC0000` | TF 卡裸机升级程序 |

手动选择 HCPU 更新镜像时，配套烧录同一次构建生成的 `ER_IROM1.bin` 和 `ER_IROM2.bin`。主固件使用链接时确定的地址读取资源，其中包括 Wi‑Fi 模组固件。

BLE 保存区位于 `0x12ABC000`，大小 16 KiB；备份导入流程可单独提供该区镜像。普通串口下载不写入 BLE 保存区。

## TF 卡固件更新

首次部署 DFU 布局时，一起烧录二级 Bootloader、启动表、主固件及资源、ACPU 镜像和 DFU。内部文件系统从 5 MiB 调整为 4.75 MiB。使用新布局生成文件系统镜像，或将旧文件系统内容迁移到新镜像。首次从旧布局切换时，擦除 `0x12FFE000` 起的 8 KiB，初始化 DFU 的两个持久化扇区。

使用打包工具指定本次要更新的镜像。下面的命令将同一次构建的主固件与内置资源放在一起。

```powershell
python tools/package_dfu.py --main --resources -o epd-update.zip
```

`--acpu` 和 `--waveform` 分别选择 ACPU 固件和波形，`--all` 选择四类镜像。将 ZIP 放入 TF 卡，再进入「设置 → 固件更新」选择文件并确认。设备重启后自动解包、写入并重启。

选择「进入固件恢复模式」可在 DFU 页面手动选包。KEY2 和 KEY3 移动选择，KEY1 确认。目标分区开始擦写前，可选择 `Exit and reboot` 返回平台。擦写中断后，设备继续启动 DFU，使用原包从头执行，也可以选取包含待修复目标的新包。

## 确认首次运行

启动后应出现桌面，串口进入 `msh />` 命令提示符。桌面的“触摸示例”对应应用 ID `hello`；英文界面显示 `Touch Demo`。打开应用，操作计数按钮、进入子页面并返回，检查计数和页面显示。应用进入锁屏后先解锁再继续操作。

内部存储和 TF 卡的应用加载分别管理。插入 TF 卡并选择加载后，卡内已安装应用才出现在桌面。连接网络、打开网页和安装其他应用见[设备与网页管理](device-management.md)。

接下来可以[创建第一个应用](../development/first-application.md)，或阅读[平台架构](../architecture/architecture.md)了解显示、服务和动态模块之间的关系。
