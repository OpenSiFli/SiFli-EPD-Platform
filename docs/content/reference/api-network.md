---
title: "网络 API"
---

# 网络 API

平台提供统一的网络接口，自动管理 Wi-Fi 和蓝牙网络共享。

::: tip 头文件
```c
#include "services/net/network.h"
```
:::

## 核心概念

### 网络自动管理

平台自动选择最佳网络：
1. **优先 Wi-Fi** - 速度快、稳定
2. **回退到蓝牙** - Wi-Fi 不可用时

你不需要自己连接 Wi-Fi 或管理蓝牙，只需检查网络是否就绪。

### 网络状态

```c
typedef struct {
    bool ready;              // 网络是否可用
    int source;             // 网络来源（WLAN/PAN）
    uint32_t generation;    // 状态版本号
} network_snapshot_t;
```

## 检查网络

### network_ready

快速检查网络是否可用。

```c
bool network_ready(void);
```

**返回值**：
- `true` - 可以发起网络请求
- `false` - 网络不可用

**示例**：
```c
if (!network_ready()) {
    show_error("网络不可用，请连接 Wi-Fi");
    return -ENETUNREACH;
}

// 发起请求
http_get(url, callback, context);
```

### network_snapshot

获取详细的网络状态。

```c
void network_snapshot(network_snapshot_t *snapshot);
```

**参数**：
- `snapshot` - 输出网络状态

**示例**：
```c
network_snapshot_t state;
network_snapshot(&state);

if (state.ready) {
    LOG_I("app", "Network ready, source=%d, gen=%u",
          state.source, state.generation);
    
    // 可以发起请求
    fetch_data();
} else {
    LOG_W("app", "Network not ready");
    show_error("请连接网络");
}
```

**网络来源**：
- `0` - Wi-Fi (WLAN)
- `1` - 蓝牙网络共享 (PAN)

## 监听网络变化

后台任务可以订阅网络状态变化。

### network_subscribe

订阅网络事件。

```c
uint32_t network_subscribe(struct rt_event *event, uint32_t event_bit);
```

**参数**：
- `event` - 事件对象
- `event_bit` - 事件位（通常用 1）

**返回值**：
- 订阅 token（非零）
- `0` - 订阅失败

**说明**：
- 订阅成功后立即收到一次初始状态通知
- 每次网络状态变化时会触发事件

### network_unsubscribe

取消订阅。

```c
void network_unsubscribe(uint32_t token);
```

**参数**：
- `token` - 订阅时返回的 token

**重要**：
- `network_unsubscribe()` 返回后才能删除事件对象
- 这样保证不会再收到事件通知

### 完整订阅示例

```c
#include "services/net/network.h"

typedef struct {
    struct rt_event network_event;
    uint32_t subscribe_token;
    bool running;
} network_ctx_t;

static network_ctx_t g_ctx = {0};

// 初始化网络监听
bool init_network_monitor(void) {
    // 创建事件对象
    rt_event_init(&g_ctx.network_event, "myapp_net", RT_IPC_FLAG_FIFO);
    
    // 订阅网络变化
    g_ctx.subscribe_token = network_subscribe(&g_ctx.network_event, 1);
    if (g_ctx.subscribe_token == 0) {
        LOG_E("app", "Failed to subscribe network");
        rt_event_detach(&g_ctx.network_event);
        return false;
    }
    
    g_ctx.running = true;
    return true;
}

// 工作线程
static void network_monitor_thread(void *param) {
    while (g_ctx.running) {
        // 等待网络事件
        rt_uint32_t events;
        rt_err_t err = rt_event_recv(&g_ctx.network_event, 1,
                                     RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                                     RT_WAITING_FOREVER, &events);
        
        if (err != RT_EOK || !g_ctx.running) {
            break;
        }
        
        // 获取当前状态
        network_snapshot_t state;
        network_snapshot(&state);
        
        LOG_I("app", "Network state changed: ready=%d, source=%d",
              state.ready, state.source);
        
        if (state.ready) {
            // 网络恢复，重试失败的请求
            retry_pending_requests();
        } else {
            // 网络断开，取消进行中的请求
            cancel_active_requests();
        }
    }
}

// 清理
void cleanup_network_monitor(void) {
    g_ctx.running = false;
    
    // 触发事件让线程退出
    rt_event_send(&g_ctx.network_event, 1);
    
    // 等待线程结束
    // ...
    
    // 取消订阅
    network_unsubscribe(g_ctx.subscribe_token);
    
    // 删除事件对象（必须在 unsubscribe 返回后）
    rt_event_detach(&g_ctx.network_event);
}
```

## 检测网络变化

用 `generation` 检测网络状态是否改变。

```c
static uint32_t last_generation = 0;

bool check_network_changed(void) {
    network_snapshot_t state;
    network_snapshot(&state);
    
    if (state.generation != last_generation) {
        last_generation = state.generation;
        return true;  // 网络状态改变了
    }
    
    return false;  // 未改变
}
```

**使用场景**：
- 长时间运行的请求（下载、上传）
- 需要验证网络稳定性的操作
- 请求重试逻辑

**示例**：
```c
bool download_file(const char *url) {
    // 记录开始时的网络状态
    network_snapshot_t start_state;
    network_snapshot(&start_state);
    
    if (!start_state.ready) {
        return false;
    }
    
    // 执行下载
    while (downloading) {
        // 检查网络是否变化
        network_snapshot_t current;
        network_snapshot(&current);
        
        if (current.generation != start_state.generation) {
            LOG_W("app", "Network changed during download");
            
            if (!current.ready) {
                // 网络断开，取消下载
                cancel_download();
                return false;
            }
            
            // 网络切换了（如从 Wi-Fi 切到蓝牙），继续
            start_state = current;
        }
        
        // 继续下载
        download_chunk();
    }
    
    return true;
}
```

## HTTP 客户端

平台提供 HTTP/HTTPS 客户端，详见 [HTTP 客户端 API](api-http.md)。

## Wi-Fi 配置

Wi-Fi 的连接和配置由系统设置管理，详见 [网络服务](../services/wifi.md)。

## 完整示例

### 天气应用网络管理

```c
#include "services/net/network.h"

typedef struct {
    struct rt_event network_event;
    uint32_t subscribe_token;
    rt_thread_t worker_thread;
    bool running;
    int retry_count;
} weather_service_t;

static weather_service_t g_service = {0};

// 获取天气数据
static bool fetch_weather_data(void) {
    // 检查网络
    if (!network_ready()) {
        LOG_W("weather", "Network not ready");
        return false;
    }
    
    // 记录网络状态
    network_snapshot_t start_state;
    network_snapshot(&start_state);
    
    // 发起 HTTP 请求
    http_response_t response;
    int err = http_get("https://api.weather.com/data", &response);
    
    if (err != 0) {
        LOG_E("weather", "HTTP request failed: %d", err);
        return false;
    }
    
    // 检查网络是否在请求期间改变
    network_snapshot_t end_state;
    network_snapshot(&end_state);
    
    if (end_state.generation != start_state.generation) {
        LOG_W("weather", "Network changed during request");
        // 请求已完成，数据仍然有效
    }
    
    // 处理响应
    parse_weather_data(response.data, response.size);
    return true;
}

// 工作线程
static void weather_worker(void *param) {
    while (g_service.running) {
        // 等待事件（1小时超时自动更新）
        rt_uint32_t events;
        rt_err_t err = rt_event_recv(&g_service.network_event, 1,
                                     RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                                     3600 * 1000, &events);
        
        if (!g_service.running) {
            break;
        }
        
        // 超时或网络事件触发
        if (network_ready()) {
            if (fetch_weather_data()) {
                g_service.retry_count = 0;
            } else {
                // 失败，稍后重试
                g_service.retry_count++;
                if (g_service.retry_count < 3) {
                    rt_thread_mdelay(60000);  // 1分钟后重试
                }
            }
        }
    }
}

// 启动服务
bool start_weather_service(void) {
    // 初始化事件
    rt_event_init(&g_service.network_event, "weather_net", RT_IPC_FLAG_FIFO);
    
    // 订阅网络
    g_service.subscribe_token = network_subscribe(&g_service.network_event, 1);
    if (g_service.subscribe_token == 0) {
        rt_event_detach(&g_service.network_event);
        return false;
    }
    
    // 创建工作线程
    g_service.running = true;
    g_service.worker_thread = rt_thread_create("weather",
                                               weather_worker, NULL,
                                               8192, 10, 10);
    if (!g_service.worker_thread) {
        network_unsubscribe(g_service.subscribe_token);
        rt_event_detach(&g_service.network_event);
        return false;
    }
    
    rt_thread_startup(g_service.worker_thread);
    return true;
}

// 停止服务
void stop_weather_service(void) {
    g_service.running = false;
    
    // 触发事件让线程退出
    rt_event_send(&g_service.network_event, 1);
    
    // 等待线程结束
    // ...
    
    // 清理资源
    network_unsubscribe(g_service.subscribe_token);
    rt_event_detach(&g_service.network_event);
}
```

## 相关文档

- **HTTP 请求**：[HTTP 客户端](../development/http-client.md)
- **Wi-Fi 设置**：[网络服务](../services/wifi.md)
- **后台任务**：[应用生命周期](../development/application-lifecycle.md)
- **最佳实践**：[开发最佳实践](../development/best-practices.md#网络请求最佳实践)
