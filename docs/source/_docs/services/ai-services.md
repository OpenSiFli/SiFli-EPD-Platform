---
title: "AI 服务"
---

# AI 服务

公共头文件为 `src/platform/epd_ai.h`，应用通过 `platform/epd_ai.h` 包含。平台管理端点、凭证、应用授权、协议编码和网络请求；应用管理提示词、历史、业务状态和工具执行。聊天应用用法见 ai_demo 源码（源码：`modules/ai_demo/src/app_main.c`）。

## 首次配置与调用

先连接可访问服务端的网络，并确认设备日期时间正确。通过[设备网页](../getting-started/device-management.md)进入“设置 → 平台 AI”，添加服务，填写服务名称、协议、HTTPS 端点、密钥和实际模型 ID。模型能力按服务提供方支持的功能选择；普通流式文本对话需要文本和流式能力。

保存并等待配置任务完成后，将服务设为默认项，在“应用访问授权”中选择应用和服务并授权。设备的系统 AI 设置页也提供服务与授权管理。网页读取配置时只返回密钥是否已保存，不返回原密钥。

打开 AI 对话应用，选择已授权服务与模型，发送一句短文本。收到完整回答即完成首次调用；配置保存成功只表示本地配置已提交。401 或 403 应核对服务凭证和权限，模型错误应核对模型 ID 与协议，TLS 错误应检查时间和证书校验信息，网络错误则先检查上行链路。具体诊断可读取本次请求结果的阶段和错误码。

## 配置与授权

在系统 AI 设置中配置服务、协议、模型和能力。平台支持 Responses、Chat Completions、Messages 三种协议；请求能力以所选模型的 `capabilities` 为准，包括文本、流式、工具、JSON、Schema 和原生记录续接。

UI 线程调用 `epd_ai_select_service(owner)` 打开授权选择页。授权后，应用用 `epd_ai_profile_list()` 或 `epd_ai_profile_get()` 读取脱敏副本，保存服务 ID、模型和 `identity_revision`。副本不包含凭证。默认服务是推荐项，已绑定服务失效时由用户重新选择。

平台最多保存 8 个服务，每个服务最多 8 个模型。服务配置及授权由 `src/services/ai/ai_settings.c` 和 `ai_config.c` 管理；设置页面位于 `src/ui/apps/ui_ai_settings.c`。

## 发起请求

`owner` 是应用的受管后台服务。`epd_ai_perform()` 和 `epd_ai_request_measure()` 必须在该服务登记的 worker 中调用；平台检查实际线程身份。

```c
epd_ai_cancel_init(&state->cancel);
epd_ai_message_t message = {
    .kind = EPD_AI_USER,
    .text = prompt,
    .text_size = strlen(prompt)
};
epd_ai_request_t request = {
    .struct_size = sizeof(request),
    .version = EPD_AI_VERSION,
    .profile_id = selected_profile,
    .model = selected_model,
    .messages = &message,
    .message_count = 1,
    .max_output_tokens = 256,
    .stream = true,
    .format = EPD_AI_FORMAT_TEXT
};
epd_ai_result_t result = {0};
epd_ai_error_t error = epd_ai_perform(
    owner, &request, &state->cancel, collect_event, state, &result);
/* 在这里读取或复制 result，再释放结果。 */
epd_ai_result_clear(&result);
```

请求指向的文字、消息、工具、取消令牌及回调上下文须保持有效到函数返回。取消令牌在请求返回前不重新初始化。页面只提交任务、调用 `epd_ai_cancel()` 或读取业务快照。

`collect_event` 在同一个 worker 中执行，事件字段只在回调期间有效。回调复制有限数据后返回；返回 `false` 请求取消。回调不访问 LVGL、不等待界面、不执行工具，也不嵌套发起请求。返回后平台不再调用本次请求的回调。

## 结果与历史

请求终态由 `epd_ai_perform()` 的返回值和 `result` 给出。检查 `error`、`complete`、`finish`，再决定是否将内容提交为完成的历史。需要继续协议上下文时还要检查 `replayable`，原样保存 `native` 及其长度，下一轮以 `EPD_AI_NATIVE_ASSISTANT` 回传；同一轮只使用一种助手历史表示。

取消、断连、缺少协议终态或超限会返回失败或不完整状态。应用可以保留已显示的文字，但不能将残缺记录作为完整原生历史。平台不自动重试 POST，也不自动切换服务或模型。

`result` 包含 HTTP 状态、错误类别、阶段、服务错误码、请求 ID、TLS 校验位和用量有效位。只有 `usage.valid` 标记的计数可用。读取或复制完毕后始终调用 `epd_ai_result_clear()`，包括失败路径。

## 结构化结果与工具

结构化输出通过 `format` 选择 JSON 或 Schema，并提供相应的 `schema_name`、`schema_json`。模型须声明所请求的能力；业务仍需校验返回数据。

工具在 `epd_ai_tool_t` 中声明名称、参数 JSON 和最大结果编码大小。事件中的参数片段只用于接收，工具执行须等请求完整返回。应用按以下顺序处理：

1. 检查完整终态、工具名称、参数及业务授权。
2. 使用 `epd_ai_request_measure()` 计算下一次请求的真实编码大小，预留工具结果及包装、转义后的字节数。
3. 预算足够后执行工具，保存结果。
4. 将原生助手记录与 `EPD_AI_TOOL_RESULT` 加入历史，使用对应 `call_id` 关联，再由应用决定是否请求下一轮。

平台不执行工具。`ai_demo` 使用文本聊天和原生历史续接，不提供工具执行界面。

## 容量与生命周期

| 对象 | 上限 |
| --- | --- |
| 编码后请求 | 128 KiB |
| 返回正文 | 64 KiB |
| 单个事件或非流式响应 | 512 KiB |
| 原生助手记录 | 256 KiB |
| 单项工具参数 | 16 KiB |
| 工具数量 | 8 |
| AI 服务共享内存预算 | 1536 KiB |

上限由 `epd_ai.h` 与 `src/services/ai/ai_internal.h` 定义。应用用实际编码器测量历史，超限时按业务规则减少完整历史或结束会话。连接、发送空闲、首个数据、读取空闲和总时限分别取自服务配置。

任务包含请求后的保存或工具处理时，应用另持 `epd_service_sleep_acquire()` 的休眠锁，全部收尾后释放。平台单次请求的锁独立管理。`flush()` 取消请求并安排 worker 保存，保存完成后才返回成功；存储会话失效时结束旧任务，不能写到新卡。停止和卸载规则见[后台生命周期](../development/application-lifecycle.md)。
