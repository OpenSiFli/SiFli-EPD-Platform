---
title: "多语言"
---

# 多语言

## 使用入口

在“设置 → 更多设置 → 语言”选择“简体中文”或“English”。平台先保存语言记录，再更新界面；保存失败时继续使用原语言。系统语言保存在 `/flash/system/language.bin`。

平台菜单、应用入口和应用自身提示随语言变化。书籍正文、文件名、用户输入和词库内容保持原文。天气应用当前固定以 `lang=zh` 请求服务器，因此返回的城市名、天气描述等仍为中文；这个限制不影响天气应用自身菜单和提示切换语言。

## 翻译源文件

翻译使用 UTF-8 JSON 表，平台位于 `src/ui/i18n/` 和 `src/ui/platform/i18n/`，应用位于 `modules/<id>/i18n/`。书架还合并 `modules/common/reader/i18n/strings.json`。

```json
{
  "locales": ["zh_cn", "en_us"],
  "strings": {
    "notes_title": {"zh_cn": "笔记", "en_us": "Notes"},
    "notes_count": {"zh_cn": "共 %u 条", "en_us": "%u notes"}
  }
}
```

文字标识表达业务含义，不使用中文原文作为查询键。源表的 `locales` 列出需要维护的语言，每个文字标识都必须提供这些语言的非空译文，格式参数也必须一致。共享表和应用表中的标识必须唯一。

生成器先校验全部源表语言，再选择实际编入语言包的集合。普通平台与应用构建编入源表中的全部语言；调用生成接口时可以通过 `c_locales` 选取子集，但未选中的源表列仍须通过校验。实际编译集合必须包含默认语言。

`tools/i18n.py` 校验源表后，直接调用 SDK `lang_excel_pack.py` 的 C 文件生成函数，产生 `lang_pack.h`、`lang_pack.c` 和各语言源文件；运行期继续使用 SDK 资源管理器。JSON 是项目的源表格式，不需要在构建时准备 Excel 或下载翻译资源。

平台生成目录为 `project/build_<board>/i18n/`，仓库内应用为 `modules/<id>/output/i18n/`。生成文件不手动编辑；普通构建会根据源表依赖更新生成物。

## 默认语言与应用名称

应用清单声明默认语言和显示名称：

```json
{
  "default_locale": "zh_cn",
  "localized_names": {"zh_cn": "笔记", "en_us": "Notes"}
}
```

这些字段与清单的 `id`、`name`、版本等字段一起维护。`id` 保持稳定；平台优先读取当前语言名称，再读取默认语言名称，均不可用时使用 `name`。本地化应用为支持的语言提供名称，默认语言必须存在于翻译表和实际编译的语言集合中。

语言代码采用小写 `语言_地区`，满足 `[a-z]{2,3}_[a-z]{2}`，例如 `zh_cn`、`en_us`、`fr_fr`。系统与应用使用完整代码匹配；目标语言不存在时使用应用默认语言。默认语言由清单决定，翻译表的排列顺序不会改变默认语言。

平台生成目录快照时读取本地化名称，不为名称加载动态模块。后台安装校验使用不翻译的元数据；UI 列表使用 `app_package_read_display()` 取得当前语言的显示名称。

## 查询文字与格式化

应用界面源文件包含自己的公共多语言接口：

```c
#include "platform/epd_i18n.h"

epd_app_text(title, epd_get_str(notes_title));
char text[64];
EPD_FMT_notes_count(text, sizeof(text), count); /* count is unsigned int */
epd_app_text(label, text);
```

`epd_get_str()` 使用当前应用的 SDK 语言资源管理对象。格式化宏按当前语言调用 `rt_snprintf()`，同时保留默认译文的字面量，让目标编译器检查参数类型。平台 UI 和使用语言表的应用模块均启用 `-Wformat -Werror=format`；格式错误会使构建失败。

当前格式集合支持整数、字符、字符串、固定宽度、`*` 宽度和 `%%`，以及整数的 `h`、`l` 修饰符。当前 SDK 启用了 `RT_PRINTF_PRECISION`，整数和字符串可以使用固定精度或 `*` 精度；动态宽度和精度参数也参与签名检查。`ll`、`hh`、`z`、位置参数、浮点和 `#` 不属于当前已接入集合。其他数值先转换为业务字符串，再用 `%s` 插入。译文之间的参数数量、顺序和类型必须一致。

输出遵循当前 SDK 的 `rt_snprintf`，不承诺与主机标准库的输出完全相同。该后端显式精度为零或负值时，字符串保留完整内容，整数不输出数字；需要截断字符串时使用正精度。格式检查器只检查合法语法和实参类型，实际输出遵循 SDK 的格式化实现。

输出缓冲、容量和业务实参只在实际输出分支执行一次。服务器文本和用户输入只能作为参数，不能作为格式字符串。

## 模块与页面生命周期

每个拥有语言表的模块只声明一组 SDK 初始化与清理入口：

```c
MODULE_INIT_DEF(NULL);
MODULE_CLEANUP_DEF(NULL);
```

已有业务初始化或清理函数时，将函数作为对应宏的回调参数。SDK 首次初始化先选择显式默认语言，再尝试系统语言。运行期间平台遍历已有模块记录，为模块选择目标语言，失败时显式回到默认语言；不重载模块、不重启后台服务。

SDK 清理宏先销毁语言资源，再执行最终业务清理回调。最终清理回调只释放剩余资源，不再查询文字。需要翻译的页面和服务状态在最终卸载前处理完毕。

页面在 UI 线程保存 `epd_locale_revision()` 的已应用版本。当前页面处理新版本，暂停页面在 `ONRESUME` 更新文字。只更新控件文字，不因为语言变化重置阅读位置、学习会话或下载任务。原始业务文字继续由 `epd_app_text()` 比较，不通过可能已被省略处理的 LVGL 标签内容判断变化。

## 后台结果与摘要

后台只发布业务结果码、参数和原始服务详情；页面持有需要继续显示的结果副本，并按当前语言生成提示。语言变化只重译仍在显示的结果，不重复执行业务请求，也不重新弹出已经关闭的提示。

天气摘要由受管服务的 `ui_process` 唯一发布。回调在同一次锁保护下取得业务快照和版本，再结合语言版本决定是否更新摘要。正常运行返回 `false`；收到 `stopping=true` 后释放自身 UI 资源并返回 `true`，允许服务管理器释放模块引用。后台线程不读取语言表、不操作控件。

## 构建与安装

在 `SiFli-EPD-Platform` 平台源码根目录执行以下命令，环境准备见[平台与应用构建](../build/building.md)。翻译表和生成工具已经纳入依赖，正常编译自动更新生成代码。完整 EPD 配套构建示例：

```powershell
. .\SiFli-SDK\export.ps1
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards APPS=all PREINSTALL=weather,books STORAGE_IMAGE=1 -j8
```

配套 `.sap` 安装包位于 `project/build_sf32lb57-epd-gyd7n6_hcpu/app-packages/`，天气和书架写入同次生成的 `fs_root.bin`。影响公共结构布局或函数契约的变化需要更新 ABI 并重建应用；构建标识仅记录来源配置，见[兼容规则](application-development.md#二进制兼容与公共符号)。

应用的 `SConstruct` 调用 `native_module.configure(repo, firmware, module_dir)`，为 SDK `BuildLibrary` 准备应用生成目录，并过滤固件的专属文字表目录。使用语言表时，`SConscript` 调用 `i18n.add_module_resources(objects, cwd)`，根据应用清单生成翻译代码并加入编译依赖；`exports.map` 保留应用入口和模块初始化、清理符号。可直接参考 `modules/hello/`。

应用自身的生成头文件优先于继承的包含目录，文字标识只依赖应用表及显式合并的共享表。没有翻译表的应用使用普通源码组，不调用 `add_module_resources()`。

生成目录中的 `i18n-report.json` 记录条目数、语言和字符串字节数。安装清单中的 `load_bytes` 表示模块可加载段的跨度；它与 `.so` 文件大小分别统计，不将字符串字节数再次加到已包含这些资源的加载跨度上。
