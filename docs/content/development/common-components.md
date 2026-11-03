---
title: "公共组件"
---

# 公共组件

公共能力有两种交付方式：固件导出函数供动态应用调用；`modules/common/` 中的源码组件由应用编入自己的 `.so`。前者随平台更新，后者随应用重新构建。选择组件时同时检查构建入口和资源生命周期。

| 能力 | 接入方式 | 源码入口 |
| --- | --- | --- |
| HTTPS | 应用编入公共源码 | [HTTP 指南](http-client.md) |
| Markdown 解析 | 应用编入公共源码 | 源码：`modules/common/markdown/markdown.h` |
| TXT/XHTML 阅读组件 | 应用编入公共源码及其依赖 | 源码：`modules/common/reader/reader.h` |
| 跨核计算 | 调用固件导出接口及计算组件契约 | 源码：`src/platform/epd_compute.h` |
| PCM 播放 | 调用固件导出接口 | 源码：`src/platform/epd_pcm_output.h` |

## Markdown

在应用的 `SConscript` 中将 `../common/markdown/SConscript` 返回的对象加入 `objects`。`md_document_parse()` 将 UTF-8 转换为块节点、行内文本和样式，不直接创建 LVGL 控件。AI 对话的显示层位于 源码：`modules/ai_demo/src/`。

```c
#include "markdown.h"
#include "platform/epd_memory.h"

static void *md_alloc(void *context, size_t bytes)
{
    (void)context;
    return epd_app_alloc(bytes, EPD_APP_PSRAM);
}
static void md_free(void *context, void *memory)
{
    (void)context;
    epd_app_free(memory);
}
static bool parse_text(const char *text, size_t length)
{
    const md_allocator_t allocator = {NULL, md_alloc, md_free};
    md_doc_t document = {0};
    md_status_t status = md_document_parse(text, length, &allocator, NULL, &document);
    if (status == MD_OK) {
        /* 从 document.nodes、runs 和 text 构造业务显示结果。 */
    }
    md_document_free(&document);
    return status == MD_OK;
}
```

输入上限为 64 KiB，默认总内存上限为 2 MiB；`md_limits_t` 可以进一步降低预算。字符串通过偏移和长度引用 `document.text`，并非逐项零结尾。流式输入末尾不完整的 UTF-8 字符由 `input_consumed` 标识，下一次拼接时保留未消费字节。文档及分配器上下文保持有效到 `md_document_free()`。

## 应用阅读组件

common/reader/SConscript（源码：`modules/common/reader/SConscript`） 收集阅读视图、字体、来源、转换及图片组件，供应用一起编译。它使用书架的文档转换代码；接入参考书架构建脚本（源码：`modules/books/SConscript`），完整数据流见[系统架构总览](../architecture/architecture.md#阅读数据流)。

1. UI 准备 `reader_options_t`：应用 ID、缓存目录、稳定的内容身份、TXT/XHTML 文件路径、标题和保存回调。
2. `epd_reader_open()` 复制字符串并排队；工作线程循环调用 `epd_reader_worker_step(service)` 完成读取和转换。
3. 独立视图 worker 推进排版、字形、图片和页面合成；UI 用 `epd_reader_step()` 消费已发布结果，显示空闲时用 `epd_reader_bind()` 绑定容器。相邻翻页调用 `epd_reader_turn()`；全书页数确定后可用 `epd_reader_seek()` 跳到指定页。持久位置保存源锚点，临时游标只在当前打开的片段内有效。
4. 停止接收新任务并取消在途转换，等待当前 step 返回，在同一个 worker 中执行 `epd_reader_worker_shutdown()` 后退出线程；确认 worker 已退出后，UI 执行 close 和 collect，回收旧会话。

传入非空 options 后，组件接管 `user`，最终调用一次 `release_user`，失败路径也包括在内。`save` 在 UI 线程执行，异步保存需要复制位置并排队。一次组件实例只保留一个可见阅读器，回调上下文要活到最后一次工作线程访问结束。

## 阅读设置与挂起

应用公共阅读组件通过平台阅读字体服务取得当前字体。设置变化后由 `epd_reader_settings_changed()` 检测并更新，`epd_reader_reflow()` 请求重排；源锚点用于在新排版中恢复阅读位置。

应用挂起时调用 `epd_reader_suspend(true)` 暂停准备并保留已绑定页面和最新操作意图，恢复时解除挂起。`epd_reader_background_pause()` 只暂停可选预备，仍允许首屏、跳转及明确的保存工作。生命周期与接口契约见 源码：`modules/common/reader/reader.h`。

## 跨核计算

计算组件有名称、版本及专用参数协议。应用在 worker 中通过 `epd_compute_query(name, version, &capability)` 查询，检查返回值、`ready` 及注册标志。`ready` 表示调度器和板级策略允许执行，设备验证状态单独由标志描述。

使用 `epd_compute_buffer_alloc()` 分配对齐的共享缓冲，调用 `epd_compute_buffer_region()` 描述容量、有效长度及读写权限。组件的输入、输出和参数引用使用其专用头文件定义，实例见阅读计算客户端（源码：`modules/common/reader/compute/reader_client.c`）。

按 `open → step/reset → close` 管理会话。同步等待期间取消回调在提交线程执行；其他线程请求取消时仍需保持会话存活，等在途 step 返回后由所有者关闭，再释放共享缓冲。能力不可用或执行失败时，由调用者处理返回状态；当前阅读链路的文档、字体和页面合成没有完整的 HCPU 自动回退实现。

组件注册、板级准入和双核产物关系见[平台扩展](../contributing/platform-development.md#hcpu-与-acpu)。

## PCM 输出

`epd_pcm_output_open(type, rate, callback, data)` 接受指定采样率的有符号 16 位单声道 PCM，失败返回空指针。应用在供数线程串行调用 open、write 和 close。

`epd_pcm_output_write()` 返回已接受的输入字节数，可能只是前缀；返回 0 表示缓存满，应保留同一份输入稍后重试；-1 表示输出被挂起，-2 表示状态或输入无效，负数不接受新输入。`epd_pcm_output_cached_ms()` 给出已进入 SDK 缓存的估计时长，不包含等待提交的输入。

`epd_pcm_output_close()` 立即关闭。应用负责等待队列音频处理完毕；关闭成功后句柄失效，关闭失败时句柄仍需保留以便收尾。应用导出接口为上述四个函数，音乐播放器的使用见 源码：`modules/music/`。播放与系统语音互斥还需通过[媒体会话](touch-ui.md#媒体会话)发布和处理控制命令。
