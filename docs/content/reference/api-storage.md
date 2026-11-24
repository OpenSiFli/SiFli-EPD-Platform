---
title: "存储 API"
---

# 存储 API

平台提供统一的存储接口，管理内部存储和 TF 卡。

::: tip 头文件
```c
#include "services/storage.h"
#include "services/storage_file.h"
```
:::

## 核心概念

### 存储卷

设备有两个存储位置：

```c
typedef enum {
    STORAGE_VOLUME_INTERNAL = 0,  // 内部存储 (/flash)
    STORAGE_VOLUME_SD = 1,         // TF 卡 (/sdcard)
    STORAGE_VOLUME_COUNT
} storage_volume_t;
```

### 应用数据区域

每个应用有专用的数据区域：

```c
typedef enum {
    STORAGE_APP_AREA_DATA = 0,    // 配置和数据
    STORAGE_APP_AREA_CACHE = 1,   // 可重建的缓存
    STORAGE_APP_AREA_COUNT
} storage_app_area_t;
```

## 路径管理

### storage_app_path

获取应用专用路径。

```c
bool storage_app_path(char *out, size_t out_size,
                      const char *app_id,
                      storage_app_area_t area,
                      const char *filename);
```

**参数**：
- `out` - 输出缓冲区
- `out_size` - 缓冲区大小
- `app_id` - 应用 ID
- `area` - 数据区域
- `filename` - 文件名（相对路径）

**返回值**：
- `true` - 成功
- `false` - 缓冲区太小或参数无效

**示例**：
```c
char path[256];

// 获取配置文件路径
if (storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_DATA, "config.json")) {
    // path = "/flash/data/myapp/config.json"
    load_config(path);
}

// 获取缓存文件路径
if (storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_CACHE, "thumbnail.bin")) {
    // path = "/flash/cache/myapp/thumbnail.bin"
    save_cache(path);
}

// 子目录也可以
if (storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_DATA, "notes/note1.txt")) {
    // path = "/flash/data/myapp/notes/note1.txt"
    save_note(path);
}
```

::: tip 自动创建目录
如果目录不存在，系统会自动创建。
:::

### storage_volume_path

获取卷的挂载路径。

```c
const char* storage_volume_path(storage_volume_t volume);
```

**返回值**：
- `"/flash"` - 内部存储
- `"/sdcard"` - TF 卡
- `NULL` - 无效的卷

**示例**：
```c
const char *sd_path = storage_volume_path(STORAGE_VOLUME_SD);
if (sd_path) {
    char book_path[256];
    snprintf(book_path, sizeof(book_path), "%s/Books/mybook.epub", sd_path);
    open_book(book_path);
}
```

## 状态查询

### storage_available

检查存储是否可用。

```c
bool storage_available(storage_volume_t volume);
```

**返回值**：
- `true` - 可以读写
- `false` - 不可用（未挂载、被占用等）

**示例**：
```c
// 检查 TF 卡
if (!storage_available(STORAGE_VOLUME_SD)) {
    show_error("请插入 TF 卡");
    return;
}

// 检查内部存储
if (!storage_available(STORAGE_VOLUME_INTERNAL)) {
    show_error("内部存储错误");
    return;
}
```

### storage_info

获取存储信息。

```c
rt_err_t storage_info(storage_volume_t volume, storage_info_t *info);
```

**参数**：
- `volume` - 存储卷
- `info` - 输出信息结构

**storage_info_t 结构**：
```c
typedef struct {
    uint64_t total;      // 总容量（字节）
    uint64_t used;       // 已使用（字节）
    uint64_t available;  // 可用空间（字节）
} storage_info_t;
```

**返回值**：
- `RT_EOK` - 成功
- `-RT_ERROR` - 失败

**示例**：
```c
storage_info_t info;
if (storage_info(STORAGE_VOLUME_SD, &info) == RT_EOK) {
    LOG_I("app", "SD card: %llu MB total, %llu MB free",
          info.total / (1024 * 1024),
          info.available / (1024 * 1024));
    
    // 检查空间是否足够
    if (info.available < file_size) {
        show_error("空间不足");
        return -ENOSPC;
    }
}
```

### storage_revision

获取存储状态版本号。

```c
uint32_t storage_revision(void);
```

**返回值**：递增的版本号

每次存储状态变化（TF 卡插拔、USB 读卡器开关），版本号会增加。

**示例**：
```c
static uint32_t last_revision = 0;

void check_storage_change(void) {
    uint32_t current = storage_revision();
    if (current != last_revision) {
        // 存储状态改变，重新扫描文件
        reload_file_list();
        last_revision = current;
    }
}
```

## 文件操作

### storage_file_read

读取整个文件。

```c
rt_err_t storage_file_read(const char *path, void *buffer, size_t size);
```

**参数**：
- `path` - 文件路径
- `buffer` - 读取缓冲区
- `size` - 要读取的字节数

**返回值**：
- `RT_EOK` - 成功
- `-RT_ENOENT` - 文件不存在
- `-RT_ERROR` - 读取失败

**示例**：
```c
char path[256];
storage_app_path(path, sizeof(path), "myapp",
                 STORAGE_APP_AREA_DATA, "config.json");

// 先获取文件大小
struct stat st;
if (stat(path, &st) != 0) {
    // 文件不存在，使用默认配置
    return load_default_config();
}

// 分配缓冲区
char *buffer = epd_app_alloc(st.st_size + 1, EPD_APP_PSRAM);
if (!buffer) {
    return -ENOMEM;
}

// 读取文件
rt_err_t err = storage_file_read(path, buffer, st.st_size);
if (err == RT_EOK) {
    buffer[st.st_size] = '\0';
    parse_config(buffer);
}

epd_app_free(buffer);
return err;
```

### storage_file_write

写入整个文件。

```c
rt_err_t storage_file_write(const char *path, const void *data, size_t size);
```

**参数**：
- `path` - 文件路径
- `data` - 要写入的数据
- `size` - 数据大小

**返回值**：
- `RT_EOK` - 成功
- `-RT_EFULL` - 空间不足
- `-RT_ERROR` - 写入失败

**示例**：
```c
const char *config = "{\"theme\":\"dark\",\"font_size\":16}";

char path[256];
storage_app_path(path, sizeof(path), "myapp",
                 STORAGE_APP_AREA_DATA, "config.json");

rt_err_t err = storage_file_write(path, config, strlen(config));
if (err != RT_EOK) {
    LOG_E("app", "Failed to save config: %d", err);
    show_error("保存失败");
}
```

### storage_file_commit

原子替换文件（推荐）。

```c
rt_err_t storage_file_commit(const char *temp_path, const char *final_path);
```

**参数**：
- `temp_path` - 临时文件路径
- `final_path` - 最终文件路径

**返回值**：
- `RT_EOK` - 成功
- `-RT_ERROR` - 失败

**工作原理**：
1. 写入临时文件
2. 原子重命名到最终路径
3. 即使断电也不会损坏数据

**示例**：
```c
bool save_data_safe(const char *data, size_t size) {
    char path[256], temp_path[256];
    
    // 获取路径
    storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_DATA, "data.bin");
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    
    // 写入临时文件
    rt_err_t err = storage_file_write(temp_path, data, size);
    if (err != RT_EOK) {
        LOG_E("app", "Failed to write temp file");
        return false;
    }
    
    // 原子替换
    err = storage_file_commit(temp_path, path);
    if (err != RT_EOK) {
        LOG_E("app", "Failed to commit file");
        unlink(temp_path);  // 清理临时文件
        return false;
    }
    
    return true;
}
```

::: tip 最佳实践
重要数据必须使用 `storage_file_commit()`，可以防止断电时数据损坏。
:::

## 记录存储

### storage_record_save

保存带校验的记录。

```c
rt_err_t storage_record_save(const char *path,
                              uint32_t magic,
                              uint32_t version,
                              const void *data,
                              size_t size);
```

**参数**：
- `path` - 文件路径
- `magic` - 魔数（用于识别文件类型）
- `version` - 数据版本
- `data` - 数据内容
- `size` - 数据大小

**返回值**：
- `RT_EOK` - 成功
- 负数 - 失败

**示例**：
```c
#define CONFIG_MAGIC 0x4D594150  // "MYAP"
#define CONFIG_VERSION 1

typedef struct {
    int theme;
    int font_size;
    bool auto_save;
} app_config_t;

bool save_config(const app_config_t *config) {
    char path[256];
    storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_DATA, "config.bin");
    
    rt_err_t err = storage_record_save(path, CONFIG_MAGIC, CONFIG_VERSION,
                                       config, sizeof(app_config_t));
    return err == RT_EOK;
}
```

### storage_record_load

加载带校验的记录。

```c
rt_err_t storage_record_load(const char *path,
                              uint32_t magic,
                              uint32_t version,
                              void *data,
                              size_t size);
```

**参数**：与 `storage_record_save` 相同

**返回值**：
- `RT_EOK` - 成功
- `-RT_ENOENT` - 文件不存在
- `-RT_ERROR` - 魔数或版本不匹配

**示例**：
```c
bool load_config(app_config_t *config) {
    char path[256];
    storage_app_path(path, sizeof(path), "myapp",
                     STORAGE_APP_AREA_DATA, "config.bin");
    
    rt_err_t err = storage_record_load(path, CONFIG_MAGIC, CONFIG_VERSION,
                                       config, sizeof(app_config_t));
    
    if (err == RT_EOK) {
        return true;
    } else if (err == -RT_ENOENT) {
        // 文件不存在，使用默认值
        *config = (app_config_t){
            .theme = 0,
            .font_size = 16,
            .auto_save = true
        };
        return true;
    } else {
        LOG_E("app", "Failed to load config: %d", err);
        return false;
    }
}
```

::: tip 记录格式
`storage_record_*` 自动添加魔数、版本和校验和，可以检测文件损坏和版本不匹配。
:::

## 存储锁

长时间的文件操作需要使用存储锁。

### storage_lock / storage_unlock

```c
void storage_lock(void);
void storage_unlock(void);
```

**使用场景**：
- 批量文件操作
- 耗时的读写
- 需要原子性的操作序列

**示例**：
```c
bool batch_save_files(file_t *files, int count) {
    storage_lock();
    
    bool success = true;
    for (int i = 0; i < count; i++) {
        if (!save_one_file(&files[i])) {
            success = false;
            break;
        }
    }
    
    storage_unlock();
    return success;
}
```

::: warning 注意
- 持有锁期间其他存储操作会阻塞
- 不要长时间持有（建议 < 1秒）
- 纯内存操作不需要锁
:::

## 完整示例

### 应用配置管理

```c
#include "services/storage.h"

#define CONFIG_MAGIC 0x4D594150
#define CONFIG_VERSION 2

typedef struct {
    int theme;
    int font_size;
    bool notifications;
    char username[32];
} config_t;

// 保存配置
bool save_app_config(const config_t *config) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_DATA, "config.bin")) {
        return false;
    }
    
    return storage_record_save(path, CONFIG_MAGIC, CONFIG_VERSION,
                              config, sizeof(config_t)) == RT_EOK;
}

// 加载配置
bool load_app_config(config_t *config) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_DATA, "config.bin")) {
        return false;
    }
    
    rt_err_t err = storage_record_load(path, CONFIG_MAGIC, CONFIG_VERSION,
                                       config, sizeof(config_t));
    
    if (err == RT_EOK) {
        return true;
    }
    
    // 使用默认配置
    *config = (config_t){
        .theme = 0,
        .font_size = 16,
        .notifications = true,
        .username = ""
    };
    return true;
}
```

### 缓存管理

```c
// 保存缓存
bool save_cache(const char *name, const void *data, size_t size) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_CACHE, name)) {
        return false;
    }
    
    return storage_file_write(path, data, size) == RT_EOK;
}

// 加载缓存
void* load_cache(const char *name, size_t *out_size) {
    char path[256];
    if (!storage_app_path(path, sizeof(path), "myapp",
                         STORAGE_APP_AREA_CACHE, name)) {
        return NULL;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }
    
    void *buffer = epd_app_alloc(st.st_size, EPD_APP_PSRAM);
    if (!buffer) {
        return NULL;
    }
    
    if (storage_file_read(path, buffer, st.st_size) != RT_EOK) {
        epd_app_free(buffer);
        return NULL;
    }
    
    *out_size = st.st_size;
    return buffer;
}

// 清理缓存
void clear_app_cache(void) {
    char cache_dir[256];
    if (!storage_app_path(cache_dir, sizeof(cache_dir), "myapp",
                         STORAGE_APP_AREA_CACHE, "")) {
        return;
    }
    
    // 递归删除缓存目录内容
    // 实现略
}
```

## 相关文档

- **应用存储指南**：[应用存储](../development/application-storage.md)
- **文件处理**：[文件内容交接](../development/content-handlers.md)
- **最佳实践**：[开发最佳实践](../development/best-practices.md#存储操作最佳实践)
