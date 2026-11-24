---
title: "应用清单"
---

# 应用清单

每个应用的 `app.json` 文件声明应用的身份、版本和功能。

## 完整示例

```json
{
  "abi": 2,
  "id": "myapp",
  "version": 3,
  "data_version": 1,
  "name": "我的应用",
  "default_locale": "zh_cn",
  "localized_names": {
    "zh_cn": "我的应用",
    "en_us": "My App"
  },
  "background": false,
  "settings": false,
  "lockscreen": false,
  "file_extensions": ["txt", "md"]
}
```

## 核心字段

### abi（必需）

**类型**：整数  
**当前值**：`2`

ABI（应用程序二进制接口）版本。

::: danger 重要
当前平台 ABI 版本是 **2**（不是 1）。使用错误的值会导致安装失败。
:::

### id（必需）

**类型**：字符串  
**格式**：1-7 个字符，只能用小写字母、数字、下划线

应用唯一标识。

✅ **有效示例**：`myapp`、`weather`、`ai_demo`  
❌ **无效示例**：`MyApp`（大写）、`my-app`（连字符）、`verylongid`（超长）

**保留名称**：`all`、`none`

**重要规则**：
- 目录名必须匹配：`modules/<id>/`
- 模块文件必须匹配：`<id>.so`
- 不能与内置应用冲突

### version（必需）

**类型**：正整数（1 - 4294967295）

应用版本号。更新时必须大于已安装版本。

**示例**：
```json
// 首次发布
{"version": 1}

// 第一次更新（必须 > 1）
{"version": 2}

// 第二次更新（必须 > 2）
{"version": 3}
```

### data_version（必需）

**类型**：正整数（1 - 4294967295）

数据格式版本。用于标识应用保存数据的格式。

**更新规则**：
- 数据格式不变：保持相同的 `data_version`
- 数据格式改变：增加 `data_version`
- 已安装应用的 `data_version` 非零时，更新包的 `data_version` 必须匹配

**示例场景**：
```json
// v1.0 - 配置格式：JSON
{"version": 1, "data_version": 1}

// v2.0 - 配置格式不变，只修 bug
{"version": 2, "data_version": 1}

// v3.0 - 配置格式改为 binary
{"version": 3, "data_version": 2}
```

### name（必需）

**类型**：字符串（1-47 字节 UTF-8）

默认显示名称。

**示例**：
```json
{"name": "我的应用"}      // 12 字节
{"name": "My Application"} // 14 字节
```

### localized_names（可选但推荐）

**类型**：对象

各语言的显示名称。

**示例**：
```json
{
  "default_locale": "zh_cn",
  "localized_names": {
    "zh_cn": "我的应用",
    "en_us": "My App"
  }
}
```

::: tip 多语言支持
详见[多语言开发](../development/multilingual.md)。
:::

## 功能字段

### background

**类型**：布尔值  
**默认**：`false`

是否提供后台服务。

设为 `true` 时，应用必须导出 `epd_app_background`。

**示例**：
```c
// 在应用代码中导出
const epd_background_t epd_app_background = {
    .run = my_service_run,
    .stack_size = 8192,
    .priority = 10,
};
```

详见[应用生命周期](../development/application-lifecycle.md)。

### settings

**类型**：布尔值  
**默认**：`false`

是否提供设置入口。

设为 `true` 时，系统设置会显示此应用，点击时会调用 `app_main()` 并传入 `page=settings`。

### lockscreen

**类型**：布尔值  
**默认**：`false`

是否可作为锁屏应用。

设为 `true` 时，用户可在设置中选择此应用作为锁屏。调用时传入 `lockscreen=1`。

### file_extensions

**类型**：字符串数组  
**默认**：`[]`

声明可以打开的文件类型。

**格式**：
- 小写字母或数字
- 不带点
- 不重复

**示例**：
```json
{
  "file_extensions": ["txt", "md", "epub"]
}
```

用户打开这些文件时，会显示"用 XXX 打开"选项。

详见[文件内容交接](../development/content-handlers.md)。

## 自动生成字段

以下字段由构建工具自动生成，不要手动编辑：

| 字段 | 说明 |
|------|------|
| `package_format` | 安装包格式版本（当前为 1） |
| `build_id` | 构建配置标识 |
| `files_sha256` | 文件完整性校验 |
| `load_bytes` | 模块加载大小 |
| `icon` | 图标文件名（`icon.ezip`） |
| `libraries` | 私有库列表 |

## 私有库

应用可以包含私有库（`codecs/<name>.so`）。

**命名规则**：
- 1-7 个字符
- 小写字母、数字、下划线
- 不能与主应用同名

私有库不需要 `app_main` 入口，只提供函数供主应用调用。

## 验证和安装

**构建时验证**：
- `tools/package_app.py` - 检查清单字段和资源
- `tools/check_app_imports.py` - 验证导入符号

**安装时验证**：
- 清单格式
- ABI 版本兼容性
- 版本号规则
- 文件完整性
- 存储空间

## 相关文档

- **包格式**：[SAP 格式](sap-format.md)
- **生命周期**：[应用生命周期](../development/application-lifecycle.md)
- **多语言**：[多语言开发](../development/multilingual.md)
- **文件处理**：[文件内容交接](../development/content-handlers.md)

## 构建生成字段

以下值由打包器计算，不在源码清单中手工维护：

| 字段 | 生成值 |
| --- | --- |
| `package_format` | 当前为 1 |
| `build_id` | 目标固件 `app-profile.json` 中的构建配置标识 |
| `files_sha256` | 包内 `files.sha256` 文件全部字节的 SHA-256，小写十六进制 |
| `load_bytes` | 主模块各可加载段从起始到末尾的地址跨度，包含段间间隙和 BSS |
| `icon` | 存在入口图标时为 `icon.ezip` |
| `libraries` | 可选映射：私有库名称到该 ELF 的装载跨度 |

`build_id` 用于记录来源配置，不要求与运行固件的字符串相等。加载仍要求 ABI、公共结构布局和导入符号兼容。`load_bytes` 是装载空间，既不是 `.so` 文件长度，也不包含应用之后申请的全部堆内存。

主模块为 `<id>.so`，须导出 `app_main` 和 `epd_app_info`。私有库位于 `codecs/<name>.so`，名称同样为 1～7 个小写字母、数字或下划线，不能与主应用同名。附属库按各自 ABI 提供函数，不要求应用入口。

## 校验与更新

源码：`tools/package_app.py` 检查源码字段、ARM 32 位小端共享 ELF、可加载段、资源路径和摘要。源码：`tools/check_app_imports.py` 将导入函数与目标固件导出表核对。设备安装时再次校验清单、文件摘要、模块和目标卷状态，再提交更新。

安装文件的字节结构见 [SAP 格式](sap-format.md)，运行和卸载的资源关系见[应用生命周期](../development/application-lifecycle.md)。
