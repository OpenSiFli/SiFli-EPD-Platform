---
title: "ADR-003: 技术栈选型"
---

# ADR-003: 技术栈选型

## 状态

已采纳

## 背景

构建墨水屏阅读平台需要选择合适的 RTOS、GUI 框架、文件系统和网络栈。选型需要平衡以下因素：

1. **硬件适配**：必须支持 SiFli SF32LB57 芯片
2. **资源占用**：在 16 MiB PSRAM、454 KiB SRAM 的约束下高效运行
3. **生态成熟度**：有活跃的社区、丰富的驱动和组件
4. **许可证友好**：适合商业产品开发
5. **开发效率**：降低从零构建系统的时间成本

## 决策

| 组件类别 | 选择 | 版本 | 许可证 |
| --- | --- | --- | --- |
| **RTOS** | RT-Thread | 通过 SiFli SDK 2.5.x 集成 | Apache 2.0 |
| **GUI 框架** | LVGL | 9.x | MIT |
| **内部文件系统** | LittleFS | SDK 集成版本 | BSD 3-Clause |
| **TF 卡文件系统** | FAT | SDK 集成版本 | - |
| **网络栈** | lwIP | SDK 集成版本 | BSD |
| **蓝牙栈** | SiFli SDK 蓝牙栈 | SDK 集成版本 | 专有 |
| **USB** | CherryUSB | SDK 集成版本 | Apache 2.0 |
| **SDK** | SiFli SDK | 2.5.x (EPD 分支) | 专有 + 开源组件 |

**关键依赖关系**：
```
SiFli SDK 2.5.x (EPD 分支)
├── RT-Thread (RTOS 核心)
├── LittleFS (内部存储)
├── FAT (TF 卡)
├── lwIP (网络栈)
├── 蓝牙协议栈
└── 板级驱动 (触摸、墨水屏、Wi-Fi、音频)

项目层
├── LVGL 9 (GUI)
├── CherryUSB (USB 读卡器)
├── TinyTTF (字体解析，SDK 修复版)
└── 各类第三方库 (HTTP、JSON、图像解码等)
```

## 理由

### RT-Thread：RTOS 的选择

**为何选择 RT-Thread**：

1. **SDK 官方支持**
   - SiFli SDK 基于 RT-Thread 构建
   - 驱动、蓝牙栈、电源管理已适配 RT-Thread 接口
   - 切换其他 RTOS 需要重写大量板级代码

2. **资源占用适中**
   - 内核 ROM < 10 KiB，RAM < 2 KiB（最小配置）
   - 相比 FreeRTOS 功能更丰富，相比 Linux 资源占用更低

3. **生态丰富**
   - 支持多种文件系统、网络栈、USB 栈
   - 有活跃的中文社区和文档

4. **许可证友好**
   - Apache 2.0 许可证，商业使用无需开源

**对比其他 RTOS**：

| RTOS | 优点 | 缺点 | 未选择原因 |
| --- | --- | --- | --- |
| FreeRTOS | 极简、广泛使用 | 功能较少，需要自行集成文件系统等 | SDK 未基于 FreeRTOS，移植成本高 |
| Zephyr | 现代设计、安全性强 | 学习曲线陡峭、社区较小 | SDK 不支持，重写驱动成本太高 |
| NuttX | 类 POSIX、功能完整 | 资源占用较大、调试工具少 | SDK 不支持，不适合当前硬件 |

### LVGL：GUI 框架的选择

**为何选择 LVGL**：

1. **墨水屏适配性**
   - 支持自定义显示驱动和刷新策略
   - 可控制局刷和全刷时机
   - 支持灰度显示（墨水屏典型为 4-bit 或 1-bit）

2. **资源占用可控**
   - 可配置的内存池大小
   - 支持外部缓冲（使用 PSRAM）
   - 不依赖操作系统图形栈

3. **开发效率高**
   - 丰富的控件库（按钮、列表、文本框、键盘等）
   - 支持触摸和按键输入
   - 活跃的社区和示例代码

4. **许可证**
   - MIT 许可证，商业使用无限制

5. **版本选择：LVGL 9**
   - 更好的性能和内存管理
   - 更现代的 API 设计
   - 长期支持版本

**对比其他 GUI 框架**：

| 框架 | 优点 | 缺点 | 未选择原因 |
| --- | --- | --- | --- |
| emWin | 商业成熟 | 商业许可证，成本高 | 预算受限，LVGL 功能已满足需求 |
| MicroUI | 轻量 | 生态较小，控件少 | 开发效率低，需要自己实现大量控件 |
| Qt for MCU | 功能强大 | 资源占用大，需要商业许可 | 超出当前硬件能力 |

### LittleFS + FAT：文件系统的选择

**为何选择 LittleFS（内部存储）**：

1. **掉电安全**
   - 写入过程掉电不会损坏文件系统
   - 适合嵌入式设备频繁的电源状态变化

2. **磨损均衡**
   - 自动分散写入，延长 Flash 寿命

3. **资源占用低**
   - RAM 占用小（~1 KiB per mount）
   - ROM 占用适中（~15 KiB）

4. **SDK 集成**
   - SiFli SDK 已适配 LittleFS

**为何选择 FAT（TF 卡）**：

1. **兼容性**
   - Windows/macOS/Linux 通用格式
   - USB 读卡器模式下电脑可直接访问

2. **标准支持**
   - 所有读卡器、相机、手机都支持 FAT
   - 用户可在电脑上管理书籍和文件

3. **SDK 集成**
   - SiFli SDK 已提供 FAT 支持

**未选择 ext4/F2FS 等 Linux 文件系统**：
- Windows 不原生支持，用户体验差
- 实现复杂度高，ROM/RAM 占用大

### lwIP：网络栈的选择

**为何选择 lwIP**：

1. **资源占用低**
   - 适合嵌入式系统的轻量级 TCP/IP 栈
   - 可配置的内存池和缓冲策略

2. **协议支持完整**
   - TCP、UDP、DHCP、DNS、HTTP
   - 支持 IPv4（当前项目未启用 IPv6）

3. **SDK 集成**
   - SiFli SDK 已集成和测试
   - Wi-Fi 驱动已适配 lwIP 接口

4. **BSD 许可证**
   - 商业使用友好

**对比其他网络栈**：

| 网络栈 | 优点 | 缺点 | 未选择原因 |
| --- | --- | --- | --- |
| uIP | 极简 | 功能受限，性能较低 | 不满足 HTTP、HTTPS 等需求 |
| CycloneTCP | 功能完整 | 商业许可证 | 预算受限 |
| Linux TCP/IP | 最成熟 | 资源占用大，不适合 MCU | 超出硬件能力 |

## 后果

### 正面影响

1. **开发周期短**
   - SDK 集成的组件开箱即用
   - LVGL 控件丰富，UI 开发快

2. **社区支持强**
   - RT-Thread、LVGL、lwIP 都有活跃社区
   - 问题解决和示例代码易获取

3. **商业友好**
   - 核心组件均为宽松许可证
   - 无需为 RTOS 和 GUI 框架付费

4. **资源利用合理**
   - 组件资源占用符合硬件能力
   - 保留足够空间给应用逻辑

### 负面影响与应对

1. **SDK 版本依赖**
   - **问题**：被锁定在 SiFli SDK 2.5.x，升级需要测试兼容性
   - **应对**：维护 SDK 改动清单（字体修复、软件混音修复），便于后续合并

2. **LVGL 9 较新**
   - **问题**：社区示例多基于 LVGL 8，需要适配 API 变化
   - **应对**：项目代码作为 LVGL 9 + 墨水屏的参考实现

3. **TinyTTF 功能受限**
   - **问题**：SDK 集成的 TinyTTF 不支持所有 TrueType 高级特性（如复杂连字、多级替换）
   - **应对**：
     - 维护修复版本（CFF 支持、边界检查）
     - 阅读组件限定支持的字体格式和复杂度
     - 文档中明确字体要求（见 `components/compute_reader_font/README.md`）

4. **无法使用最新上游版本**
   - **问题**：RT-Thread、lwIP 受限于 SDK 集成版本，无法直接升级
   - **应对**：关注上游安全公告，必要时向 SDK 请求集成或自行 backport

## 技术栈版本演进

### 当前状态（2024）

- SiFli SDK 2.5.x (EPD 分支，基于 `jingxbw-work/SiFli-SDK`)
- RT-Thread：SDK 集成版本
- LVGL 9：项目直接集成
- LittleFS / FAT / lwIP：SDK 集成版本

### 已知上游修改

1. **TinyTTF 字体修复**（已集成到 SDK EPD 分支）
   - CFF 字体支持
   - 边界检查和错误处理

2. **软件混音修复**（已集成到 SDK EPD 分支）
   - 修复 `SOFTWARE_TX_MIX_ENABLE` 模式下的音频混合问题
   - 支持音乐播放时合成触摸提示音

### 升级路径

如需升级到 SiFli SDK 2.6+ 或 3.0：

1. 检查 RT-Thread API 兼容性
2. 验证蓝牙栈、Wi-Fi 驱动变化
3. 重新应用 TinyTTF 和音频修复（或确认已被上游接受）
4. 全量回归测试（触摸、显示、文件系统、网络、蓝牙、音频）

## 实现参考

- SDK 来源：`.gitmodules`（子模块 `SiFli-SDK/`，分支 `EPD`）
- 字体修复说明：`components/compute_reader_font/README.md`
- 音频配置：`boards/sf32lb57-epd-gyd7n6/bsp_audio.c`（启用 `SOFTWARE_TX_MIX_ENABLE`）
- LVGL 配置：`src/ui/lv_conf.h`

## 相关决策

- [ADR-001: 双核架构与任务分工](001-dual-core-architecture.md) - RT-Thread 的调度能力支持双核协作
- [ADR-002: 内存分区策略](002-memory-partitioning.md) - LittleFS 和 LVGL 的内存占用影响分区设计
- [ADR-004: 动态应用模型](004-dynamic-application-model.md) - RT-Thread 的动态加载能力支撑 `.sap` 应用
