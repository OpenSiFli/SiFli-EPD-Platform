---
title: "第一个应用"
---

# 第一个应用

本例将触摸示例复制为 `myapp`，生成 `.sap`，安装到设备，再发布一次更新。开始前完成[开发环境和平台构建](../getting-started/getting-started.md)，设备运行该源码版本的配套固件。

## 复制工程

在 `SiFli-EPD-Platform` 平台源码根目录执行以下命令；该目录包含 `modules/`、`project/` 和 `SiFli-SDK/`。本页后续命令沿用这个目录：

```powershell
New-Item -ItemType Directory modules/myapp
Copy-Item modules/hello/hello.c,modules/hello/SConstruct,modules/hello/SConscript,modules/hello/rtconfig.py,modules/hello/app.json,modules/hello/exports.map modules/myapp
Copy-Item modules/hello/i18n,modules/hello/assets modules/myapp -Recurse
```

这些文件构成示例的源码、资源和构建输入；应用输出由本次构建生成。工程各部分的用途见[动态应用开发](application-development.md#创建新应用)。

修改以下位置，使模块文件名、清单和页面入口使用同一 ID：

| 文件 | 修改 |
| --- | --- |
| `app.json` | `id` 改为 `myapp`，`name` 和 `localized_names` 改为自己的显示名称；首版 `version` 设为 1 |
| `SConstruct` | `BuildLibrary('hello.so', ...)` 改为 `BuildLibrary('myapp.so', ...)` |
| `SConscript` | 将分组名 `hello` 改为 `myapp` |
| C 源码 | `EPD_APP_DEFINE("hello")`、页面注册及跳转中使用的应用 ID 改为 `myapp` |
| `i18n/` | 修改页面文字对应的翻译值，保留翻译键及格式参数的一致性 |

检查复制后仍使用示例 ID 的位置：

```powershell
Get-ChildItem modules/myapp -Recurse -File | Where-Object { $_.Name -in @('SConstruct','SConscript','app.json') -or $_.Extension -eq '.c' } | Select-String 'hello'
```

按上表修改应用标识。应用 ID 为 1～7 个小写字母、数字或下划线，显示名称单独设置。翻译资源的生成方式见[多语言](multilingual.md)。

## 构建和安装

```powershell
scons -C modules APPS=myapp -j8
```

成功后取得 `project/build_sf32lb57-epd-gyd7n6_hcpu/app-packages/myapp.sap`。构建会转换应用图标、检查模块导入、生成最终清单并打包应用资源。

在[设备网页](../getting-started/device-management.md#安装应用)选择这个文件和安装位置，等待安装结果成功，然后从桌面打开应用。也可将 `.sap` 复制到 TF 卡目录，通过设备的应用安装页面选择文件；安装到 TF 卡后需加载卡内应用。

验证显示名称、首页按钮、子页进入和返回。返回父页后，父页计数应保留；彻底退出应用再打开时，示例的内存计数重新初始化。需要退出后保存的数据可参考[配置存储示例](application-storage.md#保存和读取配置)。

## 更新应用

修改一处页面文字，将 `app.json` 的 `version` 从 1 改为 2，重新执行同一构建命令并安装新的 `myapp.sap`。更新沿用原安装卷。打开应用检查新文字，确认安装结果和程序版本一致。

`version` 是安装版本，`data_version` 是数据格式约定。更新要求更高的安装版本，已有非零数据版本须与新包一致；字段说明见[应用清单](../reference/app-manifest.md)。

后续可按需要接入[存储](application-storage.md)、[图像与字体](application-resources.md)、[HTTPS](http-client.md)和[平台服务](../reference/api-reference.md)。
