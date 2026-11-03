---
title: "界面与输入"
---

# 界面与输入

平台提供状态栏、导航、键盘、通知和媒体控制。动态应用使用 SDK `app_fwk` 创建页面，使用 LVGL 组织内容；所有控件和输入接口在 UI 线程调用。后台线程通过业务状态副本交接结果，页面与线程的退出顺序见[应用生命周期](application-lifecycle.md)。

## 页面与布局

普通页面显示平台页眉，正文放在页眉以下；沉浸页面使用完整画面，临时系统控件覆盖正文。页面创建时选择其中一种模式：

```c
#include "platform/epd_app.h"

static void configure_page(lv_obj_t *screen, bool immersive)
{
    if (immersive)
        epd_app_set_immersive(screen, true);
    else
        epd_app_header(screen);
}
```

页面模式随 screen 删除而释放，页眉由平台管理。普通页面的基准页眉高度是 64 像素，实际尺寸随界面缩放改变；通过 `epd_ui_content_area(screen, &area)` 取得可用区域，使用 `epd_ui_px()` 和角色字体组织内容。缩放版本改变或页面恢复时重新布局，字体选择见[图像与字体](application-resources.md)。

`epd_app_button()`、`epd_app_label()` 和 `epd_app_back_button()` 提供公共控件。标签后续使用 `epd_app_text()` 更新原始业务文字；函数返回 false 时保留待更新状态，下一次重试。返回按钮默认沿 SDK 页面关系返回，应用也可通过 `epd_app_back_button_cb()` 提供自己的业务回调。

## 输入作用域

应用为 screen 或弹窗根调用 `epd_app_input_group(owner)` 建立输入组。平台将其根对象及回调作为一个输入范围，优先选择当前最上层可见范围。删除 owner 自动释放输入组，应用不再单独删除组。

`epd_app_input_listen(owner, callback, data)` 注册 KEY2、KEY3 的按下、释放、点击和长按事件。KEY1 由系统管理。事件在 UI 线程执行；移除监听使用 `epd_app_input_unlisten()`，删除 owner 也会移除监听。单独创建输入组不会自动把实体键映射到业务动作。

`epd_app_input_back(owner, button)` 登记返回按钮。该按钮隐藏或禁用时阻止当前范围的返回，避免将操作转交下层页面。创建页面或弹窗时完成登记，并让回调上下文至少存活到对象删除。触摸、子页返回和计数状态可参考触摸示例（源码：`modules/hello/hello.c`）。

## 平台键盘

应用通过 `platform/epd_keyboard.h` 创建键盘，平台统一管理按键布局、拼音编辑、候选词、中英文切换和语音入口。应用提供原生 `lv_textarea`，设置允许字符、最大长度和确认后的业务动作。

```c
#include "platform/epd_keyboard.h"

epd_keyboard_config_t config = {
    .mode = EPD_KEYBOARD_TEXT,
    .chinese = true,
    .voice = true
};
lv_obj_t *keyboard = epd_keyboard_create(parent, textarea, &config);
if (keyboard) {
    lv_obj_set_height(keyboard, epd_keyboard_preferred_height(keyboard));
    lv_obj_add_event_cb(keyboard, on_confirm, LV_EVENT_READY, context);
    lv_obj_add_event_cb(keyboard, on_cancel, LV_EVENT_CANCEL, context);
}
```

`on_confirm`、`on_cancel` 是应用提供的 LVGL 事件回调。正在编辑拼音时，确认键先转换拼音；没有待转换文字时，再次确认才发送 `LV_EVENT_READY`。收起键丢弃未提交的拼音并发送 `LV_EVENT_CANCEL`，保留输入框中已经提交的文字。应用的取消回调用于隐藏键盘，不清空输入框或关闭编辑页面。

键盘支持 `TEXT`、`ASCII`、`NUMBER`、`PASSWORD` 四种模式，对应 `EPD_KEYBOARD_` 前缀的枚举。密码模式关闭中文和语音输入。输入框自身的字符限制和长度限制始终有效；传入空配置时默认使用允许中文及语音的文本模式。

以上接口在 UI 线程调用。删除键盘根对象会释放其子控件和输入法状态，输入框仍由应用管理；输入框删除后自动解除绑定。重新绑定输入框或更改模式会丢弃未提交的拼音。界面尺度改变后调用 `epd_keyboard_refresh()`，再按页面可用高度调整键盘位置和尺寸。

## 系统语音输入

系统自动跟踪触摸选中的普通原生 `lv_textarea`，并在打开语音入口时检查当前可见 `lv_keyboard` 绑定的输入框。应用保留原有输入框和 `LV_EVENT_VALUE_CHANGED` 回调即可接收识别文字，无需独立识别按钮或识别事件消费者。没有可用输入框、输入框处于密码模式或禁用状态、设置了允许字符限制时，麦克风入口不开始录音；隐藏、不可编辑或不属于当前页面的输入框也不能作为目标。

平台键盘还要求文本模式、`voice=true`，且没有待提交的拼音；应用通过键盘配置选择是否提供语音输入。

面板显示识别状态、临时草稿和录音时长。草稿不写入输入框；每段确定文字在当前光标位置插入，并触发原有 `LV_EVENT_VALUE_CHANGED` 回调。插入文字不触发消息发送，发送仍由应用原有按钮执行。

- 中央停止图标结束采集，等待最终结果并完成文字插入；面板保持打开。
- × 取消识别并关闭面板，丢弃未确定草稿，已经插入的确定文字保留。
- 键盘图标正常结束识别，收尾完成后关闭面板并回到原键盘。

跨页面、锁屏、存储交接或输入目标失效时，系统取消识别并关闭面板；旧会话结果不写入新页面或新输入框。单次录音保持 10 分钟上限，到时正常收尾。AI 对话使用这个系统入口接收语音文字，文字朗读由应用自己的语音会话提供。

服务配置见[语音服务接入](../services/speech-services.md)。

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
