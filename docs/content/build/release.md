---
title: "发布与版本配套"
---

# 发布与版本配套

一次完整发布包含配套 HCPU、ACPU、启动与资源镜像、目标面板波形、所选动态应用、必要的文件系统镜像和调试符号。应用包由该固件的导出接口及配置构建，不能只根据文件名相同认定可混用。

从 `main` 的同一个提交构建发布版和调试版，为通过验收的发布提交打标签。发布版 UI 版本为 `x.x.x-构建ID前六位`。调试版使用 `TEST_DEBUG=1`，UI 版本追加 `-dev`，输出写入 `project/build_sf32lb57-epd-gyd7n6_hcpu_dev/`。默认的发布版输出目录为 `project/build_sf32lb57-epd-gyd7n6_hcpu/`。

## 构建八个预装应用

完成[构建准备](building.md)后，在 `SiFli-EPD-Platform` 平台源码根目录的 PowerShell 中执行：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards PREINSTALL=books,weather,ai_demo,board,calendr,gallery,music,notes STORAGE_IMAGE=1 -j8
```

此组合明确选择书架、天气、AI 对话、内容看板、日历、图库、音乐和笔记。`PREINSTALL` 会同时构建和预装这些应用。`APPS=all` 包含单词和触摸示例，不包含笔记；完整集合见[应用选择对照](building.md#应用选择对照)。文件系统总容量为 4.75 MiB，构建和镜像生成必须成功后才有可发布结果。

`STORAGE_IMAGE=1` 的下载清单包含 `fs_root.bin`，烧录会覆盖内部文件系统。只更新固件而保留数据时，省略该选项重新生成下载清单。TF 卡内容不包含在内部文件系统镜像中。

在已经完成八应用构建的情况下，生成并校验 UART 出厂包。

```powershell
uv run --no-project --with littlefs-python==0.19.0 python tools/firmware_bundle.py create --build project/build_sf32lb57-epd-gyd7n6_hcpu
```

调试包传入带 `_dev` 的构建目录。包名由 Bloom UI 版本决定。`firmware.json` 记录构建类型、完整构建 ID、主仓和 SDK 提交、镜像及预装应用。打包时会校验分区地址、镜像大小、LittleFS 内的八个应用、应用文件的摘要与构建配套关系。

解压后，在包目录执行 `.\flash_factory.bat COM4`。使用装接 PPK2 的板时，替换为实际对应的串口。发布验收使用同一提交的两套包，分别验证调试版串口空闲回包，以及发布版启动、锁屏、轻睡、深睡和关机。

GitHub 定时真机测试使用标有 `epd-lab` 的 Windows 自托管运行器，运行器需要 Python 和 pyserial。仓库变量 `EPD_LAB_USB_SERIAL` 选择专用测试板；测试会写入出厂镜像并覆盖该板的内部文件系统。手动运行工作流时同时选择 `build_firmware` 和 `run_hardware` 可启动同一流程。

## 包内清单

| 产物 | 来源 | 配套要求 |
| --- | --- | --- |
| 启动表、引导、HCPU 程序与公共资源 | `project/build_sf32lb57-epd-gyd7n6_hcpu/` 的本次构建产物 | 使用同次生成的分区地址及下载脚本。 |
| ACPU 程序 | 同一构建目录的 `acpu/` | 与 HCPU 计算协议、组件表和内存布局匹配。 |
| 面板波形 | `src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin` | 与目标面板型号一致，复制到交付包内并修正脚本相对路径。 |
| 文件系统镜像 | 构建目录 `fs_root.bin` | 记录是出厂预装、空镜像还是备份导入，明确是否烧录。 |
| 动态应用 | 构建目录 `app-packages/<id>.sap` | 记录 ID、version、data_version、build_id 和摘要；未重新构建的旧包不能列作本次新版。 |
| 调试符号 | 固件 ELF、应用 `.so.nostrip` 及必要映射文件 | 与交付镜像同次构建，供故障还原。 |
| 发布记录 | 版本、配置、包清单、摘要、检查结果和已知限制 | 区分代码回归与实际板端测试。 |

当前地址表见[首次运行](../getting-started/getting-started.md)。下载脚本应从交付目录运行，不能仍依赖开发者电脑上的绝对源码路径。

## 发布检查表

| 阶段 | 检查动作 | 通过条件 |
| --- | --- | --- |
| 来源 | 记录平台提交、SDK 提交和所有未提交差异。 | 指定版本可重新取得；SDK 与平台子模块记录一致，必要本地补丁已保存。 |
| 配置 | 保存板型、分区表、配置输入、生成配置、组件清单和构建命令。 | HCPU/ACPU 使用同一配套布局，启用功能可追溯。 |
| 编译 | 完整构建固件与全部交付应用。 | 构建成功，导出符号和模块兼容检查通过。 |
| 文件系统 | 检查预装八应用及空间，核对是否包含出厂数据或导入数据。 | 镜像可生成、应用清单完整，无私人账号与服务密钥。 |
| 内容 | 计算交付文件 SHA-256，核对脚本引用的每个文件。 | 文件都在包中，大小、地址与摘要一致。 |
| 交付 | 在独立目录解压并核对脚本、清单和文档入口。 | 不依赖原工作目录；已知限制可查。 |

SDK 最新混音修复对应的蓝牙音量适配、长休眠触摸问题和最新短章节窗口的验证状态均见[SDK 专题](../contributing/sdk-board.md)及[问题记录](../troubleshooting/troubleshooting.md)。发布前需按对应验证项确认实际状态，并记录尚未通过的项目。

## 版本记录字段

发布记录至少包含 `platform_commit`、`sdk_commit`、`board`、`build_command`、`preinstalled_apps`、文件名/地址/大小/SHA-256、应用清单版本、主机测试结果、板端测试范围及文档版本。若工作树不干净，同时保留差异补丁及新增文件，不能只记录 HEAD。

文档当前核对的版本见[源码快照](../../public/assets/source-snapshot.json)。该文件记录文档依据，不是已生成或已验证的固件发布清单。
