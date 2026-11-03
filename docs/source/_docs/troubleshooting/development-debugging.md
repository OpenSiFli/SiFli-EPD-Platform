---
title: "日常调试"
---

# 日常调试

先确定问题发生在构建、安装、应用启动还是具体业务阶段。同一个“应用打不开”可能来自包校验失败、模块导入缺失、启动回调异常或存储尚未加载；各阶段的检查入口不同。

本页主机命令在 `SiFli-EPD-Platform` 平台源码根目录执行；“串口和运行状态”中的命令发送到设备控制台。

## 构建失败

在激活 SDK 的终端中保存完整命令和第一处错误。并行构建的最后一行通常只是任务汇总，具体原因位于前面。

| 现象 | 检查内容 |
| --- | --- |
| 找不到 Python、SCons 或编译器 | 重新执行 `SiFli-SDK/export.ps1`，按[首次运行](../getting-started/getting-started.md#安装开发环境)检查命令路径 |
| 准备脚本报告 SDK 提交不一致 | 对照 `.gitmodules` 和主仓库记录的子模块提交，检查 `git submodule status --recursive`；项目使用记录的 SDK 版本 |
| 找不到组件或 Kconfig | 在平台源码根目录执行 `python tools/prepare_build.py`，检查组件安装错误及网络访问 |
| 应用找不到固件配置 | 先构建平台，确认 `FIRMWARE` 指向含 `main.elf`、`rtconfig.h`、`cconfig.h`、`rtua.py` 和 `app-profile.json` 的输出目录 |
| `Unexported module imports` | 检查所用函数是否属于公共接口、是否由选定固件导出，以及所需固件配置是否启用 |
| `Missing application entry symbols` | 主应用需提供 `app_main` 和 `EPD_APP_DEFINE(id)`；附属库使用独立的库入口规则 |
| 清单或资源校验失败 | 核对[应用清单](../reference/app-manifest.md)、应用 ID、版本、图标和资源路径；重新生成 `.sap` |

修改代码后使用[构建命令](../build/building.md)增量构建。主固件接口或配置改变时，先重建平台，再针对该固件重建应用。构建输出中的导入检查会列出已解析的符号数量；仅有 `.so` 文件不代表 `.sap` 已成功生成。

## 安装与启动

网页上传后等待安装任务结束，查看最终结果。设备端可在“设置 → 应用管理 → 应用安装”选择 `.sap`。更新要求应用 ID 一致、版本递增、数据版本符合安装器要求，具体见[清单参考](../reference/app-manifest.md)。

应用未出现在桌面时，先检查安装任务是否成功，以及安装卷是否可用。TF 卡交给 USB 读卡器期间不能由设备加载；关闭读卡器并完成 TF 卡应用加载后再启动。

安装失败时保留包文件、错误文字、安装目标和现有应用版本。启动失败时补充从选择应用到失败的串口日志。图像重复、文字缺失等显示问题还需要对应页面和操作步骤；先核对页面生命周期、字体和图片的持有时间，参考[界面接入](../development/touch-ui.md)与[图像资源](../development/application-resources.md)。

## 串口和运行状态

控制台默认使用 uart1、1000000 波特、8N1。烧录结束后释放下载工具占用的串口，再打开串口终端。系统能响应时，以下命令可查询状态：

```text
help
list_thread
list_memheap
ifconfig
svc
svc pan status
svc pan ip
recovery status
```

`help` 列出当前固件实际提供的命令。`list_thread` 显示线程状态、优先级和栈使用情况；`list_memheap` 分别显示注册堆的容量与使用量，不能把 PSRAM 余额当作 SRAM 余额。`ifconfig` 显示网卡地址，取得地址只表示链路配置完成，外部服务可用性还要由请求结果确认。

`svc <应用ID> ...` 转发到已注册后台服务的命令入口。参数由应用定义，可从其 `epd_app_background.command` 实现查阅；没有命令入口的应用不能通过这一方式调用。`svc pan` 是平台提供的蓝牙网络命令组。

## 网络和异步任务

请求失败先记录错误阶段、HTTP 状态、底层返回值和网络 generation。TLS 错误同时检查设备日期与证书验证结果。AI 和语音调用还需要区分服务未配置、应用未授权与供应商返回失败，见[AI 服务](../services/ai-services.md)和[语音服务](../services/speech-services.md)。

页面退出后仍收到结果时，核对工作线程是否持有有效业务上下文、是否检查取消，以及 UI 是否只读取当前会话的结果。不要在 worker 中直接修改 LVGL 对象。受管任务的停止和存储交接顺序见[生命周期](../development/application-lifecycle.md)。

临时诊断输出记录状态变化、操作编号和错误码即可。网络密钥、密码和完整授权头不应写入日志。发布前移除临时插桩，故障记录保留相关输入与复现步骤。

## 断言或系统无响应

记录完整日志、触发步骤、主固件和应用版本。系统已经停止调度时，串口命令无法补充线程信息；保留现场并按[动态应用故障取证](dynamic-app-debugging.md)采集寄存器、SRAM 和 PSRAM。该指南同时说明如何用配套 `.so.nostrip` 定位动态模块。

需要恢复使用时按[恢复模式](recovery.md)处理故障应用。恢复操作与现场采集使用不同流程，先完成需要保留的现场采集。
