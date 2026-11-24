---
title: "应用生命周期完整指南"
---

# 应用生命周期完整指南

## 概述

本指南描述动态应用的完整生命周期，包括前台页面的状态转换、后台任务管理以及资源清理流程。理解生命周期对于正确实现应用至关重要。

平台基于 SiFli SDK 的 app_fwk 框架管理应用页面，通过生命周期回调通知应用状态变化。应用必须在适当的时机初始化资源、响应状态切换并完成清理。

## 生命周期状态

### 状态定义

应用页面在运行期间会经历以下状态：

| 状态 | 说明 |
|------|------|
| **未创建** | 页面尚未启动，不占用内存 |
| **已启动** (Started) | 页面已创建，控件和资源已初始化，但在后台 |
| **已恢复** (Resumed) | 页面在前台显示，接收用户输入和更新 |
| **已暂停** (Paused) | 页面进入后台，保留内存和状态但暂停更新 |
| **已停止** (Stopped) | 页面销毁，释放所有资源和内存 |

### 状态转换图

```
        [未创建]
            |
            | app_main() 注册
            | gui_app_regist_msg_handler_ext()
            ↓
    ┌─→ [已启动] ←──────┐
    |   ONSTART          | 返回到已有页面
    |       |            | gui_app_goback_to_page()
    |       | 进入前台   |
    |       ↓            |
    |   [已恢复] ────────┘
    |   ONRESUME
    |       |
    |       | 切换到其他页面
    |       | 被系统暂停
    |       ↓
    |   [已暂停]
    |   ONPAUSE
    |       |
    |       | 恢复前台 ←─┘
    └───────┤
            | 页面关闭
            | 内存压力
            | 应用退出
            ↓
        [已停止]
        ONSTOP
            |
            ↓
        [未创建]
```

## 生命周期回调

### ONSTART — 页面创建

**时机**: 页面首次创建时调用，整个生命周期中只调用一次。

**应用需要完成**:
- 创建 LVGL 屏幕和所有控件
- 初始化页面私有数据结构
- 创建页面专属的定时器（如果需要）
- 加载静态资源（图标、字体引用）
- 设置事件监听器
- 建立输入组和按键监听

**注意事项**:
- 此时页面在后台，尚未显示
- 避免执行耗时操作，快速完成初始化
- 不要在这里启动前台更新逻辑
- 页面内存已由框架分配并清零

**代码示例**:

```c
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title_label;
    lv_obj_t *content_label;
    lv_obj_t *button;
    lv_timer_t *update_timer;
    uint32_t locale_revision;
    int counter;
} page_data_t;

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    page_data_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART)
    {
        // 1. 准备屏幕
        gui_app_close_anim();
        lv_obj_t *screen = lv_screen_active();
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        page->screen = screen;
        
        // 2. 创建控件
        page->title_label = epd_app_label(screen, "", 80, 81, 540, 
                                         epd_app_font_role(EPD_FONT_TITLE));
        page->content_label = epd_app_label(screen, "", 80, 300, 524,
                                           epd_app_font_role(EPD_FONT_BODY));
        page->button = epd_app_button(screen, 80, 500, 524, 100,
                                     "点击", button_clicked, page);
        
        // 3. 创建返回按钮
        epd_app_back_button(screen);
        
        // 4. 初始化页面数据
        page->counter = 0;
        page->locale_revision = 0;
        
        // 5. 注册事件监听（语言和显示设置变化）
        epd_app_ui_listen(screen, 
                         EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_DISPLAY,
                         settings_changed_callback, page);
        
        // 6. 更新内容（但不启动定时器）
        update_page_content(page);
    }
}
```

### ONRESUME — 页面恢复到前台

**时机**: 
- 页面从 ONSTART 进入前台
- 页面从 ONPAUSE 恢复到前台
- 子页面返回时

**应用需要完成**:
- 刷新显示内容（读取最新业务数据）
- 检查语言、界面尺度、内容区域变化
- 启动前台更新定时器
- 恢复动画和前台订阅
- 检查并处理 Intent 参数（如果从外部启动）

**注意事项**:
- 此时页面对用户可见并可交互
- 检查 `epd_locale_revision()` 和 `epd_ui_scale_revision()` 判断是否需要重新布局
- 从后台任务复制业务快照，不要直接持有后台对象指针
- ONRESUME 可能被多次调用

**代码示例**:

```c
static void update_page_content(page_data_t *page)
{
    // 检查语言变化
    uint32_t revision = epd_locale_revision();
    if (page->locale_revision != revision)
    {
        epd_app_text(page->title_label, epd_get_str(my_app_title));
        page->locale_revision = revision;
    }
    
    // 更新动态内容
    char text[64];
    snprintf(text, sizeof(text), "计数: %d", page->counter);
    epd_app_text(page->content_label, text);
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    page_data_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONRESUME)
    {
        // 1. 刷新页面内容
        update_page_content(page);
        
        // 2. 启动定时器（如果需要周期性更新）
        if (page->update_timer)
        {
            lv_timer_resume(page->update_timer);
        }
        else
        {
            page->update_timer = lv_timer_create(timer_callback, 1000, page);
        }
        
        // 3. 检查尺度变化
        static uint32_t last_scale_revision = 0;
        uint32_t scale_revision = epd_ui_scale_revision();
        if (last_scale_revision != scale_revision)
        {
            relayout_page(page);
            last_scale_revision = scale_revision;
        }
    }
}
```

### ONPAUSE — 页面进入后台

**时机**:
- 用户打开了其他页面
- 系统弹出覆盖层
- 页面即将被停止前

**应用需要完成**:
- 暂停前台更新定时器
- 暂停动画和前台订阅
- 保存当前编辑状态（如果有）
- 停止音频播放或其他资源密集型操作

**注意事项**:
- 页面对象和内存仍然保留
- 不要删除控件或释放页面资源
- 后台任务可以继续运行
- ONPAUSE 之后可能收到 ONRESUME（恢复）或 ONSTOP（销毁）

**代码示例**:

```c
static void page_message(gui_app_msg_type_t message, void *parameter)
{
    page_data_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONPAUSE)
    {
        // 1. 暂停定时器
        if (page->update_timer)
        {
            lv_timer_pause(page->update_timer);
        }
        
        // 2. 保存编辑状态（如果需要）
        if (page->has_unsaved_changes)
        {
            save_draft(page);
        }
        
        // 3. 停止音频或动画
        // stop_audio_playback();
        // lv_anim_delete(page->animation_target, NULL);
    }
}
```

### ONSTOP — 页面销毁

**时机**:
- 用户关闭页面（返回、退出应用）
- 系统因内存压力回收页面
- 应用卸载或更新
- 应用主动调用 `gui_app_self_exit()`

**应用需要完成**:
- 删除自建的 LVGL 定时器
- 解除事件监听和输入监听
- 释放页面分配的资源（内存、文件句柄）
- 关闭网络请求
- 清理缓存

**注意事项**:
- **不要删除** `screen` 对象，框架会自动清理
- **不要释放** 页面内存（`gui_app_this_page_memory()` 返回的指针），框架管理
- 必须完成所有清理工作，不能遗留资源或回调
- 如果注册了 `epd_app_ui_listen()`，必须在此解除

**代码示例**:

```c
static void page_message(gui_app_msg_type_t message, void *parameter)
{
    page_data_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTOP)
    {
        // 1. 删除定时器
        if (page->update_timer)
        {
            lv_timer_delete(page->update_timer);
            page->update_timer = NULL;
        }
        
        // 2. 解除事件监听
        epd_app_ui_listen(page->screen, 0, NULL, NULL);
        
        // 3. 解除输入监听
        epd_app_input_unlisten(page->screen);
        
        // 4. 释放自己分配的资源
        if (page->allocated_buffer)
        {
            rt_free(page->allocated_buffer);
            page->allocated_buffer = NULL;
        }
        
        // 5. 关闭文件（如果有）
        // close_file_handles(page);
        
        // 注意：不要删除 screen，不要释放 page 本身
    }
}
```

## 应用入口

### app_main() 函数

应用模块必须导出 `app_main()` 函数作为入口点：

```c
#include "platform/epd_app.h"

// 声明应用 ID
EPD_APP_DEFINE("myapp");

// 页面回调函数
static void page_message(gui_app_msg_type_t message, void *parameter)
{
    // 处理生命周期事件
}

// 应用入口
int app_main(intent_t intent)
{
    // 可以从 intent 获取启动参数
    const char *action = intent_get_string(intent, "action");
    
    // 注册页面回调
    // 参数: 应用ID, 回调函数, 参数, 页面私有数据大小
    return gui_app_regist_msg_handler_ext(
        "myapp", 
        page_message, 
        NULL,
        sizeof(page_data_t)
    );
}
```

### 模块初始化与清理

应用可以提供模块级别的初始化和清理函数：

```c
// 模块加载时调用（在 app_main 之前）
static int module_init(void)
{
    // 初始化全局资源
    // 注意：此时页面尚未创建
    LOG_I("myapp", "Module initialized");
    return 0;
}
MODULE_INIT_DEF(module_init)

// 模块卸载时调用（在所有页面销毁后）
static void module_cleanup(void)
{
    // 清理全局资源
    LOG_I("myapp", "Module cleanup");
}
MODULE_CLEANUP_DEF(module_cleanup)
```

## 页面导航

### 创建子页面

在当前应用中创建新页面：

```c
static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    // 子页面的生命周期回调
}

static void open_detail_clicked(lv_event_t *event)
{
    // 创建子页面
    gui_app_create_page_for_app_ext(
        "myapp",           // 应用 ID
        "detail",          // 页面 ID（应用内唯一）
        detail_message,    // 子页面回调
        NULL,              // 参数
        sizeof(detail_page_data_t)  // 子页面私有数据大小
    );
}
```

### 返回导航

```c
// 返回上一页（关闭当前页）
gui_app_goback();

// 返回到指定页面（关闭中间的所有页面）
gui_app_goback_to_page("main");

// 退出整个应用
gui_app_self_exit();
```

## 后台任务生命周期

### 后台服务声明

应用可以声明后台服务在页面关闭后继续运行：

```c
// 后台线程入口
static void background_run(epd_service_t *service)
{
    LOG_I("myapp", "Background service started");
    
    while (!epd_service_cancelled(service))
    {
        // 等待事件（1 秒超时）
        uint32_t events = epd_service_wait(service, 1000);
        
        if (events & EPD_SERVICE_STOP)
        {
            // 收到停止请求
            break;
        }
        
        if (events & EPD_SERVICE_NETWORK)
        {
            // 网络状态变化
            handle_network_change();
        }
        
        // 执行后台任务
        process_background_task();
    }
    
    // 清理资源
    cleanup_background_resources();
    LOG_I("myapp", "Background service stopped");
}

// 导出后台服务描述符
const epd_background_t epd_app_background = {
    .run = background_run,
    .stack_size = 8192,      // 线程栈大小（2-32 KiB）
    .priority = 24,          // 优先级（20 到 RT_THREAD_PRIORITY_MAX-1）
    .network = true,         // 接收网络变化事件
};
```

### 后台任务生命周期

```
[页面启动]
    |
    | app_main() 注册成功
    ↓
[后台服务启动]  ← background: true 自动启动
    |             或 app_service_start() 手动启动
    | run() 线程运行
    |
    | ← 页面可以多次打开和关闭
    |
[页面关闭]
    |
    | 根据留存策略决定：
    | - RELEASE: 任务排空后停止（默认）
    | - STATE: 保留状态，等待再次打开
    | - TASKS: 完成持续任务后停止
    ↓
[后台服务停止]
    |
    | 1. 设置取消标志
    | 2. 唤醒 epd_service_wait()
    | 3. 等待 run() 返回
    | 4. 等待在途命令完成
    | 5. 注销网络订阅
    | 6. 释放模块引用
    ↓
[服务已停止]
```

### 后台留存策略

```c
// 在 app_main() 或页面回调中设置
int app_main(intent_t intent)
{
    // 页面关闭后保留状态，等待用户再次打开
    epd_app_background_set(EPD_APP_BACKGROUND_STATE);
    
    // 或：有持续任务时保持运行
    // epd_app_background_set(EPD_APP_BACKGROUND_TASKS);
    
    return gui_app_regist_msg_handler_ext(...);
}
```

## 完整示例

### 单页应用

```c
#include "platform/epd_app.h"
#include "platform/epd_locale.h"

EPD_APP_DEFINE("simple");

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *label;
    lv_timer_t *timer;
    int counter;
} simple_page_t;

static void timer_callback(lv_timer_t *timer)
{
    simple_page_t *page = timer->user_data;
    page->counter++;
    
    char text[64];
    snprintf(text, sizeof(text), "计数: %d", page->counter);
    epd_app_text(page->label, text);
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    simple_page_t *page = gui_app_this_page_memory();
    
    switch (message)
    {
        case GUI_APP_MSG_ONSTART:
            // 创建界面
            gui_app_close_anim();
            page->screen = lv_screen_active();
            lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
            
            page->label = epd_app_label(page->screen, "计数: 0", 
                                       80, 300, 524, epd_app_font());
            epd_app_back_button(page->screen);
            
            page->counter = 0;
            break;
            
        case GUI_APP_MSG_ONRESUME:
            // 启动定时器
            if (!page->timer)
            {
                page->timer = lv_timer_create(timer_callback, 1000, page);
            }
            else
            {
                lv_timer_resume(page->timer);
            }
            break;
            
        case GUI_APP_MSG_ONPAUSE:
            // 暂停定时器
            if (page->timer)
            {
                lv_timer_pause(page->timer);
            }
            break;
            
        case GUI_APP_MSG_ONSTOP:
            // 清理资源
            if (page->timer)
            {
                lv_timer_delete(page->timer);
                page->timer = NULL;
            }
            break;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("simple", page_message, NULL,
                                         sizeof(simple_page_t));
}
```

### 多页应用

```c
#include "platform/epd_app.h"

EPD_APP_DEFINE("multipage");

// === 主页 ===
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *open_button;
} main_page_t;

static void detail_message(gui_app_msg_type_t message, void *parameter);

static void open_detail_clicked(lv_event_t *event)
{
    gui_app_create_page_for_app_ext("multipage", "detail",
                                    detail_message, NULL,
                                    sizeof(detail_page_t));
}

static void main_message(gui_app_msg_type_t message, void *parameter)
{
    main_page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART)
    {
        gui_app_close_anim();
        page->screen = lv_screen_active();
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        
        page->title = epd_app_label(page->screen, "主页",
                                   80, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
        page->open_button = epd_app_button(page->screen, 80, 300, 524, 100,
                                          "打开详情", open_detail_clicked, NULL);
        
        epd_app_back_button(page->screen);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        // 从详情页返回时刷新
        LOG_I("multipage", "Main page resumed");
    }
}

// === 详情页 ===
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *content;
} detail_page_t;

static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    detail_page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART)
    {
        gui_app_close_anim();
        page->screen = lv_screen_active();
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        
        page->content = epd_app_label(page->screen, "这是详情页内容",
                                     80, 300, 524, epd_app_font());
        
        epd_app_back_button(page->screen);
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("multipage", main_message, NULL,
                                         sizeof(main_page_t));
}
```

## 最佳实践

### 资源管理

1. **在 ONSTART 创建，ONSTOP 释放**
   - 控件、定时器、事件监听器遵循创建-销毁对称
   - 页面内存由框架管理，不要手动释放

2. **在 ONRESUME 启动，ONPAUSE 暂停**
   - 前台更新定时器、动画、周期性任务
   - 保持页面内存和状态，只暂停活动

3. **检查状态变化**
   - 语言: `epd_locale_revision()`
   - 界面尺度: `epd_ui_scale_revision()`
   - 内容区域: `epd_ui_content_area()`

### 性能优化

1. **快速完成生命周期回调**
   - 避免在回调中执行耗时操作
   - 使用后台线程处理网络请求和文件 I/O

2. **延迟加载资源**
   - 在 ONSTART 只创建必要控件
   - 在 ONRESUME 加载动态内容

3. **及时释放资源**
   - ONPAUSE 时暂停不必要的更新
   - ONSTOP 时确保所有资源都被释放

### 常见错误

❌ **错误**: 在 ONSTOP 中删除 screen

```c
if (message == GUI_APP_MSG_ONSTOP)
{
    lv_obj_delete(page->screen);  // 错误！框架会自动清理
}
```

✅ **正确**: 只清理自己创建的资源

```c
if (message == GUI_APP_MSG_ONSTOP)
{
    if (page->timer) lv_timer_delete(page->timer);
    epd_app_ui_listen(page->screen, 0, NULL, NULL);
}
```

---

❌ **错误**: 在 ONPAUSE 中释放页面资源

```c
if (message == GUI_APP_MSG_ONPAUSE)
{
    lv_timer_delete(page->timer);  // 错误！可能还会 ONRESUME
    page->timer = NULL;
}
```

✅ **正确**: 只暂停，保留资源

```c
if (message == GUI_APP_MSG_ONPAUSE)
{
    if (page->timer) lv_timer_pause(page->timer);  // 暂停，不删除
}
```

---

❌ **错误**: 忘记解除监听器

```c
if (message == GUI_APP_MSG_ONSTART)
{
    epd_app_ui_listen(page->screen, EPD_SERVICE_UI_SETTINGS, callback, page);
}
// ONSTOP 中忘记调用 epd_app_ui_listen(page->screen, 0, NULL, NULL)
```

✅ **正确**: 对称地注册和解除

```c
if (message == GUI_APP_MSG_ONSTART)
{
    epd_app_ui_listen(page->screen, EPD_SERVICE_UI_SETTINGS, callback, page);
}
else if (message == GUI_APP_MSG_ONSTOP)
{
    epd_app_ui_listen(page->screen, 0, NULL, NULL);  // 解除监听
}
```

## 调试技巧

### 日志跟踪

在每个生命周期回调中添加日志：

```c
static void page_message(gui_app_msg_type_t message, void *parameter)
{
    const char *msg_name[] = {"ONSTART", "ONRESUME", "ONPAUSE", "ONSTOP"};
    LOG_I("myapp", "Page lifecycle: %s", msg_name[message]);
    
    page_data_t *page = gui_app_this_page_memory();
    // ... 处理逻辑
}
```

### 资源泄漏检查

确保每个分配的资源都有对应的释放：

```c
// 创建资源时记录
if (message == GUI_APP_MSG_ONSTART)
{
    page->buffer = rt_malloc(1024);
    LOG_D("myapp", "Allocated buffer: %p", page->buffer);
}

// 释放时验证
if (message == GUI_APP_MSG_ONSTOP)
{
    if (page->buffer)
    {
        LOG_D("myapp", "Freeing buffer: %p", page->buffer);
        rt_free(page->buffer);
        page->buffer = NULL;
    }
    else
    {
        LOG_W("myapp", "Buffer already freed or never allocated");
    }
}
```

## 参考

- [动态应用开发](application-development.md) — 开发环境、构建和安装
- [应用生命周期](application-lifecycle.md) — 后台任务和网络订阅
- [应用内存](application-memory.md) — 内存管理和后台留存策略
- [触摸界面](touch-ui.md) — 界面缩放、输入和手势
- [多语言开发](multilingual.md) — 国际化和本地化
- [API 参考](../reference/api-reference.md) — 完整接口说明
