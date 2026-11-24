---
title: "界面与输入"
---

# 界面与输入

平台已经准备好状态栏、导航、键盘、通知和媒体控制，你只需专注于应用内容。使用 SDK 的 `app_fwk` 创建页面，用 LVGL 组织控件。

::: tip 重要提示
所有界面和输入接口都在 UI 线程调用。后台线程的结果通过数据副本传递给 UI 线程。
:::

## 页面模式

应用页面有两种显示模式：

**普通页面** - 显示平台页眉（时间、电量等），内容在页眉下方。适合大多数应用。

**沉浸页面** - 使用全屏显示，适合阅读器、图片查看等需要最大显示区域的应用。

创建页面时选择模式：

```c
#include "platform/epd_app.h"

// 普通页面 - 显示页眉
epd_app_header(screen);

// 沉浸页面 - 全屏显示
epd_app_set_immersive(screen, true);
```

页面模式会在 screen 删除时自动清理，你不需要手动释放。

## 获取可用区域

普通页面的页眉高度是 64 像素（会根据界面缩放调整）。用 `epd_ui_content_area()` 获取页眉下方的可用区域：

```c
lv_area_t area;
epd_ui_content_area(screen, &area);

// 在可用区域内布局内容
int x = area.x1 + epd_ui_px(20);
int y = area.y1 + epd_ui_px(20);
int width = area.x2 - area.x1 + 1 - epd_ui_px(40);
```

::: tip 响应缩放变化
用户可以调整界面缩放。在缩放改变或页面恢复时，重新布局你的控件。详见[响应式布局示例](code-examples.md#响应式布局支持缩放)。
:::

## 创建控件

平台提供常用控件的快捷创建函数：

```c
// 创建标签
const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
lv_obj_t *label = epd_app_label(screen, "Hello", 80, 200, 500, font);

// 创建按钮
lv_obj_t *btn = epd_app_button(screen, 80, 300, 524, 100,
                               "点击", on_click, user_data);

// 创建返回按钮
lv_obj_t *back = epd_app_back_button(screen);
```

## 更新文本

用 `epd_app_text()` 更新标签文本，它会自动检测变化，避免不必要的重绘：

```c
bool updated = epd_app_text(label, "新文本");
if (!updated) {
    // 内存分配失败，保留旧文本
    // 可以稍后重试
}
```

::: warning 注意
同一个标签的所有文本更新都要用 `epd_app_text()`，不要混用 `lv_label_set_text()`。
:::

## 实体按键

设备有三个实体按键：KEY1（系统保留）、KEY2 和 KEY3。应用可以监听 KEY2 和 KEY3。

### 创建输入组

每个页面或弹窗需要一个输入组来管理按键：

```c
// 为页面创建输入组
lv_group_t *group = epd_app_input_group(screen);

// 按钮会自动加入这个组
lv_obj_t *btn = epd_app_button(screen, ...);
```

删除 screen 时会自动释放输入组，不需要手动删除。

### 监听按键事件

```c
static void on_key_press(epd_app_key_t key, epd_app_key_event_t event, void *data) {
    if (key == EPD_APP_KEY2) {
        if (event == EPD_APP_KEY_CLICKED) {
            // KEY2 短按
        } else if (event == EPD_APP_KEY_LONG_PRESSED) {
            // KEY2 长按（3秒）
        }
    }
}

// 注册监听
epd_app_input_listen(screen, on_key_press, page_data);

// 移除监听（通常在页面关闭时）
epd_app_input_unlisten(screen);
```

::: tip 事件顺序
- 按下：`EPD_APP_KEY_PRESSED`
- 释放：`EPD_APP_KEY_RELEASED`
- 短按完成：`EPD_APP_KEY_CLICKED`（按下后快速释放）
- 长按触发：`EPD_APP_KEY_LONG_PRESSED`（按住 3 秒）
:::

### 返回按钮处理

用 `epd_app_input_back()` 指定哪个按钮处理返回操作：

```c
lv_obj_t *back_btn = epd_app_back_button(screen);
epd_app_input_back(screen, back_btn);
```

如果返回按钮被隐藏或禁用，返回操作会被阻止。这样可以防止用户在不合适的时候退出页面。

## 输入键盘

平台提供统一的输入键盘，自动处理拼音输入、候选词、中英文切换和语音输入。你只需创建一个 `lv_textarea` 输入框，剩下的交给键盘。

### 创建键盘

```c
#include "platform/epd_keyboard.h"

// 创建输入框
lv_obj_t *textarea = lv_textarea_create(parent);
lv_textarea_set_max_length(textarea, 100);

// 创建键盘并绑定输入框
epd_keyboard_config_t config = {
    .mode = EPD_KEYBOARD_TEXT,    // 文本模式
    .chinese = true,                // 支持中文
    .voice = true                   // 显示语音按钮
};

lv_obj_t *keyboard = epd_keyboard_create(parent, textarea, &config);
if (keyboard) {
    // 设置键盘高度
    lv_obj_set_height(keyboard, epd_keyboard_preferred_height(keyboard));
    
    // 监听确认和取消
    lv_obj_add_event_cb(keyboard, on_confirm, LV_EVENT_READY, page_data);
    lv_obj_add_event_cb(keyboard, on_cancel, LV_EVENT_CANCEL, page_data);
}
```

### 键盘模式

| 模式 | 说明 | 中文 | 语音 |
|------|------|------|------|
| `EPD_KEYBOARD_TEXT` | 普通文本 | ✅ | ✅ |
| `EPD_KEYBOARD_ASCII` | 英文字符 | ❌ | ✅ |
| `EPD_KEYBOARD_NUMBER` | 数字 | ❌ | ❌ |
| `EPD_KEYBOARD_PASSWORD` | 密码 | ❌ | ❌ |

### 处理输入结果

```c
static void on_confirm(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    
    // 获取输入的文本
    const char *text = lv_textarea_get_text(page->textarea);
    
    // 处理文本
    process_input(text);
    
    // 清空输入框
    lv_textarea_set_text(page->textarea, "");
}

static void on_cancel(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    
    // 用户取消输入，隐藏键盘
    lv_obj_add_flag(page->keyboard, LV_OBJ_FLAG_HIDDEN);
}
```

::: tip 确认键行为
如果正在编辑拼音（有待转换的字母），第一次按确认键会转换拼音，第二次才真正确认。这样用户可以完整输入拼音再转换。
:::

### 响应界面缩放

界面缩放改变后，调用 `epd_keyboard_refresh()` 更新键盘布局：

```c
// 检测到缩放变化
if (page->scale_revision != epd_ui_scale_revision()) {
    epd_keyboard_refresh(keyboard);
    page->scale_revision = epd_ui_scale_revision();
}
```

### 键盘生命周期

- 删除键盘会自动清理输入法状态
- 输入框仍由你的应用管理
- 删除输入框会自动解除与键盘的绑定
- 重新绑定输入框或切换模式会丢弃未提交的拼音

## 语音输入

用户点击键盘上的麦克风按钮可以语音输入。系统会自动识别语音并插入到输入框中，你不需要写额外代码。

### 自动工作

系统会：
1. 跟踪当前选中的输入框
2. 显示语音识别面板
3. 实时显示识别结果
4. 将确认的文字插入到光标位置
5. 触发你的 `LV_EVENT_VALUE_CHANGED` 回调

你只需保持原有的文本变化处理逻辑即可。

### 语音面板操作

用户可以通过三个按钮控制语音输入：

- **停止按钮**（中央）- 停止录音，等待最终识别结果
- **× 按钮** - 取消识别，丢弃未确定的文字
- **键盘按钮** - 结束识别，返回键盘

::: tip 识别文字处理
- 临时草稿：在面板中显示，不写入输入框
- 确认文字：插入到输入框，触发 `VALUE_CHANGED` 事件
- 插入文字不会自动发送消息，发送由你的按钮控制
:::

### 语音输入条件

语音输入会在以下情况启用：

✅ **可用条件**：
- 输入框可见且可编辑
- 输入框属于当前页面
- 键盘是文本模式（非密码模式）
- 键盘配置了 `voice = true`
- 没有正在编辑的拼音

❌ **不可用条件**：
- 密码模式
- 输入框禁用或隐藏
- 设置了字符限制（如只允许数字）

### 自动取消

以下情况会自动取消识别：
- 切换页面
- 锁屏
- TF 卡被拔出
- 输入框被删除

::: warning 录音时长限制
单次语音输入最长 10 分钟。到时会自动结束并插入已识别的文字。
:::

### 配置语音服务

语音识别需要配置在线服务，详见[语音服务](../services/speech-services.md)。

## 通知

通知接口位于 源码：`src/ui/platform/epd_system.h`，应用按需包含 `platform/epd_system.h`。通知使用 `epd_notification_t`，包含通知 ID、标题、正文、可选页面名及 `ongoing` 状态。`ongoing=true` 表示仍在进行的任务通知，`false` 表示可以独立保留的结果通知。

`epd_notification_publish(service, &notice)` 同步复制全部字段，应用传入的 `app_id` 由已注册服务的真实应用 ID 替换。发布结束后，调用方可以重用原结构体。字符串必须在各自数组内以零结束；无有效服务、字段不合法或新条目分配失败时返回 `false`，已有副本保持不变。

| 接口 | 用途与结果 |
| --- | --- |
| `epd_notification_publish()` | 每个应用最多保留一条可见通知。相同通知 ID 更新内容，新事件 ID 替换该应用旧通知并排到最前。相同内容不推进版本，进度更新不改变位置。 |
| `epd_notification_revision()` | 取得列表变化版本，供 UI 判断是否需要重读。 |
| `epd_notification_read()` | 在一次互斥保护中复制指定偏移的一页数据，同时取得总数和版本。新事件排在前面。 |
| `epd_notification_find()` | 按两个 ID 重新取得通知副本，供点击操作核查最新入口。 |
| `epd_notification_dismiss()` | 删除可见内容并保留该 ID 的删除记录，不调用应用的任务取消接口。 |
| `epd_notification_app_ids()` | 复制当前可见通知对应的应用 ID，去重后供状态栏显示图标。 |

这些数据接口使用互斥锁，可在工作线程或 UI 线程调用，不在中断上下文调用。发布者必须使用生命周期仍有效的受管服务；不保存服务停止后的旧句柄再次发布。需要读取语言表的文字，在应用的 UI 回调中根据业务快照生成，再发布副本。

通知与删除记录保存在本次运行的 RAM 中。同一服务运行期间，通知被删除后再次发布相同 ID 会返回成功，但不会重新显示；新的独立业务事件使用不同 ID，并替换旧内容或旧删除记录。一个应用的多个并行任务由应用汇总后发布。删除通知不停止下载、播放或其他任务。

通知中的 `page` 非空时，平台将页面名作为 SDK intent 的 `page` 参数传入应用；空值打开应用根入口。应用在启动流程中解释页面名。点击时平台重新核对通知和应用目录，不保存页面指针或直接调用模块回调。

## 媒体会话

媒体服务通过 `epd_media_publish(service, &media)` 发布 `epd_media_t`，包含曲目、作者、播放状态、支持的操作以及 SDK 音频类型 `audio_type`。固件复制结构体，最近发布的服务成为当前受控媒体会话；发布 `NULL` 撤回该服务的会话。平台不根据按钮点击推定已经开始播放，播放状态由服务按实际结果重新发布。

`epd_media_read()` 取得状态副本和版本。控制中心提供上一首、播放或暂停、下一首按钮，根据 `EPD_MEDIA_CAN_PREVIOUS`、`EPD_MEDIA_CAN_PLAY`、`EPD_MEDIA_CAN_PAUSE`、`EPD_MEDIA_CAN_NEXT` 决定按钮是否可用。`EPD_MEDIA_CAN_STOP` 仍是媒体接口的能力标志，控制中心没有单独的停止按钮。

媒体控制的交接顺序为：

1. 控制中心调用 `epd_media_send(app_id, command)`。
2. 固件检查当前会话、服务有效性和能力位，分配命令节点，加入先进先出队列，然后唤醒服务。失败时返回 `false`，不执行播放动作。
3. 服务工作线程从 `epd_service_wait()` 返回后，使用 `epd_media_poll(service, &command)` 逐项取出并执行命令。
4. 服务完成业务操作后重新发布实际状态，UI 根据新副本更新曲目及播放图标。

命令包括 `PREVIOUS`、`PLAY`、`PAUSE`、`NEXT`、`STOP` 五种值，不携带页面指针或模块回调。`send()` 返回成功表示已入队，不表示设备已经完成播放操作。切换媒体所有者、撤回会话或停止所属服务时，旧队列清空，命令不会被交给另一个播放器。

成功取出命令的 `epd_media_poll()` 已为服务取得幂等休眠锁。服务在命令执行、播放和资源收尾期间保持该锁，全部工作结束后调用 `epd_service_sleep_release()`。取命令失败或队列为空时不改变已有锁；取得休眠锁失败时命令继续留在队列。

需要与系统语音协调输出的播放器使用 `epd_media_publish_state(service, &media, item, quiet)`。同一次播放使用稳定的 `item`；暂停或关闭输出完成后才发布 `quiet=true`。播放器受理直接来自用户的播放、队列或定位操作时调用 `epd_media_note_intent()`，重复暂停也计入；经 `epd_media_send()` 入队的命令由平台登记。worker 执行完全部已接受命令后调用 `epd_media_intent_settled()`，并与直接命令入队过程同步。系统语音在确认原输出关闭后取得音频使用权，结束时仅恢复原服务、播放实例和用户操作状态仍匹配的会话。

媒体发布、读取、发送和取出接口均在任务上下文使用。控制中心在 UI 线程发送，应用在自己的受管工作线程取出；固件不会在发送者线程上执行播放器代码。音乐应用（源码：`modules/music/`）已经通过受管服务发布媒体会话，并处理上一首、播放、暂停和下一首命令。没有会话时显示“无媒体”，上一首、播放和下一首禁用。

## 动态锁屏

应用在清单中声明 `lockscreen` 后可作为锁屏候选，通过 SDK intent 的 `lockscreen=1` 参数识别锁屏启动。锁屏 UI 调用 `epd_lock_unlock()` 请求解锁，平台在回调返回后的主循环退出锁屏；应用继续按正常页面生命周期清理资源。

`epd_lock_selected()` 查询选择，空 ID 表示内置锁屏，`epd_lock_select(id)` 保存选择，`epd_lock_active()` 查询当前锁屏状态。这些公共接口只在 UI 线程调用。所选应用不可用时使用内置锁屏。接口声明见 源码：`src/ui/platform/epd_lock.h`，休眠与后台任务的关系见[电源管理](../services/power-management.md)。

## 显示更新和退出

页面只在可见内容或布局改变时更新控件。面板刷新期间的新画面会合并，刷新完成后提交最新内容；业务不要依赖每个中间帧都出现在墨水屏上。确需全刷时调用 `epd_app_request_full_refresh()`；普通页面不直接选择波形或操作显示电源。

页面关闭时释放页面控件和绘制资源；受管后台服务可按业务继续运行。通知和媒体状态由固件保存副本，不持有页面对象。服务停止时撤回媒体会话并清理命令队列，工作线程和 UI 收尾完成后才释放模块引用。TF 卡交接时关闭文件并响应取消，下一次加载使用新的存储会话，见[应用存储](application-storage.md)。
