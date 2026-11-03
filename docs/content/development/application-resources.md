---
title: "图像与字体"
---

# 图像与字体

应用自有资源与源码一起放在 `modules/<id>/`，随 `.sap` 安装。平台公共图标和字体由主固件提供；应用通过接口引用公共资源，私有内容通过安装路径访问。本页构建及图标生成命令在 `SiFli-EPD-Platform` 平台源码根目录执行。

## 随应用打包文件

普通应用的打包入口收集 `assets/` 和 `res/`，去掉这一级目录名后合并到包内 `res/`。例如 `assets/data.json` 和 `res/cover.ezip` 分别安装为 `res/data.json` 和 `res/cover.ezip`。同名或文件与目录的路径冲突会报错，大小写不同的同名也属于冲突。入口图标 `assets/icon.png` 单独处理。放入资源后执行 `scons -C modules APPS=<id>`。

应用入口图标使用 `assets/icon.png`，构建工具调用 SDK 转换为包根的 `icon.ezip`。图标来源和授权保存在应用自己的 `assets/icon.source.json`、`assets/icon.LICENSE.txt`。天气、书架和单词还通过 源码：`tools/native_apps.py` 收集专用资源；新增普通应用从通用资源目录开始即可。

运行时查询资源路径：

```c
#include "storage_file.h"

static bool image_path(char *path, size_t size)
{
    return storage_app_path(path, size, "myapp",
                            STORAGE_APP_CODE, "res/cover.ezip");
}
```

EZIP 是 SDK 的压缩图像格式。通用 `res/` 收集保留文件内容，不会把任意 PNG 自动转换为 EZIP；需要压缩图像时使用 SDK 转换器，参数和 LVGL V9 文件头校验可参考 `native_apps.py` 的 `add_package()`。JPEG、PNG 等外部文件按格式解码，相关源码在 源码：`modules/common/image/`。

## 显示和释放图像

在 UI 线程为 `lv_image` 控件调用 `epd_app_image_set(image, path, fallback)`，接口位于 源码：`src/ui/platform/app_image.h`。成功后组件为该控件保留压缩图像；传入 `NULL` 路径使用固件后备图像，相同来源不会重复触发重绘。

页面 `ONPAUSE` 时调用 `epd_app_image_release()`，在 `ONRESUME` 时重新设置图像。删除控件也会自动释放。若路径位于 TF 卡，会话失效后停止读取并释放引用；应用不能在卸载卷后继续保留文件访问任务。

## 界面字体

按钮、标题和说明文字使用 `epd_app_font_role()` 返回的角色字体，随系统界面缩放调整。合成粗体、斜体可通过 `epd_app_font_style()` 取得；这些返回值由平台拥有，应用不释放。

用角色字体实际度量布局，并在界面尺度变化时重新排版。示例见触摸示例（源码：`modules/hello/hello.c`）与[界面输入和字体](application-development.md#界面输入和字体)。

## 阅读字体

阅读正文可以选用内置 MiSans 或用户安装的静态 TrueType `.ttf`。字体可从网页或设备字体管理页面安装，单个文件最大 32 MiB，安装目标为内部存储或 TF 卡。安装、预览和切换见[设备管理](../getting-started/device-management.md#字体和服务设置)。

应用在打开新的阅读会话时调用一次 `epd_app_reading_font_begin()`，然后读取实际字体 ID 和 revision。字体 ID 是稳定目录标识，不是列表下标；同一 ID 内容变化时 revision 也可能变化，复用排版前比较两者。

UI 线程调用 `epd_app_reading_font_create(family, size, weight, &identity, &fallback)` 创建异步阅读字体。`family` 是目录 ID；返回字体由应用拥有，`fallback` 返回 NULL。返回对象后，界面仍需等待 `epd_app_font_status()` 为 1 才使用度量；字形像素通过准备接口排队，并检查对应就绪状态。已经确认缺字时才使用远端 MiSans 补字，加载等待和来源错误不按缺字处理。

后台排版使用 `epd_app_font_metrics()` 读取度量副本，合成时用 `epd_app_font_glyph_acquire()` 获取带有效期的字形数据，完成后调用 `epd_app_font_glyph_release()`。移除控件和绘制引用后，再以 `epd_app_font_destroy()` 释放字体。来源、字号、字重和版式版本共同决定缓存是否可复用，不能只比较字体指针。

`epd_app_font_create()` 仍提供同步的系统界面 MiSans，只接受 `family=0`；需要外置阅读字体时使用上述阅读专用创建入口。完整参数、引用及线程契约见 源码：`src/ui/platform/epd_app_font.h`，字体供数与 ACPU 流程见[阅读架构](../architecture/reading-architecture.md)。

## 重新生成平台图标

平台图标源文件及映射位于 源码：`assets/icons/`，应用图标位于各自 `assets/`。从 SVG 重新生成 PNG 需要 Node.js、npm 和 `sharp`。以下示例使用 Node.js 22.x 与 `sharp` 0.34.4。

在平台源码根目录安装本地图标依赖并确认可加载：

```powershell
npm install --no-save --package-lock=false sharp@0.34.4
node -e "console.log(require('sharp').versions.sharp)"
```

再按修改范围选择生成器：

```powershell
node tools/generate_ui_icons.cjs
node tools/generate_app_icons.cjs --apps hello
node modules/weather/tools/generate_icons.cjs
```

应用图标命令中的 `hello` 替换为实际 ID，多个 ID 以逗号分隔；省略 `--apps` 会生成所有具备 SVG 来源的应用图标。系统图标 PNG 由 源码：`assets/SConscript` 转为固件 EZIP C 资源。普通固件与应用构建使用已有 PNG，无需安装这组 Node.js 依赖。
