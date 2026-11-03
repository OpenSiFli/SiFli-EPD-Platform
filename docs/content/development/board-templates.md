---
title: "内容看板模板"
---

# 内容看板模板

内容看板应用的标识为 `board`。模板用 JSON 描述页面标题和若干内容卡片，支持固定文本及平台服务摘要。布局由同一份 C 代码计算，可在设备上显示，也可在 PC 生成 SVG 预览。

## 配置位置

| 安装位置 | 安装包默认配置 | 可选用户配置 |
| --- | --- | --- |
| 内部存储 | `/flash/apps/board/res/layout.json` | `/flash/data/board/layout.json` |
| TF 卡 | `/sdcard/.epd/apps/board/res/layout.json` | `/sdcard/.epd/data/board/layout.json` |

路径通过平台存储 API 按实际安装卷解析。用户配置存在时优先使用；用户配置不存在时读取安装包默认配置。用户配置格式错误、无法读取或布局越界时，页面显示错误。配置在进入应用时读取，修改后退出并重新进入即可加载。

## 配置示例

```json
{
  "version": 1,
  "title": "我的看板",
  "layout": {"columns": 2, "margin": 24, "gap": 16, "padding": 20},
  "cards": [
    {"title": "今日重点", "text": "读二十页书。\n记录一个新想法。", "height": 240, "span": 2},
    {"title": "便笺", "text": "下午整理笔记。", "height": 280},
    {"title": "服务摘要", "summary": "weather", "empty": "暂无摘要", "height": 280}
  ]
}
```

| 字段 | 含义 |
| --- | --- |
| `version` | 必填，当前为整数 `1`。 |
| `title` | 必填，页面标题字符串。 |
| `layout` | 可选布局对象。省略时使用单列、24 像素外边距、16 像素间距、20 像素内边距。 |
| `layout.columns` | 正整数列数。 |
| `layout.margin` | 页面外边距，单位为像素。 |
| `layout.gap` | 卡片行列之间的间距，单位为像素。 |
| `layout.padding` | 卡片内容到边框的距离，单位为像素。 |
| `cards` | 必填卡片数组，可以为空。 |
| `cards[].title` | 必填卡片标题字符串。 |
| `cards[].text` | 固定正文，与 `summary` 必须二选一。字符串中的 `\n` 表示换行。 |
| `cards[].summary` | 摘要服务标识，由小写字母、数字和下划线组成；不会启动或安装对应服务。 |
| `cards[].empty` | 服务未安装、正在停止或内容为空时显示的文字，默认“暂无摘要”。 |
| `cards[].height` | 必填卡片总高度，单位为像素。 |
| `cards[].span` | 卡片占用的列数，默认 `1`，不能大于 `columns`。 |

布局从左到右排列，剩余列不足时换行，每行以最高卡片决定下一行的位置。页面顶部保留 64 像素返回按钮及标题区域。每张卡片为标题保留 44 像素，为正文至少保留 28 像素，因此卡片高度不得小于 `2 × padding + 72`。完整卡片必须落在屏幕边界内，文字在固定文本框中显示，超出文本框高度的部分裁切。

所有布局数值必须是非负整数，列数、跨度和高度必须大于零。重复字段、未知字段、整数溢出和越界会被拒绝。配置容量与卡片数量取决于实际可分配内存；应用不设置固定卡片数量或文件字节数上限。

## 参考组合

- 源码：`modules/board/res/layout.json`：两列日常看板，包含跨列重点、便笺、可选天气服务摘要和阅读摘记。未安装天气应用时仍可使用全部静态卡片，摘要卡片显示空状态。
- 源码：`modules/board/res/templates/reading.json`：单列阅读计划、可选学习服务摘要和随手记。可以复制为用户配置后修改正文。

## 同源 PC 预览

在 `SiFli-EPD-Platform` 平台源码根目录、已配置 C 编译器的命令行中执行本节命令：

```text
python tools/preview_board.py
python tools/preview_board.py --data path/to/layout.json --output modules/board/custom-preview.svg
python tools/preview_board.py --summary "weather=晴，适合散步"
```

Windows 可使用 Visual Studio 的 x64 Native Tools 命令行；Linux 可使用 GCC 或 Clang。脚本默认使用 `684 × 1216` 视口，可以通过 `--width` 和 `--height` 指定其他尺寸。预览样例摘要通过 `--summary ID=TEXT` 传入，按平台摘要缓冲区保留最多 95 字节正文。

默认生成 `modules/board/preview.svg`。预览编译 `board_model.c` 和 SDK 自带 cJSON，卡片及文本框的几何布局与设备共用 `board_layout()`。SVG 使用浏览器字体及排版引擎，字体栅格、换行和设备效果不完全一致。请用支持 SVG `foreignObject` 的浏览器打开预览。

使用 `scons -C modules APPS=board -j8` 构建看板。环境准备见[构建说明](../build/building.md)，模板解析、存储与页面实现见 源码：`modules/board/`。
