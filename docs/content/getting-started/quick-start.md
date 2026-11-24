---
title: "快速开始"
---

# 快速开始

**5 分钟快速上手 SiFli EPD Platform 应用开发**。本指南帮助你快速构建、烧录固件并运行第一个示例应用。

## 前置要求

- Windows 10/11 操作系统
- Git 和 PowerShell
- 开发板：sf32lb57-epd-gyd7n6（SF32LB57 + 684×1216 墨水屏）
- USB 转串口线

## 一、环境准备（3 分钟）

### 1. 安装工具

```powershell
# 安装 uv（Python 包管理器）
winget install --id=astral-sh.uv -e

# 验证安装
git --version
uv --version
```

### 2. 获取源码

```powershell
git clone https://github.com/OpenSiFli/SiFli-EPD-Platform.git
cd SiFli-EPD-Platform
```

### 3. 初始化 SDK 和依赖

```powershell
# 初始化 SDK 子模块
git submodule update --init --recursive

# 安装 SDK 工具链
.\SiFli-SDK\install.ps1

# 激活环境（每次打开新终端都需要执行）
. .\SiFli-SDK\export.ps1

# 准备构建依赖
python tools/prepare_build.py
```

::: tip 提示
如果遇到 PowerShell 执行策略错误，运行：`Set-ExecutionPolicy -Scope Process Bypass`
:::

## 二、构建固件（2 分钟）

```powershell
# 构建固件和触摸示例应用
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu `
  --board_search_path=../boards `
  PREINSTALL=hello `
  STORAGE_IMAGE=1 `
  -j8
```

**构建产物位置**：`project/build_sf32lb57-epd-gyd7n6_hcpu/`

| 文件 | 说明 |
|------|------|
| `main.bin/` | HCPU 主固件 |
| `acpu/acpu.bin` | ACPU 计算固件 |
| `fs_root.bin` | 文件系统镜像（包含 hello 示例） |
| `uart_download.bat` | 烧录脚本 |

::: warning 注意
`STORAGE_IMAGE=1` 会覆盖设备内部文件系统，包括设置和应用数据。正式开发时不要使用此参数。
:::

## 三、烧录固件（2 分钟）

### 1. 连接设备

- 使用 USB 转串口线连接开发板的 UART1 接口
- 在 Windows 设备管理器中查看 COM 端口号（如 COM18）

### 2. 执行烧录

```powershell
# 运行烧录脚本
.\project\build_sf32lb57-epd-gyd7n6_hcpu\uart_download.bat

# 根据提示输入 COM 端口号（例如：18）
```

烧录完成后，设备自动重启并显示桌面。

## 四、验证运行

设备启动后应该看到：

1. **桌面界面** - 显示"触摸示例"应用图标
2. **串口输出** - 显示 `msh />` 命令提示符（波特率：1000000）

### 运行示例应用

1. 在桌面点击"触摸示例"（英文界面显示为 Touch Demo）
2. 点击"计数按钮"，观察数字增加
3. 点击"详细信息"进入子页面
4. 点击返回按钮回到主页面（计数应保持）

## 下一步

::: tip 推荐学习路径
- 📱 [创建第一个应用](../development/first-application.md) - 从示例开始开发自己的应用
- 🏗️ [系统架构总览](../architecture/architecture.md) - 理解平台的整体设计
- 📚 [应用开发指南](../development/application-development.md) - 深入学习开发技巧
- 🔌 [API 参考](../reference/api-reference.md) - 查阅完整的平台接口
:::

## 常见问题

### Q: 烧录失败，停在"等待芯片连接"？
**A**: 按开发板上的复位键，让下载工具重新建立连接。

### Q: 编译报错找不到某些文件？
**A**: 确保正确执行了 `git submodule update --init --recursive` 和 `python tools/prepare_build.py`。

### Q: 串口无法打开？
**A**: 关闭所有占用该 COM 口的程序（串口终端、其他下载工具等）。

### Q: 如何只更新固件不覆盖文件系统？
**A**: 构建时去掉 `STORAGE_IMAGE=1` 参数：
```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu -j8
```

## 更多资源

- **设备管理**：[设备与网页管理](device-management.md)
- **详细构建指南**：[首次运行](getting-started.md)
- **故障排查**：[问题排查](../troubleshooting/troubleshooting.md)
- **源码仓库**：[GitHub](https://github.com/OpenSiFli/SiFli-EPD-Platform)
