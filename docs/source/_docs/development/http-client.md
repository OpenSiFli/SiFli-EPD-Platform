---
title: "HTTPS 请求"
---

# HTTPS 请求

`modules/common/http` 提供同步 HTTPS 客户端。应用将该源码组件编入自身模块，在受管 worker 内发起请求，再把结果副本交给 UI。它使用平台当前上行网络，处理 TLS 校验、传输分帧、gzip 和取消。

## 加入构建

在应用 `SConscript` 中、`Return('objects')` 之前加入：

```python
objects += SConscript('../common/http/SConscript')
```

参考 HTTP 组件构建脚本（源码：`modules/common/http/SConscript`）与天气应用（源码：`modules/weather/SConscript`）。包含头文件 `epd_http.h`。固件需具备网络和 mbedTLS 支持，应用导入检查会验证目标固件提供的符号。

## 请求与结果

下面的函数在工作线程中读取一个 HTTPS 文本资源。`host` 使用域名或 IPv4 地址，`path` 以 `/` 开头；调用者根据业务选择地址，并在返回前保持所有参数有效。

```c
#include "epd_http.h"

static bool fetch_text(const char *host, const char *path,
                       epd_http_cancelled_fn cancelled, void *context)
{
    epd_http_request_t request = {
        .host = host,
        .path = path,
        .method = "GET",
        .timeout_ms = 30000,
        .body_limit = 64 * 1024,
        .cancelled = cancelled,
        .user = context
    };
    epd_http_response_t response = {0};
    bool transported = epd_http_perform(&request, &response);
    bool ok = transported && response.status >= 200 && response.status < 300;
    if (ok) {
        /* 在释放前解析 response.body，或把需要的字段复制到业务状态。 */
    }
    epd_http_response_clear(&response);
    return ok;
}
```

`epd_http_perform()` 返回 `true` 表示收到完整响应。404、500 等 HTTP 状态也可能对应成功传输，因此业务还要检查 `response.status`。响应正文在 PSRAM 中分配，末尾附加零字节，`size` 不含结尾零；二进制数据使用长度读取。

使用 POST 时设置 `method`、`body`、`body_size` 和所需的 `Content-Type`。请求数据由调用者持有。客户端生成 Host、Connection、Content-Length 和 Transfer-Encoding，调用者不能重复设置这些头。接口和完整字段契约见 源码：`modules/common/http/epd_http.h`。

## 取消与错误

`cancelled` 在发起请求的线程中执行，应读取受同步保护的取消状态并迅速返回。服务停止、页面业务取消或存储会话失效时请求取消，等待函数返回后再释放上下文。网络 generation 改变会结束在途请求。SDK DNS 查询是同步的，取消和超时在查询前后检查，无法中途打断解析器。

失败时 `stage`、`detail` 和 `verify_flags` 分别提供发生阶段、底层错误和证书校验信息；正文已清理。记录诊断信息后仍调用 `epd_http_response_clear()`。平台验证证书和主机名，设备日期时间也应正确。

客户端当前使用 HTTPS 443，支持 GET 和 POST，不自动跟随重定向或重试。需要分块消费响应的应用可使用 源码：`modules/common/http/epd_http_stream.h`。网络连接与服务状态见[网络服务](../services/wifi.md)，worker 的停止及 UI 交接见[应用生命周期](application-lifecycle.md)。
