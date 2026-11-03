---
title: "应用清单"
---

# 应用清单

应用源码中的 `modules/<id>/app.json` 声明应用身份和能力。构建工具读取它、模块 ELF 和固件配置，生成 `.sap` 内的最终 `app.json`。普通应用通过[统一构建命令](../build/building.md#构建应用包)生成安装包。

## 源码字段

```json
{
  "abi": 1,
  "id": "myapp",
  "version": 1,
  "data_version": 1,
  "name": "我的应用",
  "default_locale": "zh_cn",
  "localized_names": {"zh_cn": "我的应用", "en_us": "My App"},
  "background": false,
  "settings": false,
  "lockscreen": false
}
```

| 字段 | 类型与要求 | 含义 |
| --- | --- | --- |
| `abi` | 必填，当前为整数 1 | 与模块 `EPD_APP_ABI` 对应的二进制接口版本 |
| `id` | 必填，`[a-z0-9_]{1,7}`；构建选择保留 `all`、`none` | 模块、安装目录和数据目录的应用标识 |
| `version` | 必填，1～4294967295 的整数 | 安装版本，更新必须大于已安装版本 |
| `data_version` | 必填，1～4294967295 的整数 | 数据格式约定；已有非零数据版本须与更新包一致 |
| `name` | 必填，1～47 个 UTF-8 字节 | 默认显示名称 |
| `default_locale` | 使用翻译表时必填 | 默认语言，必须存在于翻译表中 |
| `localized_names` | 与 `default_locale` 配套的语言名映射 | 每种翻译语言对应显示名称，语言集合须与翻译表一致 |
| `background` | 布尔值，默认 `false` | 是否提供 `epd_app_background` 受管服务描述符 |
| `settings` | 布尔值，默认 `false` | 是否接受 `page=settings` 的设置入口 |
| `lockscreen` | 布尔值，默认 `false` | 是否提供 `lockscreen=1` 的锁屏入口 |
| `file_extensions` | 字符串数组，默认空 | 文件处理类型；小写字母或数字，不带点，不重复 |

翻译命名、字符长度及语言包约束见[多语言](../development/multilingual.md)。声明后台、设置或锁屏能力后，应用实现对应入口。文件扩展名只登记处理能力，实际接收与读取流程见[文件内容交接](../development/content-handlers.md)。

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
