---
title: "应用存储"
---

# 应用存储

应用的程序、业务数据和缓存位于安装卷上。使用 `storage_app_path()` 取得路径，应用安装到内部存储或 TF 卡时可以复用同一份代码。

| 区域 | 用途 | 内部存储路径 | TF 卡路径 |
| --- | --- | --- | --- |
| `STORAGE_APP_CODE` | 程序和随包资源 | `/flash/apps/<id>/` | `/sdcard/.epd/apps/<id>/` |
| `STORAGE_APP_DATA` | 配置、进度、历史 | `/flash/data/<id>/` | `/sdcard/.epd/data/<id>/` |
| `STORAGE_APP_CACHE` | 可重新生成的缓存 | `/flash/cache/<id>/` | `/sdcard/.epd/cache/<id>/` |

路径查询将结果写入调用者的缓冲区，空间不足或应用不可用时返回 `false`，不会创建目录。原始文件写入需要时调用 `storage_mkdirs()`；记录保存和文件替换辅助函数负责其写入流程。接口见 源码：`src/services/storage_file.h`。

## 保存和读取配置

cfgdemo（源码：`modules/cfgdemo/app_main.c`） 保存按钮点击次数。它先载入默认值，再尝试读取记录；只有文件长度、标识、版本和 CRC 都匹配时才采用保存的值。

```c
#include "storage_file.h"

typedef struct { uint32_t presses; } saved_config_t;

static bool load_config(saved_config_t *config)
{
    char path[STORAGE_PATH_MAX];
    saved_config_t loaded;
    config->presses = 0;
    if (!storage_app_path(path, sizeof(path), "cfgdemo",
                          STORAGE_APP_DATA, "settings.bin") ||
        !storage_record_load(path, 0x43464731u, 1u, &loaded, sizeof(loaded)))
        return false;
    *config = loaded;
    return true;
}

static bool save_config(const saved_config_t *config)
{
    char path[STORAGE_PATH_MAX];
    return !storage_changing() &&
        storage_app_path(path, sizeof(path), "cfgdemo",
                         STORAGE_APP_DATA, "settings.bin") &&
        storage_record_save(path, 0x43464731u, 1u, config, sizeof(*config));
}
```

这里直接调用的 `storage_record_save()` 是同步操作，应在应用 worker 中执行，函数返回后不再借用 `config`。平台系统设置使用独立的串行后台保存任务，两者的调用契约不同。先形成新配置，保存成功后再替换当前状态；保存失败时保留原值并显示失败结果。读取失败可能来自文件不存在、数据格式变化、内容损坏或卷不可用，调用者按业务选择默认值或错误页面。结构包含指针或未定义填充时，应先编码为确定的字节格式，再保存。

`storage_record_save()` 使用临时文件、同步和改名提交，内部管理存储锁。`storage_file_replace()` 保存无记录头的整份数据；`storage_file_commit()` 发布已经写好的临时文件。FAT 替换经过两次改名，使用原始文件接口读取替换目标前调用 `storage_file_recover()` 完成中断恢复。

## 文件访问与 TF 卡会话

原始 `open/read/write/close` 操作与介质交接共用 `storage_lock()/storage_unlock()`。在锁内检查 `storage_path_available()`，执行一段有界 I/O 后释放锁。网络等待、UI 等待和大规模解析放在锁外；长任务运行在受管 worker 中。

读取 TF 卡前保存 `storage_card_session()`，后续每次访问检查 `storage_session_valid(STORAGE_SD, session)`。拔卡或转交 USB 后旧会话失效，即使路径相同，新插入的卡也属于另一会话。此时取消任务、关闭文件并报告资源已离线。

应用的后台 `flush()` 和停止回调需等待已接受的数据保存或取消收尾。平台释放模块和卸载存储的顺序见[应用生命周期](application-lifecycle.md)。

## 卸载与缓存

卸载删除程序和缓存，业务数据保留。应用重新安装后仍应校验自己的数据格式。安装位置改变时，由安装器处理符合条件的数据迁移，规则见[安装包与存储](application-development.md#安装包与存储)。

缓存键应包含源内容和影响输出的配置。LittleFS 在当前 SDK 中没有可用的修改时间，阅读器可用 `storage_file_signature()` 取得内容标记；该接口复用与规范路径、文件版本、大小、存储变化序号和卡会话绑定的签名；命中时不重新全读。需要重新计算时仍会读取文件，应放在工作线程。写入或外部介质交接使受影响记录失效。缓存损坏或缺失时重新生成，不以缓存代替用户原始文件。
