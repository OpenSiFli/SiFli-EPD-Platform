---
title: "核心概念"
---

# 核心概念

理解 SiFli EPD Platform 的核心概念，是高效开发应用的基础。本文介绍平台的关键设计理念和术语。

## 平台架构模型

### 双核协作架构

SiFli EPD Platform 采用**双核分工架构**：

```
┌─────────────────────────────────────────────┐
│              动态应用层                        │
│   (books, weather, ai_demo, notes, ...)     │
├─────────────────────────────────────────────┤
│            平台固件 (HCPU)                    │
│  • 用户交互（触摸、按键）                       │
│  • 文件系统访问                                │
│  • 网络、蓝牙、音频服务                         │
│  • 应用生命周期管理                            │
├─────────────────────────────────────────────┤
│          计算固件 (ACPU)                      │
│  • 文档解析（EPUB、TXT、Markdown）             │
│  • 字体渲染和字形生成                          │
│  • 分页排版计算                                │
│  • 图像处理和页面合成                          │
└─────────────────────────────────────────────┘
```

**关键特点**：
- **HCPU**（主处理器）：负责界面、交互和平台服务
- **ACPU**（协处理器）：负责计算密集型任务（文档解析、排版、渲染）
- **跨核通信**：通过统一的计算组件接口进行数据交换

::: tip 为什么需要双核？
墨水屏设备需要复杂的文档处理和页面渲染，将这些计算密集任务交给 ACPU，可以让 HCPU 保持响应灵敏，提升用户体验。
:::

## 动态应用模型

### 什么是动态应用？

动态应用是**可独立安装和卸载的程序包**（`.sap` 格式），无需重新烧录固件即可添加新功能。

```
应用包 (.sap)
├── <app_id>.so          # 应用二进制代码
├── app.json             # 应用清单（ID、名称、版本、权限）
├── i18n/                # 多语言资源
│   └── strings.json
└── assets/              # 图标和资源
    └── icon.png
```

### 应用生命周期

```
[安装] → [加载] → [启动] → [运行] → [暂停/恢复] → [停止] → [卸载]
   ↓        ↓       ↓        ↓          ↓           ↓        ↓
 文件系统   加载    app_    页面      后台任务     保存     释放
          模块    main()   显示       可继续      状态     内存
```

**关键状态**：
1. **ONSTART** - 应用首次启动，创建页面
2. **ONRESUME** - 应用从后台恢复到前台
3. **ONPAUSE** - 应用进入后台（但未停止）
4. **ONSTOP** - 应用停止，释放 UI 资源
5. **后台服务** - 即使页面关闭，服务仍可运行

::: info 状态保存
应用可以声明"冷状态"保存点，在内存压力或正常退出时保存数据，下次启动时自动恢复。
:::

## 内存管理模型

### 内存分区

```
PSRAM 总计 16 MiB
├── HCPU: 13.5 MiB
│   ├── 应用 PSRAM 池       # 文本、图像、缓存
│   ├── LVGL 内存池        # UI 对象
│   └── 系统内存
└── ACPU: 2.5 MiB
    └── 计算组件私有内存    # 解析、排版、渲染工作区
```

### 内存分配接口

```c
// 从应用 PSRAM 池分配（推荐用于大数据）
void* buffer = epd_app_alloc(size, EPD_APP_PSRAM);

// LVGL 对象（自动管理）
lv_obj_t* obj = lv_obj_create(parent);

// 释放
epd_app_free(buffer);
lv_obj_delete(obj);
```

**内存压力处理**：
- 平台会在内存不足时，自动回收后台应用
- 应用需要实现 `flush()` 接口来保存状态
- 前台应用受到保护，不会被强制回收

## 存储模型

### 双存储架构

```
/flash (内部 Flash, 4.625 MiB)
├── apps/                    # 已安装应用
│   ├── books.sap
│   └── notes.sap
├── data/                    # 应用数据
│   ├── books/
│   └── weather/
└── cache/                   # 可重建缓存

/sdcard (TF 卡, 外部存储)
├── Books/                   # 电子书
├── Notes/                   # 笔记内容
├── Music/                   # 音乐文件
└── apps/                    # 安装包或应用
```

### 存储最佳实践

| 数据类型 | 推荐位置 | 原因 |
|---------|---------|------|
| 应用程序 | `/flash/apps/` | 快速加载，不受 TF 卡拔出影响 |
| 用户内容 | `/sdcard/` | 容量大，易于电脑访问 |
| 配置数据 | `/flash/data/<app_id>/` | 持久保存，不易丢失 |
| 缓存数据 | `/flash/cache/<app_id>/` | 可重建，定期清理 |

## 显示与刷新模型

### 墨水屏刷新策略

```
内容变化 → 刷新协调器 → 选择刷新模式
                         ├─ 局部刷新（快速，可能残影）
                         └─ 全局刷新（慢速，完全清除残影）
```

**刷新模式**：
- **局部刷新（Partial）**：约 300-500ms，用于频繁更新
- **全局刷新（Full）**：约 1-2 秒，用于清除残影
- **自动策略**：平台根据变化内容和历史刷新次数自动选择

### 刷新优化建议

```c
// 请求一次全刷（清除残影）
epd_app_request_full_refresh();

// 检查刷新是否完成（用于资源释放）
if (epd_app_refresh_done()) {
    // 可以安全释放旧资源
}

// 获取刷新序号（用于变化检测）
uint32_t submission = epd_app_refresh_submission();
```

## 计算组件模型

### 组件接入流程

```
应用调用 → HCPU 客户端 → 跨核传输 → ACPU 组件 → 返回结果
   ↓                                      ↓
读取文件                              执行计算
提供数据       ←── 资源请求 ──         (解析、排版、渲染)
```

**典型计算流程**：
1. **查询能力** - 检查组件是否可用
2. **OPEN** - 建立计算会话
3. **STEP** - 分步执行（可暂停请求资源）
4. **资源请求** - ACPU 请求数据，HCPU 供数
5. **CLOSE** - 关闭会话，释放资源

### 阅读计算流水线

```
打开书籍 → 文档转换 → 字体准备 → 分页排版 → 页面合成 → 显示
   ↓          ↓          ↓          ↓          ↓         ↓
 文件路径   文本+结构   字形位图   行布局     像素合成   墨水屏
 (HCPU)    (ACPU)     (ACPU)     (ACPU)     (ACPU)    (HCPU)
```

## 服务模型

### 后台服务

应用可以导出**后台服务**，在页面关闭后继续运行：

```c
// 定义后台服务
static void service_run(epd_service_t* service) {
    while (!epd_service_cancelled(service)) {
        // 执行后台任务
        process_data();
        
        // 等待事件或超时
        uint32_t events = epd_service_wait(service, timeout_ms);
        
        if (events & EPD_SERVICE_NETWORK) {
            // 网络状态变化
        }
    }
}

static const epd_background_t my_service = {
    .run = service_run,
    .stack_size = 8192,
    .priority = 10,
    .network = true,  // 接收网络变化通知
    // ...
};

// 导出服务
const epd_background_t epd_app_background = my_service;
```

**服务特点**：
- 在独立线程中运行
- 可以接收网络变化事件
- 支持内存压力时保存并退出
- 通过 UI 步进与界面交互

## 多语言模型

### 本地化流程

```
i18n/strings.json → 构建时生成宏 → 运行时查询 → 显示对应语言文本
     ↓                   ↓                ↓              ↓
  翻译键值对         EPD_STR_xxx       epd_get_str()   中文/英文
```

**示例**：

```json
// i18n/strings.json
{
  "hello_title": {
    "zh": "触摸示例",
    "en": "Touch Demo"
  }
}
```

```c
// 应用代码
const char* text = epd_get_str(hello_title);  // 根据系统语言自动选择
```

## 网络与在线服务

### 网络服务模型

```
应用 → 网络服务 → 上行接口选择 → 实际连接
        ↓            ↓               ↓
     订阅通知    WLAN / PAN    Wi-Fi / 蓝牙网络共享
```

**网络优先级**：
1. **WLAN**（Wi-Fi）- 优先使用
2. **PAN**（蓝牙网络共享）- WLAN 不可用时回退

**状态查询**：
```c
network_snapshot_t state;
network_snapshot(&state);

if (state.ready) {
    // 网络就绪，可以发起请求
}
```

## 下一步学习

根据你的开发需求，选择对应的深入文档：

- **应用开发者** → [第一个应用](../development/first-application.md)
- **理解架构** → [系统架构总览](../architecture/architecture.md)
- **接口查询** → [API 参考](../reference/api-reference.md)
- **平台扩展** → [扩展平台](../contributing/platform-development.md)
