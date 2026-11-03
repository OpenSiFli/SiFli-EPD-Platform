---
title: "应用接口参考"
---

# 应用接口参考

动态应用使用 SDK 的 LVGL、RT-Thread 和 `app_fwk`，再按功能包含平台公共头文件。以下接口由固件提供；应用无需把对应 `.c` 编入模块。`modules/common/` 的源码组件另见[公共组件](../development/common-components.md)。目标固件是否提供所调用符号，由应用构建的导入检查确认。

## 页面与控件

头文件：platform/epd_app.h（源码：`src/ui/platform/epd_app.h`）。本节所有接口均在 UI 线程调用。

`EPD_APP_DEFINE(id)` 导出 `epd_app_info_t { abi, id }`，与 `app_main(intent_t)` 构成主模块入口。页面状态由 `gui_app_regist_msg_handler_ext()` 申请，通过 `gui_app_this_page_memory()` 访问；事件顺序见[应用生命周期](../development/application-lifecycle.md)。

| 函数 | 参数、结果与所有权 |
| --- | --- |
| `epd_app_object_init(obj)` | 为应用对象初始化平台基础样式；对象仍由页面拥有 |
| `epd_app_label(parent,text,x,y,width,font)`、`epd_app_centered_label(...)` | 创建普通或居中文字标签，返回归属于 parent 的控件 |
| `epd_app_panel(parent,x,y,width,height)` | 创建页面面板 |
| `epd_app_icon(parent,src,x,y)` | 使用图像描述符创建图标；描述符及像素需在使用期间有效 |
| `epd_app_button(parent,x,y,width,height,text,callback,data)` | 创建按钮并加入最近的受管输入组；data 在回调期间保持有效 |
| `epd_app_text(label,text)` | 复制文本并检测变化；失败返回 false，业务保留待更新状态重试；同一标签后续文字更新沿用此函数 |
| `epd_app_header(screen)`、`epd_app_header_refresh(screen)` | 声明普通页面并刷新平台页眉 |
| `epd_app_set_immersive(screen,immersive)` | 声明沉浸页面，声明随 screen 删除 |
| `epd_app_back_button(screen)`、`epd_app_back_button_cb(parent,callback,data)` | 创建标准返回按钮；后者由业务提供回调 |
| `epd_app_back_button_layout(button,x,center_y)` | 按标题中心对齐返回按钮 |
| `epd_app_status(out)` | 把电量和时钟写入调用者的 `epd_app_status_t` |
| `epd_app_set_recent_reading(title,percent)` | 发布最近阅读信息供系统展示 |
| `epd_app_open_text_settings()` | 请求打开阅读文字设置，返回是否受理 |
| `epd_app_request_full_refresh()` | 请求一次全刷 |
| `epd_app_refresh_submission()`、`epd_app_refresh_done()` | 查询提交序号和显示完成状态，用于资源交接 |
| `epd_app_clean_draw_buffer(buffer)` | 按平台绘制路径清理缓冲缓存状态 |
| `epd_app_ui_available()` | LVGL 私有堆可用量快照，0 表示不可取得 |

## 输入与尺度

`epd_app_input_group(owner)` 为 screen 或弹窗根创建输入范围，删除 owner 自动释放组。实体键使用 `epd_app_input_listen(owner,callback,data)` 注册，返回 false 表示失败；`epd_app_input_unlisten(owner)` 移除。回调参数为 KEY2/KEY3、按下/释放/点击/长按事件及 data；只分发给最上层可见范围。KEY1 由系统管理。`epd_app_input_back(owner,button)` 将返回动作交给按钮，隐藏或禁用按钮会阻止该范围的返回。

platform/epd_ui_scale.h（源码：`src/ui/platform/epd_ui_scale.h`） 提供 UI 线程度量：

| 接口 | 含义 |
| --- | --- |
| `epd_ui_scale_percent()`、`epd_ui_scale_revision()` | 当前缩放百分比和变化版本 |
| `epd_ui_px(base)`、`epd_ui_touch(base)` | 将设计尺寸转换为像素或触摸尺寸 |
| `epd_ui_content_area(screen,out)` | 将当前页面可用区域写入 `lv_area_t` |
| `epd_ui_font_at(index,role)` | 预览五档设置之一的角色字体；失败为 NULL，返回字体由平台拥有 |

platform/epd_keyboard.h（源码：`src/ui/platform/epd_keyboard.h`） 中 `epd_keyboard_config_t` 包含 mode、chinese、voice。mode 为 TEXT、ASCII、NUMBER 或 PASSWORD，密码模式关闭中文和语音。`epd_keyboard_create(parent,textarea,config)` 返回复合控件，NULL config 选择带中文和语音的文本输入。键盘由父对象管理，输入框仍由应用拥有。

用 `epd_keyboard_set_textarea/get_textarea` 更换或查询目标，用 `epd_keyboard_set_mode` 修改模式；重新绑定和改模式丢弃未提交拼音。`epd_keyboard_refresh` 更新缩放字体和布局，`epd_keyboard_preferred_height` 返回期望高度。READY/CANCEL 回调注册在复合控件上，事件及布局示例见[平台键盘](../development/touch-ui.md#平台键盘)。

## 字体与图像

| 接口 | 结果与生命周期 |
| --- | --- |
| `epd_app_font()`、`epd_app_font_role(role)`、`epd_app_font_style(role,flags)` | 平台拥有的界面字体；flags 可组合 BOLD 和 ITALIC，失败回到普通角色字体 |
| `epd_app_reading_font_begin()` | 新阅读会话开始时恢复可用字体选择，UI 线程调用一次 |
| `epd_app_reading_font_id/revision()` | 实际字体稳定 ID 和内容版本 |
| `epd_app_font_create(family,size,weight,identity,fallback)` | UI 线程同步创建内置 MiSans 界面字体，family 必须为 0；identity、fallback 输出指针均必填，fallback 返回 NULL，返回字体由应用拥有 |
| `epd_app_reading_font_create(family,size,weight,identity,fallback)` | UI 线程创建异步阅读字体；family 为目录稳定 ID，返回值由应用拥有，fallback 返回 NULL；等待 status==1 并准备所需字形后才绘制 |
| `epd_app_font_status/metrics/glyph_ready` | status 在 UI 线程发布度量；metrics 与 glyph_ready 读取线程安全快照；返回 -1 失败、0 等待、1 就绪 |
| `epd_app_font_glyph_acquire/release` | 持有字体引用的线程获取字形快照及独占释放凭证；每次成功获取都配对释放，像素借用期到 release 为止 |
| `epd_app_font_destroy(font)` | 去除控件与绘制引用后销毁自有字体 |
| `epd_app_font_prepare(font,letter,adjacent)` | 请求准备字形；异步阅读字体返回 true 也可能仍在等待，应使用 glyph_ready 检查最终状态 |
| `epd_app_font_derived_acquire/store/release/drop` | 获取、复制保存、释放派生字形引用及使字体实例的缓存失效；精确参数与绘制约束见头文件 |
| `epd_app_builtin_font_data(size)` | 可选不可变字体字节；文件型字体返回 NULL 且 size 为 0 |
| `epd_app_image_set(image,path,fallback)`、`epd_app_image_release(image)` | UI 线程为控件保留或释放 EZIP；删除控件自动释放 |

类型和完整声明见 源码：`src/ui/platform/epd_app_font.h`、源码：`src/ui/platform/app_image.h`。路径、示例和释放顺序见[图像与字体](../development/application-resources.md)。

## 后台任务和摘要

头文件：源码：`src/services/app_service.h`。模块导出 `epd_app_background`，类型为 `epd_background_t`：

| 成员 | 执行位置与契约 |
| --- | --- |
| `run(service)` | 受管 worker，收到停止后关闭请求和句柄并返回 |
| `stack_size`、`priority` | 任务栈及优先级，应用范围见[后台服务](../development/application-development.md) |
| `network` | 是否接收网络变化唤醒 |
| `command(argc,argv)` | 服务命令入口；模块在命令返回前保持有效 |
| `ui_process(service,stopping)` | 有界 UI 步进；worker 和命令返回后 stopping 才为 true，释放完 UI 资源后返回 true |
| `flush()` | UI 线程保存屏障；未保存或失败返回 false，保存持久化完成后返回 true |

| 函数 | 行为 |
| --- | --- |
| `epd_service_wait(service,ticks)` | 等待并返回 WAKE、NETWORK、STOP 事件位 |
| `epd_service_cancelled(service)` | 查询停止状态，接受新任务前检查 |
| `epd_service_wake(service)` | 唤醒任务 |
| `epd_service_sleep_acquire/release(service)` | 每个服务幂等的休眠锁，申请失败不接受新任务，所有工作及清理结束后释放 |
| `epd_service_ui_request(service,delay_ms)` | 请求下一次 UI 步进，重复请求保留最早期限 |
| `epd_service_publish_summary(service,text)` | 发布文字副本 |
| `epd_app_summary_revision()`、`epd_app_summary_read(id,text,capacity)` | 读取驻留摘要版本和副本；读取失败返回 false |
| `app_service_start(id,definition,module)` | UI 线程启动服务，返回句柄或 NULL；动态应用传入自身 SDK 模块句柄 |
| `app_service_stop(id)`、`app_service_running(id)` | UI 线程请求取消并查询是否仍存活，停止请求不等于已清理完成 |
| `epd_service_is_worker(service)` | 检查调用线程是否为该服务尚未结束的 worker |

## 存储与内存

源码：`src/services/storage.h` 的 volume 为 FLASH 或 SD。`storage_root/path_root/relative_path` 返回静态或借用字符串，不释放。`storage_info(volume,out)` 复制 mounted、total、used；`storage_available/path_available` 检查可访问性。`storage_revision`、`storage_changing` 及 `storage_card_session/session_valid` 描述介质变化；原始 I/O 使用同线程配对的 `storage_lock/unlock`。

源码：`src/services/storage_file.h` 提供同步辅助函数：

| 函数 | 参数与结果 |
| --- | --- |
| `storage_app_path(out,size,id,area,relative)` | 把安装卷对应路径写入 out，失败返回 false，不创建目录 |
| `storage_app_available(id)` | 查询应用安装卷可用性 |
| `storage_mkdirs(path)` | 创建目录，返回是否成功 |
| `storage_crc32(data,size)` | 计算字节数据的 CRC32 |
| `storage_record_load/save(path,magic,version,data,size)` | 按标识、版本、长度及 CRC 读写记录；缓冲由调用者拥有，返回是否成功 |
| `storage_file_recover(path)` | 恢复中断的文件替换 |
| `storage_file_commit(temporary,path)` | 发布已写好的临时文件 |
| `storage_file_replace(path,data,size)` | 同步复制并替换完整文件 |
| `storage_file_import(source,destination)` | 复制文件并提交目标 |
| `storage_file_signature(path,out)` | 读取内容生成缓存标记，成功写入 out |

完整操作示例见[应用存储](../development/application-storage.md)。platform/epd_memory.h（源码：`src/platform/epd_memory.h`） 的 `epd_app_alloc/realloc/free`、`epd_app_psram_available` 和 `epd_thread_create_psram` 的区域、线程和释放规则见[应用内存](../development/application-memory.md)。

## 网络、AI 和语音

源码：`src/services/net/network.h` 中 `network_snapshot_t` 包含 ready、source、generation、IPv4 address。`network_snapshot(out)` 复制当前上行状态，`network_ready()` 查询链路就绪；ready 不表示公网服务可达。`network_interface_snapshot(source,out)` 可读取指定接口，返回 RT_EOK 表示成功取得副本，连接情况仍看副本字段。地址均为网络字节序。

`network_subscribe(event,bits)` 返回订阅标识，网络变化向事件发送 bits；`network_unsubscribe(id)` 等待在途发送结束，随后才能销毁 event。网络线程不执行应用回调。见[网络服务](../services/wifi.md)。

| API 组 | 类型、调用方式及结果 |
| --- | --- |
| `epd_ai_profile_list/get` | 使用 owner 读取 `epd_ai_profile_t` 脱敏副本，返回 `epd_ai_error_t` |
| `epd_ai_select_service` | UI 线程打开 owner 的服务选择和授权页面 |
| `epd_ai_perform` | owner 的 worker 提交 `epd_ai_request_t`、取消令牌、事件回调及 result；同步返回终态错误码 |
| `epd_ai_request_measure` | 同一 worker 用实际编码器测量请求预算 |
| `epd_ai_cancel_init/cancel` | 初始化或设置取消令牌，请求结束前保持令牌有效 |
| `epd_ai_result_clear` | 释放成功或失败请求的结果资源 |
| `epd_speech_capability` | 查询 ASR/TTS 能力副本 |
| `epd_speech_start` | owner、kind 和 request 创建异步会话，成功写入 session |
| `epd_speech_append` | 复制已接受的 UTF-8 前缀，返回字节数、0 或负错误码 |
| `epd_speech_finish/cancel/pause/resume/stop` | 针对 owner/session 请求异步状态变化，返回是否受理的错误码 |
| `epd_speech_snapshot` | 复制会话状态 |
| `epd_speech_poll/event_release` | 单一消费者读取事件，处理后释放文字和资源 |
| `epd_speech_error_name` | 返回错误名称 |

请求字段、能力位、事件与终态分别见 源码：`src/platform/epd_ai.h`、源码：`src/platform/epd_speech.h`，完整示例见 [AI 服务](../services/ai-services.md)、[语音服务](../services/speech-services.md)。

## 通知、媒体和声音

头文件：源码：`src/ui/platform/epd_system.h`。通知和媒体在任务上下文发布副本，字符串在结构数组内必须以零结尾。

`epd_notification_t` 包含事件 ID、title、text、目标 page 和 ongoing，app_id 由 owner 指定。`epd_notification_publish(service,value)` 返回是否接受；每应用最多一条可见通知。`revision/read/find` 读取版本、分页副本或指定通知，`dismiss` 删除指定通知，`app_ids` 返回当前发布者 ID。被用户删除的同一事件 ID 不重新显示；新业务事件使用新 ID。

`epd_media_t` 包含 artist、title、audio_type、capabilities 和 playing。`epd_media_publish` 发布状态，传 NULL 撤回；`epd_media_publish_state` 还提交播放实例 item 和静音确认 quiet。`epd_media_read` 读取副本，`epd_media_send` 排队命令，worker 用 `epd_media_poll` 取出。成功取命令时已取得服务休眠锁，完成所有任务后释放。直接接收用户命令时使用 `note_intent`，执行完成后使用 `intent_settled`。见[媒体会话](../development/touch-ui.md#媒体会话)。

源码：`src/ui/platform/epd_sound.h` 的 `epd_sound_read(out)` 只复制内存状态；`set_volume(channel,percent)`、`set_muted(muted)` 保存偏好，失败恢复原状态。百分比为 0～100，静音单独保存。`tone_valid/get_tone/set_tone` 验证或设置分类提示音；`preview` 返回试听请求是否受理，`preview_stop` 停止试听。`notify/touch` 向常驻声音 worker 请求提示音，不在调用线程播放。

阅读字体接口的完整声明见 源码：`src/ui/platform/epd_app_font.h`。`status` 在 UI 线程发布字体度量；worker 使用只读度量快照及成对的字形 acquire/release 接口。异步字体返回对象不等于字形已经准备好，绑定前需要检查对应就绪状态。

## 内容、私有库和阅读

| 接口 | 参数与线程 |
| --- | --- |
| `app_content_query(path,first,items,capacity,count)` | UI 线程枚举处理者副本，不加载模块 |
| `app_content_open(id,path)` | UI 线程排队打开，RT_EOK 表示 Intent 已入队 |
| `app_content_receive(intent,path,size)` | 复制入站路径，-ENOENT 表示普通启动，介质变化或空间不足返回错误 |
| `app_content_error(error)` | 取得错误说明 |
| `epd_app_library_open(app_id,name)` | UI 线程打开所属应用的私有库，失败为 NULL |
| `epd_app_library_symbol(library,symbol)` | 查找库接口，返回符号地址或 NULL |
| `epd_app_library_close(library)` | worker 及库结果全部结束后在 UI 线程释放 |

完整定义见 源码：`src/ui/platform/app_content.h`、源码：`src/ui/platform/epd_app_library.h`。阅读会话、跨核计算及 PCM 的状态、参数和释放要求见[公共组件](../development/common-components.md)，多语言宏和语言版本查询见[多语言](../development/multilingual.md)。

## 时间、语言和锁屏

源码：`src/ui/ui_settings.h` 提供 UI 线程的设置查询。`ui_settings_value(id)` 返回格式化文字，借用期到下一次设置文字查询，需要长期保留时复制。`ui_settings_enabled(id)` 查询该项是否可操作；`ui_settings_cycle(id)` 经系统设置服务切换下一项并返回结果，阅读字体由字体目录管理。`ui_settings_index(id)` 通常返回选项下标，FONT 返回实际字体稳定 ID，并需结合阅读字体 revision 判断内容变化。可用 id 见头文件的 `ui_setting_id_t`。

源码：`src/services/system_time.h` 的接口在任务上下文使用，设置和日历输出均为副本：

| 函数或类型 | 行为与参数 |
| --- | --- |
| `system_time_settings_t` | timezone_minutes 为 -720～840 的固定分钟偏移，不应用夏令时；automatic 表示自动校时，hour24 表示 24 小时显示 |
| `system_time_get_settings(out)`、`system_time_get_utc()` | 复制当前显示设置，或取得 UTC 秒数 |
| `system_time_utc_valid(utc)`、`system_time_valid()` | 判断给定 UTC 或当前系统时间是否有效 |
| `system_time_local_from_utc(utc,out)`、`system_time_get_local(out)` | 将本地日历写入调用者的 `struct tm`，失败返回 false |
| `system_time_local_day(utc)` | 取得按当前时区计算的日编号，无效 UTC 返回 INT32_MIN |
| `system_time_format(utc,locale,with_date,out,size)` | 按语言及 12/24 小时设置输出；无效时间写占位符并返回 false，缓冲不足不输出截断的 UTF-8 字符 |

源码：`src/ui/platform/epd_locale.h` 提供 UI 线程的 `epd_locale_get()` 和 `epd_locale_revision()`。语言代码字符串由固件持有，生命周期与固件一致；应用依据版本更新自己的文字和布局。

源码：`src/ui/platform/epd_lock.h` 提供 UI 线程的 `epd_lock_selected()`、`epd_lock_select(id)`、`epd_lock_active()` 和 `epd_lock_unlock()`。空 ID 选择内置锁屏；选择返回是否成功。解锁请求在调用回调返回后执行，调用方不能立即假设锁屏页面已销毁。动态锁屏声明见[应用清单](app-manifest.md)。

## JPEG 与 SVG

源码：`src/platform/epd_image.h` 的 `epd_image_jpeg_layout(width,height,out)` 查询 HAL 所需的工作区和输出平面大小。`epd_jpeg_layout_t` 包含 width、height、stride、work_size、y_size、u_size、v_size、buffer_size；有效图像为 width×height，Y 行跨度为 stride。

`epd_image_jpeg_decode(input,length,layout,buffer,cancel,context)` 在 worker 中同步解码 baseline YCbCr 4:2:0 JPEG。使用查询得到的原始 layout；输入按 32 字节对齐并补齐，RAM1 工作和输出缓冲按 64 字节对齐，容量为 buffer_size。GPU 锁忙时立即返回 false；函数返回后，包括取消或失败，硬件已停止访问这些缓冲。

源码：`src/platform/epd_svg.h` 的 `epd_svg_render(svg,length,max_width,max_height,load,user,cancel,out)` 在 LVGL 初始化后从线程调用，输出白底紧密排列的 gray8。`epd_svg_bitmap_t` 包含 pixels、width、height；成功后用 `epd_app_free(out.pixels)` 释放，失败时 out 为空。

SVG 输入上限 256 KiB，输出上限 684×1216，单张嵌入图片上限 384×608，适配尺寸不放大原图。加载图片的回调用 `epd_app_alloc` 分配像素，返回后像素所有权交给渲染器，包括同时返回错误的情况；加载和取消回调不能重入渲染。状态码为 0 成功、-1 参数无效、-2 取消、-3 内存不足、-4 不支持、-5 I/O 失败。

更改公共接口时需同步声明、实现、导出和配套应用，具体流程见[平台扩展](../contributing/platform-development.md#新增公共接口)。
