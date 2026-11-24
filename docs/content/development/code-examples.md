---
title: "代码示例集"
---

# 代码示例集

本文档提供 SiFli EPD Platform 应用开发的常见场景代码示例。所有示例都是可运行的真实代码。

## 基础应用结构

### 最小应用

```c
#include "platform/epd_app.h"
#include "platform/epd_locale.h"
#include "app_service.h"

EPD_APP_DEFINE("myapp");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *label;
} page_t;

static void page_message(gui_app_msg_type_t message, void *param) {
    page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART) {
        lv_obj_t *screen = lv_screen_active();
        page->screen = screen;
        
        // 声明使用标准页眉
        epd_app_header(screen);
        
        // 创建标签
        const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
        page->label = epd_app_label(screen, "Hello, World!", 
                                    80, 300, 524, font);
        
        // 创建返回按钮
        epd_app_back_button(screen);
    }
}

int app_main(intent_t intent) {
    return gui_app_regist_msg_handler_ext("myapp", page_message, 
                                          NULL, sizeof(page_t));
}
```

**对应的 app.json**：
```json
{
  "id": "myapp",
  "name": "My App",
  "version": 1,
  "data_version": 0,
  "localized_names": {
    "zh": "我的应用",
    "en": "My App"
  }
}
```

## 界面开发示例

### 响应式布局（支持缩放）

```c
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *content;
    lv_obj_t *button;
    uint32_t scale_revision;
} page_t;

static void layout_page(page_t *page) {
    // 获取内容区域
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    
    // 使用缩放转换尺寸
    int gap = epd_ui_px(24);
    int padding = epd_ui_px(40);
    int button_height = epd_ui_touch(100);  // 触摸友好尺寸
    
    int x = area.x1 + padding;
    int y = area.y1 + gap;
    int width = area.x2 - area.x1 + 1 - padding * 2;
    
    // 布局标题
    const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
    lv_obj_set_pos(page->title, x, y);
    lv_obj_set_size(page->title, width, title_font->line_height);
    y += title_font->line_height + gap;
    
    // 布局内容
    lv_obj_set_pos(page->content, x, y);
    lv_obj_set_width(page->content, width);
    int content_height = lv_obj_get_height(page->content);
    y += content_height + gap;
    
    // 布局按钮
    lv_obj_set_pos(page->button, x, y);
    lv_obj_set_size(page->button, width, button_height);
    
    // 记录缩放版本
    page->scale_revision = epd_ui_scale_revision();
}

static void on_settings_changed(uint32_t events, void *ctx) {
    page_t *page = ctx;
    
    // 检查缩放是否变化
    if (page->scale_revision != epd_ui_scale_revision()) {
        layout_page(page);
    }
}

static void page_message(gui_app_msg_type_t message, void *param) {
    page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART) {
        lv_obj_t *screen = lv_screen_active();
        page->screen = screen;
        epd_app_header(screen);
        
        // 创建控件
        const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
        const lv_font_t *body_font = epd_app_font_role(EPD_FONT_BODY);
        
        page->title = epd_app_label(screen, "响应式页面", 0, 0, 0, title_font);
        page->content = epd_app_label(screen, "此页面支持界面缩放", 
                                      0, 0, 0, body_font);
        page->button = epd_app_button(screen, 0, 0, 0, 0, "按钮", NULL, NULL);
        
        // 初始布局
        layout_page(page);
        
        // 监听设置变化
        epd_app_ui_listen(screen, EPD_SERVICE_UI_DISPLAY, 
                         on_settings_changed, page);
    } else if (message == GUI_APP_MSG_ONSTOP) {
        epd_app_ui_listen(page->screen, 0, NULL, NULL);
    }
}
```

### 多语言支持

**i18n/strings.json**：
```json
{
  "app_title": {
    "zh": "我的应用",
    "en": "My App"
  },
  "welcome_message": {
    "zh": "欢迎使用本应用",
    "en": "Welcome to this app"
  },
  "count_text": {
    "zh": "计数: %u",
    "en": "Count: %u"
  },
  "button_increment": {
    "zh": "增加",
    "en": "Increment"
  }
}
```

**应用代码**：
```c
#include "platform/epd_i18n.h"

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *message;
    lv_obj_t *count_label;
    lv_obj_t *button;
    uint32_t locale_revision;
    unsigned int count;
} page_t;

static void update_text(page_t *page) {
    // 更新固定文本
    epd_app_text(page->title, epd_get_str(app_title));
    epd_app_text(page->message, epd_get_str(welcome_message));
    
    // 更新格式化文本
    char text[64];
    EPD_FMT_count_text(text, sizeof(text), page->count);
    epd_app_text(page->count_label, text);
    
    // 更新按钮文本
    epd_app_text(lv_obj_get_child(page->button, 0), 
                 epd_get_str(button_increment));
    
    page->locale_revision = epd_locale_revision();
}

static void on_settings_changed(uint32_t events, void *ctx) {
    page_t *page = ctx;
    
    // 检查语言是否变化
    if (page->locale_revision != epd_locale_revision()) {
        update_text(page);
    }
}

static void on_button_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    page->count++;
    
    // 只更新计数文本
    char text[64];
    EPD_FMT_count_text(text, sizeof(text), page->count);
    epd_app_text(page->count_label, text);
}
```

### 列表和滚动

```c
static void create_scrollable_list(lv_obj_t *parent) {
    // 创建滚动容器
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, 604, 800);
    lv_obj_set_pos(list, 40, 200);
    lv_obj_set_style_bg_color(list, lv_color_white(), 0);
    lv_obj_set_style_border_width(list, 2, 0);
    lv_obj_set_style_pad_all(list, epd_ui_px(10), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    // 添加列表项
    const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
    for (int i = 0; i < 20; i++) {
        char text[32];
        snprintf(text, sizeof(text), "列表项 %d", i + 1);
        
        lv_obj_t *item = epd_app_button(list, 0, 0, 584, epd_ui_touch(80),
                                        text, on_item_click, (void*)(intptr_t)i);
        lv_obj_set_style_margin_bottom(item, epd_ui_px(8), 0);
    }
}
```

## 存储操作示例

### 保存和读取配置

```c
#include "services/storage.h"
#include "services/storage_file.h"

#define CONFIG_MAGIC 0x4D594150  // "MYAP"
#define CONFIG_VERSION 1

typedef struct {
    uint32_t theme;
    uint32_t font_size;
    bool notifications_enabled;
    char username[32];
} app_config_t;

static bool save_config(const app_config_t *config) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp", 
                         STORAGE_APP_AREA_DATA, "config.bin")) {
        LOG_E("myapp", "Failed to get config path");
        return false;
    }
    
    // 使用原子保存
    rt_err_t err = storage_record_save(path, CONFIG_MAGIC, CONFIG_VERSION,
                                       config, sizeof(app_config_t));
    if (err != RT_EOK) {
        LOG_E("myapp", "Failed to save config: %d", err);
        return false;
    }
    
    LOG_I("myapp", "Config saved successfully");
    return true;
}

static bool load_config(app_config_t *config) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_DATA, "config.bin")) {
        return false;
    }
    
    // 加载配置
    rt_err_t err = storage_record_load(path, CONFIG_MAGIC, CONFIG_VERSION,
                                       config, sizeof(app_config_t));
    if (err != RT_EOK) {
        // 使用默认配置
        config->theme = 0;
        config->font_size = 16;
        config->notifications_enabled = true;
        config->username[0] = '\0';
        LOG_W("myapp", "Using default config");
        return false;
    }
    
    LOG_I("myapp", "Config loaded successfully");
    return true;
}
```

### 保存大文件（临时文件 + 原子替换）

```c
static bool save_large_file(const char *filename, const void *data, size_t size) {
    char path[256];
    char temp_path[256];
    
    // 获取目标路径
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_DATA, filename)) {
        return false;
    }
    
    // 生成临时文件路径
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    
    // 写入临时文件
    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        LOG_E("myapp", "Failed to create temp file: %s", temp_path);
        return false;
    }
    
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);
    
    if (written != size) {
        LOG_E("myapp", "Write incomplete: %zu/%zu", written, size);
        unlink(temp_path);
        return false;
    }
    
    // 原子替换
    rt_err_t err = storage_file_commit(temp_path, path);
    if (err != RT_EOK) {
        LOG_E("myapp", "Failed to commit file: %d", err);
        unlink(temp_path);
        return false;
    }
    
    return true;
}
```

## 后台服务示例

### 简单后台任务

```c
typedef struct {
    epd_service_t *service;
    bool running;
    int task_count;
} service_ctx_t;

static service_ctx_t g_service_ctx = {0};

static void service_run(epd_service_t *service) {
    g_service_ctx.service = service;
    g_service_ctx.running = true;
    
    LOG_I("myapp", "Service started");
    
    while (!epd_service_cancelled(service)) {
        // 等待事件（1 秒超时）
        uint32_t events = epd_service_wait(service, 1000);
        
        if (events & EPD_SERVICE_WAKE) {
            // 被唤醒，执行任务
            LOG_I("myapp", "Service woke up, processing task");
            process_task();
            g_service_ctx.task_count++;
            
            // 请求 UI 更新
            epd_service_ui_request(service, 100);
        }
        
        if (events & EPD_SERVICE_STOP) {
            LOG_I("myapp", "Service stop requested");
            break;
        }
    }
    
    // 清理资源
    cleanup_service();
    g_service_ctx.running = false;
    LOG_I("myapp", "Service stopped");
}

static bool ui_process(epd_service_t *service, bool stopping) {
    // 更新 UI（快速执行）
    update_ui_with_result();
    
    if (stopping) {
        // 释放 UI 资源
        cleanup_ui();
        return true;
    }
    
    return false;
}

static bool service_flush(void) {
    // 保存状态
    if (has_unsaved_data()) {
        return save_data();
    }
    return true;
}

const epd_background_t epd_app_background = {
    .run = service_run,
    .ui_process = ui_process,
    .flush = service_flush,
    .stack_size = 8192,
    .priority = 10,
    .network = false,
};

// 启动服务（UI 线程）
static void start_service(void) {
    app_service_start("myapp", &epd_app_background, 
                     rt_dlmodule_self());
}

// 停止服务（UI 线程）
static void stop_service(void) {
    app_service_stop("myapp");
}

// 唤醒服务（任何线程）
static void wake_service(void) {
    if (g_service_ctx.service) {
        epd_service_wake(g_service_ctx.service);
    }
}
```

### 网络监听服务

```c
#include "services/net/network.h"

static void network_service_run(epd_service_t *service) {
    while (!epd_service_cancelled(service)) {
        uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
        
        if (events & EPD_SERVICE_NETWORK) {
            // 网络状态变化
            network_snapshot_t state;
            network_snapshot(&state);
            
            if (state.ready) {
                LOG_I("myapp", "Network available, source=%d", state.source);
                
                // 获取休眠锁
                if (epd_service_sleep_acquire(service) == RT_EOK) {
                    // 执行网络任务
                    perform_network_task();
                    
                    // 释放休眠锁
                    epd_service_sleep_release(service);
                }
            } else {
                LOG_W("myapp", "Network unavailable");
                cancel_pending_requests();
            }
        }
    }
}

const epd_background_t epd_app_background = {
    .run = network_service_run,
    .stack_size = 8192,
    .priority = 10,
    .network = true,  // 接收网络事件
};
```

## 网络请求示例

### HTTP GET 请求

```c
#include "platform/epd_http.h"

typedef struct {
    char *response_data;
    size_t response_size;
    bool completed;
    int status_code;
} http_ctx_t;

static void http_callback(epd_http_event_t event, void *data, 
                         size_t size, void *user) {
    http_ctx_t *ctx = user;
    
    switch (event) {
        case EPD_HTTP_EVENT_DATA:
            // 接收数据
            if (data && size > 0) {
                // 重新分配缓冲区
                char *new_buf = epd_app_realloc(ctx->response_data, 
                                                ctx->response_size + size,
                                                EPD_APP_PSRAM);
                if (new_buf) {
                    memcpy(new_buf + ctx->response_size, data, size);
                    ctx->response_data = new_buf;
                    ctx->response_size += size;
                }
            }
            break;
            
        case EPD_HTTP_EVENT_COMPLETE:
            ctx->status_code = *(int*)data;
            ctx->completed = true;
            LOG_I("myapp", "HTTP request completed: %d", ctx->status_code);
            break;
            
        case EPD_HTTP_EVENT_ERROR:
            ctx->status_code = -1;
            ctx->completed = true;
            LOG_E("myapp", "HTTP request failed");
            break;
    }
}

static bool fetch_data(const char *url) {
    if (!network_ready()) {
        LOG_W("myapp", "Network not ready");
        return false;
    }
    
    http_ctx_t ctx = {0};
    
    epd_http_config_t config = {
        .url = url,
        .method = EPD_HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .callback = http_callback,
        .user_data = &ctx,
    };
    
    epd_http_session_t *session = epd_http_request(&config);
    if (!session) {
        LOG_E("myapp", "Failed to create HTTP request");
        return false;
    }
    
    // 等待完成
    while (!ctx.completed && !epd_service_cancelled(service)) {
        rt_thread_mdelay(100);
    }
    
    epd_http_session_destroy(session);
    
    // 处理响应
    if (ctx.status_code == 200 && ctx.response_data) {
        process_response(ctx.response_data, ctx.response_size);
        epd_app_free(ctx.response_data);
        return true;
    }
    
    if (ctx.response_data) {
        epd_app_free(ctx.response_data);
    }
    
    return false;
}
```

## 完整应用示例

### 计数器应用（带状态保存）

```c
#include "platform/epd_app.h"
#include "platform/epd_locale.h"
#include "platform/epd_cold_state.h"
#include "app_service.h"

EPD_APP_DEFINE("counter");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)

// 应用状态（可保存）
typedef struct {
    epd_cold_state_t cold;
    int count;
    bool dirty;
} counter_state_t;

static counter_state_t g_state = {0};

// 状态保存
static size_t state_save(void *buffer, size_t capacity) {
    if (capacity < sizeof(int)) return 0;
    memcpy(buffer, &g_state.count, sizeof(int));
    g_state.dirty = false;
    return sizeof(int);
}

// 状态恢复
static void state_restore(const void *buffer, size_t size) {
    if (size == sizeof(int)) {
        memcpy(&g_state.count, buffer, sizeof(int));
        g_state.dirty = false;
        LOG_I("counter", "State restored: count=%d", g_state.count);
    }
}

// 页面数据
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *count_label;
    lv_obj_t *inc_button;
    lv_obj_t *dec_button;
    lv_obj_t *reset_button;
    uint32_t locale_revision;
    uint32_t scale_revision;
} page_t;

static void layout_page(page_t *page) {
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    
    int gap = epd_ui_px(24);
    int x = area.x1 + epd_ui_px(40);
    int y = area.y1 + gap;
    int width = area.x2 - area.x1 + 1 - epd_ui_px(80);
    int btn_height = epd_ui_touch(100);
    
    const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
    const lv_font_t *body_font = epd_app_font_role(EPD_FONT_BODY);
    
    // 标题
    lv_obj_set_pos(page->title, x, y);
    lv_obj_set_size(page->title, width, title_font->line_height);
    y += title_font->line_height + gap * 2;
    
    // 计数显示
    lv_obj_set_pos(page->count_label, x, y);
    lv_obj_set_width(page->count_label, width);
    y += lv_obj_get_height(page->count_label) + gap * 2;
    
    // 按钮
    lv_obj_set_pos(page->inc_button, x, y);
    lv_obj_set_size(page->inc_button, width, btn_height);
    y += btn_height + gap;
    
    lv_obj_set_pos(page->dec_button, x, y);
    lv_obj_set_size(page->dec_button, width, btn_height);
    y += btn_height + gap;
    
    lv_obj_set_pos(page->reset_button, x, y);
    lv_obj_set_size(page->reset_button, width, btn_height);
    
    page->scale_revision = epd_ui_scale_revision();
}

static void update_count_display(page_t *page) {
    char text[64];
    snprintf(text, sizeof(text), "%d", g_state.count);
    epd_app_text(page->count_label, text);
}

static void update_text(page_t *page) {
    epd_app_text(page->title, "计数器");
    update_count_display(page);
    epd_app_text(lv_obj_get_child(page->inc_button, 0), "增加");
    epd_app_text(lv_obj_get_child(page->dec_button, 0), "减少");
    epd_app_text(lv_obj_get_child(page->reset_button, 0), "重置");
    page->locale_revision = epd_locale_revision();
}

static void on_inc_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    g_state.count++;
    g_state.dirty = true;
    update_count_display(page);
    epd_cold_state_checkpoint(&g_state.cold);
}

static void on_dec_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    g_state.count--;
    g_state.dirty = true;
    update_count_display(page);
    epd_cold_state_checkpoint(&g_state.cold);
}

static void on_reset_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    g_state.count = 0;
    g_state.dirty = true;
    update_count_display(page);
    epd_cold_state_checkpoint(&g_state.cold);
}

static void on_settings_changed(uint32_t events, void *ctx) {
    page_t *page = ctx;
    if (page->locale_revision != epd_locale_revision()) {
        update_text(page);
    }
    if (page->scale_revision != epd_ui_scale_revision()) {
        layout_page(page);
    }
}

static void page_message(gui_app_msg_type_t message, void *param) {
    page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART) {
        lv_obj_t *screen = lv_screen_active();
        page->screen = screen;
        
        epd_app_header(screen);
        epd_app_back_button(screen);
        
        const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
        const lv_font_t *body_font = epd_app_font_role(EPD_FONT_BODY);
        
        page->title = epd_app_label(screen, "", 0, 0, 0, title_font);
        page->count_label = epd_app_centered_label(screen, "", 0, 0, 0,
            epd_app_font_style(EPD_FONT_BODY, EPD_FONT_STYLE_BOLD));
        lv_obj_set_style_text_font(page->count_label, 
            epd_app_font_style(EPD_FONT_TITLE, 0), 0);
        
        page->inc_button = epd_app_button(screen, 0, 0, 0, 0, "",
                                          on_inc_click, page);
        page->dec_button = epd_app_button(screen, 0, 0, 0, 0, "",
                                          on_dec_click, page);
        page->reset_button = epd_app_button(screen, 0, 0, 0, 0, "",
                                            on_reset_click, page);
        
        page->locale_revision = epd_locale_revision() - 1;
        update_text(page);
        layout_page(page);
        
        epd_app_ui_listen(screen, 
            EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_DISPLAY,
            on_settings_changed, page);
            
    } else if (message == GUI_APP_MSG_ONRESUME) {
        epd_app_header_refresh(page->screen);
        on_settings_changed(0, page);
        
    } else if (message == GUI_APP_MSG_ONSTOP) {
        epd_app_ui_listen(page->screen, 0, NULL, NULL);
    }
}

int app_main(intent_t intent) {
    // 初始化状态保存
    if (!epd_cold_state_open(&g_state.cold, "counter", "state",
                             state_save, state_restore)) {
        LOG_E("counter", "Failed to open cold state");
        return -1;
    }
    
    return gui_app_regist_msg_handler_ext("counter", page_message,
                                          NULL, sizeof(page_t));
}
```

## 相关文档

- **API 参考**：[应用接口参考](../reference/api-reference.md)
- **开发指南**：[应用开发](../development/application-development.md)
- **最佳实践**：[开发最佳实践](../development/best-practices.md)
