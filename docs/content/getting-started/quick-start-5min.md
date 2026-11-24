---
title: "5分钟快速入门"
---

# 5分钟快速入门

从零开始，5分钟内完成环境搭建、固件构建、设备烧录，并运行你的第一个墨水屏应用。

## 你将完成什么

- ✅ 搭建 SiFli EPD Platform 开发环境
- ✅ 构建包含触摸示例的完整固件
- ✅ 烧录固件到 SF32LB57 开发板
- ✅ 在墨水屏上运行并验证第一个应用

## 准备工作

**硬件**：
- SF32LB57 开发板（sf32lb57-epd-gyd7n6，带 684×1216 墨水屏）
- USB 转串口线

**软件**（Windows 10/11）：
- Git
- PowerShell（系统自带）

## 步骤 1：环境准备（2分钟）

### 安装 uv 包管理器

```powershell
winget install --id=astral-sh.uv -e
```

**预期输出**：安装成功提示

**验证安装**：

```powershell
git --version
uv --version
```

**预期输出**：
```
git version 2.x.x
uv 0.x.x
```

### 获取源码并初始化

```powershell
# 克隆仓库
git clone https://github.com/OpenSiFli/SiFli-EPD-Platform.git
cd SiFli-EPD-Platform

# 初始化 SDK 子模块
git submodule update --init --recursive
```

**预期输出**：显示子模块克隆进度，最后显示 "Submodule path 'SiFli-SDK': checked out ..."

### 安装 SDK 工具链

```powershell
.\SiFli-SDK\install.ps1
```

**预期输出**：
```
Installing Python 3.x.x...
Installing ARM toolchain...
✓ Installation complete
```

::: tip PowerShell 执行策略错误？
如果遇到 "无法加载，因为在此系统上禁止运行脚本"，执行：
```powershell
Set-ExecutionPolicy -Scope Process Bypass
```
然后重新运行安装命令。
:::

### 激活开发环境

```powershell
# 激活 SDK 环境（每次打开新终端都需要执行）
. .\SiFli-SDK\export.ps1

# 准备构建依赖
python tools/prepare_build.py
```

**预期输出**：
```
Checking SDK version...
Installing font compression dependencies...
Creating SD card component...
✓ Build environment ready
```

**验证环境**：

```powershell
Get-Command scons, arm-none-eabi-gcc | Select-Object Name, Source
```

**预期输出**：显示 `scons` 和 `arm-none-eabi-gcc` 的路径

## 步骤 2：构建固件（1分钟）

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu `
  --board_search_path=../boards `
  PREINSTALL=hello `
  STORAGE_IMAGE=1 `
  -j8
```

**关键参数说明**：
- `PREINSTALL=hello`：将触摸示例预装到文件系统
- `STORAGE_IMAGE=1`：生成文件系统镜像（首次烧录必需）
- `-j8`：使用 8 线程并行编译

**预期输出**（构建成功）：
```
Compiling ...
Linking ...
Creating app package hello.sap...
Creating filesystem image fs_root.bin...
✓ Build complete: project/build_sf32lb57-epd-gyd7n6_hcpu/
```

**产物位置**：`project/build_sf32lb57-epd-gyd7n6_hcpu/`

| 文件 | 说明 |
|------|------|
| `main.bin/ER_IROM1.bin` | HCPU 主固件 |
| `main.bin/ER_IROM2.bin` | 固件资源（含 Wi-Fi 固件） |
| `acpu/acpu.bin` | ACPU 计算固件 |
| `fs_root.bin` | 文件系统镜像（含 hello 应用） |
| `uart_download.bat` | 串口烧录脚本 |

::: warning 注意
`STORAGE_IMAGE=1` 会覆盖设备内部文件系统的所有内容（设置、应用、数据）。后续开发时不要使用此参数。
:::

## 步骤 3：烧录固件（1分钟）

### 连接设备

1. 使用 USB 转串口线连接开发板的 **UART1** 接口到电脑
2. 打开 Windows 设备管理器 → "端口(COM 和 LPT)"
3. 记录新增的 COM 端口号（例如 `COM18`）

### 执行烧录

```powershell
.\project\build_sf32lb57-epd-gyd7n6_hcpu\uart_download.bat
```

**交互提示**：
```
请输入串口编号（如设备为 COM18，输入 18）: 18
```

**预期输出**：
```
Connecting to device...
Downloading ftab.bin to 0x12000000... ✓
Downloading bootloader.bin to 0x12010000... ✓
Downloading ER_IROM1.bin to 0x12020000... ✓
Downloading ER_IROM2.bin to 0x12398000... ✓
Downloading acpu.bin to 0x129FE000... ✓
Downloading epd_waveform.bin to 0x12AE0000... ✓
Downloading fs_root.bin to 0x12B20000... ✓
✓ Download complete. Reset device to run.
```

::: tip 烧录停在"等待芯片连接"？
按开发板上的**复位键**，让下载工具重新建立连接。
:::

烧录完成后，设备自动重启。

## 步骤 4：验证运行（1分钟）

### 检查串口输出

使用串口终端（如 PuTTY、MobaXterm）连接设备：

**串口参数**：
- 波特率：**1000000**
- 数据位：8
- 校验：无
- 停止位：1

**预期启动日志**：
```
[I] System starting...
[I] LVGL initialized
[I] EPD driver ready
[I] Loading desktop...
msh />
```

看到 `msh />` 提示符表示系统启动成功。

### 检查墨水屏显示

设备启动后，墨水屏应显示：
- **桌面界面**
- **状态栏**（顶部显示时间、电池、网络图标）
- **应用图标**："触摸示例"（中文）或 "Touch Demo"（英文）

### 运行触摸示例应用

1. **打开应用**：点击桌面上的"触摸示例"图标
   - **预期**：墨水屏刷新，显示应用主页面

2. **测试计数按钮**：
   - 点击"计数按钮"或 "Counter" 按钮
   - **预期**：数字从 0 开始递增（0 → 1 → 2 ...）

3. **测试页面跳转**：
   - 点击"详细信息"或 "Details" 按钮
   - **预期**：进入子页面，显示详细信息
   - 点击返回按钮（左上角 ← 或 Back）
   - **预期**：返回主页面，计数值保持（例如仍显示 2）

4. **测试串口命令**（可选）：
   ```bash
   msh /> ps
   ```
   **预期输出**：显示运行中的线程列表，包括 LVGL、应用线程等

✅ **验证成功**：如果以上功能正常，说明固件、应用和触摸交互都工作正常。

## 预期遇到的问题

### 编译错误："找不到文件"

**原因**：SDK 子模块未初始化或依赖未准备

**解决**：
```powershell
git submodule update --init --recursive
python tools/prepare_build.py
```

### 烧录失败："端口被占用"

**原因**：串口终端或其他程序占用了 COM 口

**解决**：关闭所有串口终端、下载工具，再运行烧录脚本

### 设备启动后黑屏

**原因**：可能是波形文件未烧录或屏幕供电异常

**解决**：
1. 检查烧录日志中 `epd_waveform.bin` 是否成功下载
2. 按复位键重启设备
3. 检查硬件连接（屏幕排线、电源）

### 应用图标未出现

**原因**：文件系统镜像未烧录

**解决**：确保构建时使用了 `STORAGE_IMAGE=1` 参数，并重新烧录

## 下一步：创建你的第一个应用

现在你已经有了可运行的开发环境和固件，可以开始创建自己的应用：

### 最小应用示例

创建一个显示"Hello World"的应用只需 3 个步骤：

#### 1. 复制示例应用

```powershell
# 在平台根目录执行
New-Item -ItemType Directory modules/myapp
Copy-Item modules/hello/* modules/myapp -Recurse
```

#### 2. 修改应用标识

编辑 `modules/myapp/app.json`：

```json
{
  "id": "myapp",
  "name": "我的应用",
  "version": 1,
  "localized_names": {
    "zh": "我的应用",
    "en": "My App"
  }
}
```

编辑 `modules/myapp/SConstruct`，将 `hello.so` 改为 `myapp.so`：

```python
BuildLibrary('myapp.so', ...)
```

编辑 C 源码（`modules/myapp/hello.c`），将应用 ID 修改：

```c
// 将所有 "hello" 改为 "myapp"
EPD_APP_DEFINE("myapp")
```

#### 3. 构建并安装

```powershell
# 构建应用
scons -C modules APPS=myapp -j8

# 产物位置
# project/build_sf32lb57-epd-gyd7n6_hcpu/app-packages/myapp.sap
```

**安装方式**：
1. 在设备上打开"设置 → 无线与连接 → USB → TF 卡读卡器"
2. 将 `myapp.sap` 复制到 TF 卡
3. 关闭读卡器，进入"设置 → 应用管理 → 应用安装"选择文件安装

::: tip 提示
更新应用只需修改代码、增加 `app.json` 中的 `version` 值，重新构建并安装新的 `.sap` 文件即可。
:::

## 学习路径

根据你的目标，选择下一步学习内容：

| 目标 | 推荐文档 |
|------|---------|
| **理解平台架构** | [核心概念](core-concepts.md) - 双核模型、应用生命周期、内存管理 |
| **深入应用开发** | [第一个应用](../development/first-application.md) - 完整的应用开发流程 |
| **接入平台服务** | [应用开发指南](../development/application-development.md) - 存储、网络、AI 等 |
| **查阅 API** | [API 参考](../reference/api-reference.md) - 完整的平台接口文档 |
| **理解构建系统** | [构建与部署](../build/building.md) - 高级构建选项和分区配置 |
| **设备管理** | [设备与网页管理](device-management.md) - 网络连接、应用管理、系统设置 |

## 更多资源

- **源码仓库**：[GitHub - OpenSiFli/SiFli-EPD-Platform](https://github.com/OpenSiFli/SiFli-EPD-Platform)
- **在线文档**：[SiFli EPD Platform 文档](https://docs.sifli.com/projects/epd-platform/)
- **问题排查**：[开发调试](../troubleshooting/development-debugging.md)
- **示例应用**：`modules/hello`（触摸）、`modules/cfgdemo`（配置）、`modules/netdemo`（网络）

---

**恭喜！** 🎉 你已经完成了 SiFli EPD Platform 的快速入门。现在可以开始构建自己的墨水屏应用了。
