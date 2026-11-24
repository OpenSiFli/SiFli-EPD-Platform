---
title: "开发最佳实践"
---

# 开发最佳实践

本文总结 SiFli EPD Platform 应用开发的最佳实践，帮助你编写高质量、高性能、易维护的应用。

## 内存管理最佳实践

### 1. 选择正确的内存池

```c
// ✅ 推荐：大数据使用 PSRAM
void* large_buffer = epd_app_alloc(1024 * 100, EPD_APP_PSRAM);

// ❌ 避免：大数据不要用 SRAM（资源有限）
void* buffer = epd_app_alloc(1024 * 100, EPD_APP_SRAM);

// ✅ LVGL 对象自动使用正确的池
lv_obj_t* label = lv_label_create(parent);
```

**原则**：
- **PSRAM**：文本、图像、缓存、工作区（> 4KB）
- **SRAM**：小型控制结构、实时关键数据
- **LVGL 池**：所有 UI 对象通过 LVGL API 创建

### 2. 及时释放不用的资源

```c
// ✅ 推荐：加载完成后立即释放临时缓冲
void load_config() {
    char* buffer = epd_app_alloc(file_size, EPD_APP_PSRAM);
    storage_file_read(path, buffer, file_size);
    parse_config(buffer);
    epd_app_free(buffer);  // 立即释放
}

// ❌ 避免：长期持有不再使用的大块内存
static char* global_buffer = NULL;  // 可能导致内存泄漏
```

### 3. 实现内存压力响应

```c
// ✅ 实现 flush() 保存状态
static bool my_service_flush() {
    if (!has_unsaved_data) return true;
    
    // 保存状态到文件
    bool success = save_state_to_file();
    if (success) {
        has_unsaved_data = false;
    }
    return success;
}

// 在服务定义中声明
const epd_background_t epd_app_background = {
    .flush = my_service_flush,
    // ...
};
```

### 4. 检查分配是否成功

```c
// ✅ 推荐：检查分配结果
void* buffer = epd_app_alloc(size, EPD_APP_PSRAM);
if (!buffer) {
    // 处理失败：显示错误、降级处理、稍后重试
    show_error("内存不足");
    return -ENOMEM;
}

// ❌ 避免：假设分配一定成功
void* buffer = epd_app_alloc(size, EPD_APP_PSRAM);
memcpy(buffer, data, size);  // 可能崩溃
```

## 存储操作最佳实践

### 1. 使用原子文件替换

```c
// ✅ 推荐：使用临时文件 + 原子替换
bool save_data_safe(const char* path, const void* data, size_t size) {
    char temp_path[256];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    
    // 写入临时文件
    if (!write_file(temp_path, data, size)) {
        return false;
    }
    
    // 原子替换
    return storage_file_commit(temp_path, path) == RT_EOK;
}

// ❌ 避免：直接覆盖（断电可能损坏）
write_file(path, data, size);
```

### 2. 验证存储可用性

```c
// ✅ 推荐：检查存储状态
bool save_to_sdcard(const void* data, size_t size) {
    if (!storage_available(STORAGE_VOLUME_SD)) {
        show_error("TF 卡不可用");
        return false;
    }
    
    // 检查可用空间
    storage_info_t info;
    storage_info(STORAGE_VOLUME_SD, &info);
    if (info.used + size > info.total) {
        show_error("空间不足");
        return false;
    }
    
    return write_file(path, data, size);
}
```

### 3. 处理存储变化

```c
// ✅ 监听存储变化
static uint32_t last_revision = 0;

void update_ui() {
    uint32_t current = storage_revision();
    if (current != last_revision) {
        // TF 卡状态变化，重新扫描
        reload_file_list();
        last_revision = current;
    }
}
```

### 4. 使用应用专属路径

```c
// ✅ 推荐：使用 storage_app_path 获取路径
char path[256];
if (!storage_app_path(path, sizeof(path), "myapp", 
                      STORAGE_APP_AREA_DATA, "config.json")) {
    return -ENOENT;
}
// path = "/flash/data/myapp/config.json"

// ❌ 避免：硬编码路径
const char* path = "/flash/data/myapp/config.json";  // 维护困难
```

## 界面开发最佳实践

### 1. 响应语言和缩放变化

```c
// ✅ 推荐：监听变化并重新布局
static void on_settings_changed(uint32_t events, void* context) {
    page_t* page = context;
    
    if (page->locale_revision != epd_locale_revision()) {
        // 语言变化，更新文本
        update_text(page);
        page->locale_revision = epd_locale_revision();
    }
    
    if (page->scale_revision != epd_ui_scale_revision()) {
        // 缩放变化，重新布局
        layout_page(page);
        page->scale_revision = epd_ui_scale_revision();
    }
}

// 注册监听
epd_app_ui_listen(screen, 
    EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_DISPLAY,
    on_settings_changed, page);
```

### 2. 使用平台控件创建接口

```c
// ✅ 推荐：使用平台封装接口
lv_obj_t* label = epd_app_label(parent, text, x, y, width, font);
lv_obj_t* button = epd_app_button(parent, x, y, w, h, text, callback, data);

// ✅ 更新文本使用 epd_app_text
bool updated = epd_app_text(label, new_text);
if (!updated) {
    // 分配失败，保留旧状态，稍后重试
}

// ❌ 避免：直接使用 lv_label_set_text（无变化检测）
lv_label_set_text(label, new_text);
```

### 3. 管理输入组和按键

```c
// ✅ 推荐：为每个屏幕创建输入组
static void create_page(page_t* page) {
    lv_obj_t* screen = lv_screen_active();
    
    // 创建输入组（自动管理生命周期）
    lv_group_t* group = epd_app_input_group(screen);
    
    // 按钮自动加入组
    lv_obj_t* btn = epd_app_button(screen, ...);
    
    // 注册实体键监听
    epd_app_input_listen(screen, key_callback, page);
    
    // 绑定返回按钮
    epd_app_input_back(screen, back_button);
}
```

### 4. 优化墨水屏刷新

```c
// ✅ 在合适时机请求全刷
void on_page_complete() {
    // 页面内容稳定后请求全刷，清除残影
    epd_app_request_full_refresh();
}

// ✅ 避免频繁小区域更新
void update_clock() {
    static char last_text[32] = "";
    char current_text[32];
    format_time(current_text, sizeof(current_text));
    
    // 只在实际变化时更新
    if (strcmp(last_text, current_text) != 0) {
        epd_app_text(clock_label, current_text);
        strcpy(last_text, current_text);
    }
}

// ❌ 避免：每秒强制刷新（浪费电量和寿命）
void timer_callback() {
    lv_label_set_text(label, get_time());  // 即使内容未变化
}
```

## 后台服务最佳实践

### 1. 响应取消请求

```c
// ✅ 推荐：定期检查取消状态
static void service_run(epd_service_t* service) {
    while (!epd_service_cancelled(service)) {
        // 执行一小块工作
        process_one_item();
        
        // 长时间操作前检查
        if (epd_service_cancelled(service)) {
            break;
        }
        
        // 等待事件
        uint32_t events = epd_service_wait(service, 1000);
    }
    
    // 清理资源
    cleanup_resources();
}
```

### 2. 使用休眠锁保护工作

```c
// ✅ 推荐：在执行关键任务时持有休眠锁
static void service_run(epd_service_t* service) {
    while (!epd_service_cancelled(service)) {
        uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
        
        if (events & EPD_SERVICE_WAKE) {
            // 获取休眠锁
            if (epd_service_sleep_acquire(service) != RT_EOK) {
                continue;  // 系统准备休眠，不接受新任务
            }
            
            // 执行任务
            do_important_work();
            
            // 释放休眠锁
            epd_service_sleep_release(service);
        }
    }
}
```

### 3. 通过 UI 步进更新界面

```c
// ✅ 推荐：使用 UI 步进更新界面
static bool ui_process(epd_service_t* service, bool stopping) {
    page_t* page = get_page();
    if (!page) return true;
    
    // 检查是否有新结果
    result_t* result = get_pending_result();
    if (result) {
        display_result(page, result);
        free_result(result);
    }
    
    if (stopping) {
        // 释放 UI 资源
        cleanup_ui(page);
        return true;
    }
    
    return false;
}

// 在服务定义中
const epd_background_t epd_app_background = {
    .ui_process = ui_process,
    // ...
};

// Worker 请求下一次 UI 步进
epd_service_ui_request(service, 100);  // 100ms 后调用
```

## 网络请求最佳实践

### 1. 检查网络状态

```c
// ✅ 推荐：在请求前检查网络
bool make_http_request() {
    if (!network_ready()) {
        show_error("网络不可用");
        return false;
    }
    
    // 发起请求
    return http_get(url, callback, context);
}
```

### 2. 订阅网络变化

```c
// ✅ 在后台服务中监听网络
static void service_run(epd_service_t* service) {
    while (!epd_service_cancelled(service)) {
        uint32_t events = epd_service_wait(service, timeout);
        
        if (events & EPD_SERVICE_NETWORK) {
            // 网络状态变化
            if (network_ready()) {
                // 网络恢复，重试失败的请求
                retry_pending_requests();
            }
        }
    }
}

// 服务定义中声明接收网络事件
const epd_background_t epd_app_background = {
    .network = true,  // 接收网络变化通知
    // ...
};
```

### 3. 处理请求超时和错误

```c
// ✅ 推荐：完善的错误处理
void http_callback(int status, const void* data, size_t size, void* ctx) {
    if (status == HTTP_STATUS_OK) {
        process_data(data, size);
    } else if (status == HTTP_STATUS_TIMEOUT) {
        // 超时，可以重试
        schedule_retry();
    } else if (status == HTTP_STATUS_NETWORK_ERROR) {
        // 网络错误，等待网络恢复
        mark_pending();
    } else {
        // 其他错误，显示给用户
        show_error("请求失败: %d", status);
    }
}
```

## 多语言开发最佳实践

### 1. 完整定义翻译

```json
// ✅ 推荐：为所有支持的语言提供翻译
{
  "app_title": {
    "zh": "我的应用",
    "en": "My App"
  },
  "error_network": {
    "zh": "网络连接失败，请检查网络设置",
    "en": "Network connection failed, please check network settings"
  }
}

// ❌ 避免：缺少某种语言的翻译
{
  "app_title": {
    "zh": "我的应用"
    // 缺少英文翻译
  }
}
```

### 2. 使用格式化字符串

```json
{
  "download_progress": {
    "zh": "已下载 %u/%u MB",
    "en": "Downloaded %u/%u MB"
  }
}
```

```c
// 使用格式化宏
char text[64];
EPD_FMT_download_progress(text, sizeof(text), downloaded_mb, total_mb);
epd_app_text(label, text);
```

### 3. 监听语言变化

```c
// ✅ 在 ONRESUME 和设置变化时更新文本
static void page_message(gui_app_msg_type_t message, void* param) {
    page_t* page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONRESUME) {
        // 可能语言已改变，重新加载文本
        update_all_text(page);
    }
}
```

## 错误处理最佳实践

### 1. 返回清晰的错误码

```c
// ✅ 推荐：使用标准错误码
int load_file(const char* path) {
    if (!path) return -EINVAL;
    if (!storage_available(volume)) return -ENOENT;
    if (!check_space()) return -ENOSPC;
    return RT_EOK;
}

// 调用方
int err = load_file(path);
if (err != RT_EOK) {
    switch (err) {
        case -EINVAL: show_error("参数无效"); break;
        case -ENOENT: show_error("文件不存在"); break;
        case -ENOSPC: show_error("空间不足"); break;
        default: show_error("未知错误"); break;
    }
}
```

### 2. 对用户友好的错误提示

```c
// ✅ 推荐：提供可操作的错误信息
void show_network_error() {
    show_dialog(
        "网络连接失败",
        "请检查 Wi-Fi 设置或稍后重试",
        "打开设置", open_wifi_settings,
        "稍后重试", NULL
    );
}

// ❌ 避免：技术性错误信息
show_error("errno: -ENETUNREACH");
```

## 性能优化最佳实践

### 1. 避免在 UI 线程做耗时操作

```c
// ✅ 推荐：后台处理 + UI 步进更新
static void worker_thread(void* arg) {
    // 在 worker 中执行耗时操作
    result_t* result = process_large_file();
    
    // 请求 UI 更新
    post_result_to_ui(result);
    epd_service_ui_request(service, 0);
}

static bool ui_process(epd_service_t* service, bool stopping) {
    // UI 线程快速更新界面
    result_t* result = get_result();
    if (result) {
        update_display(result);
    }
    return false;
}

// ❌ 避免：在 UI 线程阻塞
static void button_clicked(lv_event_t* e) {
    process_large_file();  // 界面会卡顿
    update_display();
}
```

### 2. 使用缓存减少重复计算

```c
// ✅ 推荐：缓存计算结果
static result_t* cached_result = NULL;
static uint32_t cache_signature = 0;

result_t* get_result(uint32_t input) {
    uint32_t current_sig = compute_signature(input);
    
    if (cached_result && cache_signature == current_sig) {
        return cached_result;  // 命中缓存
    }
    
    // 计算新结果
    free_result(cached_result);
    cached_result = expensive_computation(input);
    cache_signature = current_sig;
    
    return cached_result;
}
```

### 3. 分批处理大量数据

```c
// ✅ 推荐：分批处理
void process_large_list(item_t* items, size_t count) {
    const size_t BATCH_SIZE = 50;
    
    for (size_t i = 0; i < count; i += BATCH_SIZE) {
        size_t batch_end = (i + BATCH_SIZE < count) ? i + BATCH_SIZE : count;
        
        // 处理一批
        for (size_t j = i; j < batch_end; j++) {
            process_item(&items[j]);
        }
        
        // 检查取消
        if (epd_service_cancelled(service)) {
            break;
        }
        
        // 让出 CPU
        rt_thread_mdelay(1);
    }
}
```

## 调试最佳实践

### 1. 使用日志而非断言

```c
// ✅ 推荐：记录日志并处理错误
if (!buffer) {
    LOG_E("myapp", "Failed to allocate buffer, size=%zu", size);
    return -ENOMEM;
}

// ❌ 避免：在发布版本中使用断言
RT_ASSERT(buffer != NULL);  // 发布版本会崩溃
```

### 2. 添加有意义的日志

```c
// ✅ 推荐：包含上下文信息
LOG_I("myapp", "Loading file: %s, size=%zu bytes", path, size);
LOG_W("myapp", "Network timeout after %d attempts", retry_count);

// ❌ 避免：无意义的日志
LOG_I("myapp", "Here");  // 无法定位问题
```

## 下一步

- **代码示例**：[第一个应用](first-application.md)
- **API 参考**：[应用接口参考](../reference/api-reference.md)
- **故障排查**：[日常调试](../troubleshooting/development-debugging.md)
