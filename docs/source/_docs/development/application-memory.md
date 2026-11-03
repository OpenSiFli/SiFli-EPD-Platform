---
title: "应用内存"
---

# 应用内存

平台使用片上 SRAM 和外部 PSRAM。SRAM 容纳内核对象及有硬件约束的缓冲；图片、文本、网络响应和应用的大块工作区优先放入 PSRAM。各池的空闲量取决于已加载应用和正在执行的任务，分区大小见[平台架构](../architecture/architecture.md)。

## 选择和释放内存

公共接口位于 源码：`src/platform/epd_memory.h`，应用包含 `platform/epd_memory.h`：

```c
#include <stdbool.h>
#include "platform/epd_memory.h"

static bool process_buffer(size_t capacity)
{
    unsigned char *buffer = epd_app_alloc(capacity, EPD_APP_PSRAM);
    if (!buffer) return false;
    /* 在本次任务中使用 buffer。 */
    epd_app_free(buffer);
    return true;
}
```

`epd_app_alloc()` 返回调用者拥有的内存，失败返回 `NULL`。显式请求 PSRAM 不会回退到 SRAM。扩容使用 `epd_app_realloc()`，内存区域参数必须与原分配一致；先将返回值存入临时指针，成功后再更新原指针。释放使用 `epd_app_free()`，它接受 `NULL`。

| 对象来源 | 释放方式 |
| --- | --- |
| `epd_app_alloc/realloc` | `epd_app_free` |
| `lv_malloc` | `lv_free` |
| LVGL 控件 | 在 UI 线程删除控件；子对象随父对象删除 |
| `epd_app_font_create` | 去除控件和绘制引用后调用 `epd_app_font_destroy`，后备字体另行释放 |
| HTTP、AI、语音的结果对象 | 使用对应 `clear` 或 `event_release` 接口 |
| SDK 带标签的缓存分配 | 使用该 SDK 分配器配套的释放接口 |

跨异步任务传递数据时明确谁保留、谁释放。页面内存、局部数组和事件借用指针不能在其有效期结束后继续交给 worker 使用；可复制数据或通过受管服务保留状态。

## 线程与硬件缓冲

`epd_thread_create_psram()` 将线程栈放入保留型 PSRAM，控制块仍使用 SRAM。正常退出或删除后，内核空闲线程回收两部分内存。动态应用优先通过[受管后台服务](application-lifecycle.md)创建任务，使任务退出与模块卸载关联。

DMA、跨核共享内存和显示缓冲还需要满足对齐、缓存一致性与设备可访问范围。计算任务使用 `epd_compute_buffer_alloc()`，图像或音频使用对应组件的缓冲契约；单纯改用 PSRAM 分配不能替代这些条件。

## 分配失败与用量

`epd_app_psram_available()` 返回应用可用 PSRAM 的瞬时统计；UI 线程可通过 `epd_app_ui_available()` 查看 LVGL 私有堆的可用量。统计值不是内存预留，也不保证同样大小的连续块可以分配。

每次分配失败均应结束本次操作或降低可重建缓存用量，并释放本次已经取得的资源。复现持续增长时，对比进入页面前、退出页面后和 worker 完成后的 `list_memheap`，结合[日常调试](../troubleshooting/development-debugging.md)检查所有权。

## 后台资源回收

受管服务只有显式声明可回收，并且不处于前台、没有在途调用时，才会被内存压力流程选中。服务通过停止协议主动释放资源，平台等待完成后再卸载模块。当前天气服务声明了该能力；这不是所有应用默认具备的自动换出功能。阅读器自身先回收远页与可重建缓存，仍不足时请求平台回收低优先级后台资源，具体流程见[内存与存储](../architecture/memory-storage.md)。
