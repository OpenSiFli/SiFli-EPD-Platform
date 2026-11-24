---
title: "HTTP 客户端 API"
---

# HTTP 客户端 API

平台提供简单易用的 HTTP/HTTPS 客户端。

::: tip 头文件
```c
#include "platform/epd_http.h"
```
:::

## 核心概念

### 异步请求

HTTP 请求是异步的：
1. 创建配置
2. 发起请求（立即返回）
3. 在回调中接收数据
4. 销毁会话

### 自动管理

平台自动处理：
- DNS 解析
- TLS 连接
- 重定向（可配置）
- 超时控制
- 连接池

## 快速开始

### 简单 GET 请求

```c
#include "platform/epd_http.h"

typedef struct {
    char *response_data;
    size_t response_size;
    int status_code;
    bool completed;
} http_ctx_t;

static void http_callback(epd_http_event_t event, void *data, 
                         size_t size, void *user) {
    http_ctx_t *ctx = user;
    
    switch (event) {
        case EPD_HTTP_EVENT_DATA:
            // 接收响应数据
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
            // 请求完成
            ctx->status_code = *(int*)data;
            ctx->completed = true;
            LOG_I("app", "Request completed: %d", ctx->status_code);
            break;
            
        case EPD_HTTP_EVENT_ERROR:
            // 请求失败
            ctx->status_code = -1;
            ctx->completed = true;
            LOG_E("app", "Request failed");
            break;
    }
}

bool fetch_data(const char *url) {
    // 检查网络
    if (!network_ready()) {
        return false;
    }
    
    // 准备上下文
    http_ctx_t ctx = {0};
    
    // 配置请求
    epd_http_config_t config = {
        .url = url,
        .method = EPD_HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .callback = http_callback,
        .user_data = &ctx,
    };
    
    // 发起请求
    epd_http_session_t *session = epd_http_request(&config);
    if (!session) {
        LOG_E("app", "Failed to create session");
        return false;
    }
    
    // 等待完成
    while (!ctx.completed) {
        rt_thread_mdelay(100);
    }
    
    // 销毁会话
    epd_http_session_destroy(session);
    
    // 处理结果
    if (ctx.status_code == 200) {
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

## 配置结构

### epd_http_config_t

```c
typedef struct {
    const char *url;                    // 请求 URL（必需）
    epd_http_method_t method;           // 请求方法
    const epd_http_header_t *headers;   // 请求头数组
    size_t header_count;                // 请求头数量
    const void *body;                   // 请求体数据
    size_t body_size;                   // 请求体大小
    uint32_t timeout_ms;                // 超时时间（毫秒）
    epd_http_callback_t callback;       // 事件回调
    void *user_data;                    // 用户数据
    bool follow_redirects;              // 是否跟随重定向
    int max_redirects;                  // 最大重定向次数
} epd_http_config_t;
```

### 请求方法

```c
typedef enum {
    EPD_HTTP_METHOD_GET,
    EPD_HTTP_METHOD_POST,
    EPD_HTTP_METHOD_PUT,
    EPD_HTTP_METHOD_DELETE,
    EPD_HTTP_METHOD_HEAD,
    EPD_HTTP_METHOD_PATCH
} epd_http_method_t;
```

### 请求头

```c
typedef struct {
    const char *name;   // 头名称
    const char *value;  // 头值
} epd_http_header_t;
```

**示例**：
```c
epd_http_header_t headers[] = {
    {"Content-Type", "application/json"},
    {"Authorization", "Bearer token123"},
    {"User-Agent", "MyApp/1.0"}
};

epd_http_config_t config = {
    .url = "https://api.example.com/data",
    .method = EPD_HTTP_METHOD_GET,
    .headers = headers,
    .header_count = 3,
    // ...
};
```

## 回调事件

### epd_http_event_t

```c
typedef enum {
    EPD_HTTP_EVENT_CONNECTED,    // 已连接
    EPD_HTTP_EVENT_HEADER,       // 接收响应头
    EPD_HTTP_EVENT_DATA,         // 接收响应数据
    EPD_HTTP_EVENT_COMPLETE,     // 请求完成
    EPD_HTTP_EVENT_ERROR         // 请求失败
} epd_http_event_t;
```

### 事件数据

| 事件 | data 参数 | size 参数 |
|------|----------|----------|
| `CONNECTED` | NULL | 0 |
| `HEADER` | `epd_http_header_t*` | 1 |
| `DATA` | 响应数据 | 数据大小 |
| `COMPLETE` | `int*`（状态码） | sizeof(int) |
| `ERROR` | NULL | 0 |

**完整回调示例**：
```c
static void on_http_event(epd_http_event_t event, void *data,
                         size_t size, void *user) {
    app_ctx_t *ctx = user;
    
    switch (event) {
        case EPD_HTTP_EVENT_CONNECTED:
            LOG_I("app", "Connected to server");
            break;
            
        case EPD_HTTP_EVENT_HEADER:
            {
                epd_http_header_t *header = data;
                LOG_D("app", "Header: %s = %s", header->name, header->value);
                
                // 处理特定头
                if (strcmp(header->name, "Content-Length") == 0) {
                    ctx->content_length = atoi(header->value);
                }
            }
            break;
            
        case EPD_HTTP_EVENT_DATA:
            LOG_D("app", "Received %zu bytes", size);
            append_data(ctx, data, size);
            
            // 更新进度
            ctx->downloaded += size;
            if (ctx->content_length > 0) {
                int progress = (ctx->downloaded * 100) / ctx->content_length;
                update_progress(progress);
            }
            break;
            
        case EPD_HTTP_EVENT_COMPLETE:
            {
                int status = *(int*)data;
                LOG_I("app", "Complete: HTTP %d", status);
                ctx->status_code = status;
                ctx->completed = true;
            }
            break;
            
        case EPD_HTTP_EVENT_ERROR:
            LOG_E("app", "Request error");
            ctx->status_code = -1;
            ctx->completed = true;
            break;
    }
}
```

## 发起请求

### epd_http_request

创建 HTTP 会话并发起请求。

```c
epd_http_session_t* epd_http_request(const epd_http_config_t *config);
```

**参数**：
- `config` - 请求配置

**返回值**：
- 会话句柄（成功）
- `NULL`（失败）

**注意**：
- 返回非 NULL 表示请求已受理
- 实际结果通过回调接收
- 必须调用 `epd_http_session_destroy()` 释放

### epd_http_session_destroy

销毁 HTTP 会话。

```c
void epd_http_session_destroy(epd_http_session_t *session);
```

**参数**：
- `session` - 会话句柄

**说明**：
- 如果请求未完成，会自动取消
- 阻塞等待会话完全释放
- 之后不会再收到回调

## 常见用法

### POST JSON 数据

```c
bool post_json(const char *url, const char *json_data) {
    epd_http_header_t headers[] = {
        {"Content-Type", "application/json"}
    };
    
    http_ctx_t ctx = {0};
    
    epd_http_config_t config = {
        .url = url,
        .method = EPD_HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 1,
        .body = json_data,
        .body_size = strlen(json_data),
        .timeout_ms = 15000,
        .callback = http_callback,
        .user_data = &ctx,
    };
    
    epd_http_session_t *session = epd_http_request(&config);
    if (!session) {
        return false;
    }
    
    // 等待完成
    while (!ctx.completed) {
        rt_thread_mdelay(100);
    }
    
    epd_http_session_destroy(session);
    
    return ctx.status_code == 200 || ctx.status_code == 201;
}
```

### 下载文件

```c
typedef struct {
    FILE *fp;
    size_t total_size;
    size_t downloaded;
    int status_code;
    bool completed;
} download_ctx_t;

static void download_callback(epd_http_event_t event, void *data,
                             size_t size, void *user) {
    download_ctx_t *ctx = user;
    
    switch (event) {
        case EPD_HTTP_EVENT_HEADER:
            {
                epd_http_header_t *header = data;
                if (strcmp(header->name, "Content-Length") == 0) {
                    ctx->total_size = atoi(header->value);
                }
            }
            break;
            
        case EPD_HTTP_EVENT_DATA:
            if (ctx->fp && data && size > 0) {
                fwrite(data, 1, size, ctx->fp);
                ctx->downloaded += size;
                
                // 更新进度条
                if (ctx->total_size > 0) {
                    int progress = (ctx->downloaded * 100) / ctx->total_size;
                    update_ui_progress(progress);
                }
            }
            break;
            
        case EPD_HTTP_EVENT_COMPLETE:
            ctx->status_code = *(int*)data;
            ctx->completed = true;
            break;
            
        case EPD_HTTP_EVENT_ERROR:
            ctx->status_code = -1;
            ctx->completed = true;
            break;
    }
}

bool download_file(const char *url, const char *path) {
    // 打开文件
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    
    download_ctx_t ctx = {
        .fp = fp,
        .total_size = 0,
        .downloaded = 0,
        .completed = false
    };
    
    epd_http_config_t config = {
        .url = url,
        .method = EPD_HTTP_METHOD_GET,
        .timeout_ms = 60000,  // 60秒
        .callback = download_callback,
        .user_data = &ctx,
        .follow_redirects = true,
        .max_redirects = 5
    };
    
    epd_http_session_t *session = epd_http_request(&config);
    if (!session) {
        fclose(fp);
        unlink(path);
        return false;
    }
    
    // 等待下载完成
    while (!ctx.completed) {
        rt_thread_mdelay(100);
        
        // 检查取消
        if (is_cancelled()) {
            epd_http_session_destroy(session);
            fclose(fp);
            unlink(path);
            return false;
        }
    }
    
    epd_http_session_destroy(session);
    fclose(fp);
    
    if (ctx.status_code != 200) {
        unlink(path);
        return false;
    }
    
    return true;
}
```

### 带认证的请求

```c
bool fetch_with_auth(const char *url, const char *token) {
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    
    epd_http_header_t headers[] = {
        {"Authorization", auth_header}
    };
    
    http_ctx_t ctx = {0};
    
    epd_http_config_t config = {
        .url = url,
        .method = EPD_HTTP_METHOD_GET,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = 10000,
        .callback = http_callback,
        .user_data = &ctx,
    };
    
    epd_http_session_t *session = epd_http_request(&config);
    if (!session) {
        return false;
    }
    
    while (!ctx.completed) {
        rt_thread_mdelay(100);
    }
    
    epd_http_session_destroy(session);
    
    return ctx.status_code == 200;
}
```

## 错误处理

### 检查状态码

```c
switch (status_code) {
    case 200:
        // 成功
        break;
    case 201:
        // 创建成功
        break;
    case 400:
        show_error("请求参数错误");
        break;
    case 401:
        show_error("需要认证");
        break;
    case 403:
        show_error("无权访问");
        break;
    case 404:
        show_error("资源不存在");
        break;
    case 500:
        show_error("服务器错误");
        break;
    case -1:
        show_error("网络错误");
        break;
    default:
        show_error("请求失败");
        break;
}
```

### 超时和重试

```c
bool fetch_with_retry(const char *url, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        if (fetch_data(url)) {
            return true;
        }
        
        LOG_W("app", "Retry %d/%d", i + 1, max_retries);
        rt_thread_mdelay(1000 * (i + 1));  // 递增延迟
    }
    
    return false;
}
```

## 注意事项

### 线程安全

- 回调在 HTTP 客户端线程调用
- 不要在回调中直接操作 UI
- 使用消息队列或 UI 步进更新界面

### 内存管理

- 请求头和请求体数据必须在会话期间有效
- 响应数据需要自己管理（拷贝或保存）
- 及时销毁会话释放资源

### 网络变化

- 长时间请求要检查网络状态变化
- 网络切换可能导致连接中断
- 实现重试机制应对网络波动

## 相关文档

- **网络状态**：[网络 API](api-network.md)
- **使用指南**：[HTTPS 请求](../development/http-client.md)
- **最佳实践**：[开发最佳实践](../development/best-practices.md#网络请求最佳实践)
