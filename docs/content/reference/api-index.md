---
title: "完整 API 参考索引"
---

# 完整 API 参考索引

本文档提供 Bloom UI 平台所有公共 API 的完整索引，按功能模块分类。每个 API 包含函数签名、参数说明、返回值、使用示例和相关文档链接。

> **目标读者**：嵌入式应用开发者，使用 SDK 开发基于 LVGL 和 RT-Thread 的动态应用。

## 目录结构

- [页面与控件](#页面与控件)
- [输入与尺度](#输入与尺度)
- [字体与图像](#字体与图像)
- [后台任务和摘要](#后台任务和摘要)
- [存储与内存](#存储与内存)
- [网络服务](#网络服务)
- [AI 服务](#ai-服务)
- [语音服务](#语音服务)
- [通知、媒体和声音](#通知媒体和声音)
- [内容处理](#内容处理)
- [时间与语言](#时间与语言)
- [图像解码](#图像解码)

---

## 页面与控件

**头文件**：`platform/epd_app.h`（源码：`src/ui/platform/epd_app.h`）

> **注意**：本节所有接口均在 UI 线程调用。

### 应用入口

#### EPD_APP_DEFINE

```c
#define EPD_APP_DEFINE(app_id)
```

**功能**：导出应用信息结构体 `epd_app_info_t { abi, id }`，与 `app_main(intent_t)` 构成主模块入口。

**参数**：
- `app_id`：应用唯一标识符字符串

**示例**：
```c
EPD_APP_DEFINE("com.example.myapp");

int app_main(intent_t *intent) {
    // 应用入口逻辑
    return 0;
}
```

**相关文档**：
- [应用生命周期](../development/application-lifecycle.md)
- [第一个应用](../development/first-application.md)

---

### 控件创建

#### epd_app_object_init

```c
void epd_app_object_init(lv_obj_t *obj)
```

**功能**：为应用对象初始化平台基础样式。

**参数**：
- `obj`：LVGL 对象指针

**所有权**：对象仍由页面拥有

**示例**：
```c
lv_obj_t *panel = lv_obj_create(parent);
epd_app_object_init(panel);
```

---

#### epd_app_label

```c
lv_obj_t *epd_app_label(lv_obj_t *parent, const char *text, 
                        int x, int y, int width, const lv_font_t *font)
```

**功能**：创建普通文字标签。

**参数**：
- `parent`：父对象
- `text`：显示文本
- `x`, `y`：位置坐标
- `width`：标签宽度
- `font`：字体指针

**返回值**：创建的标签对象，归属于 parent

**示例**：
```c
const lv_font_t *font = epd_app_font();
lv_obj_t *label = epd_app_label(screen, "Hello World", 10, 20, 200, font);
```

---

#### epd_app_centered_label

```c
lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text,
                                 int x, int y, int width, const lv_font_t *font)
```

**功能**：创建居中文字标签。

**参数**：同 `epd_app_label`

**返回值**：创建的居中标签对象

**示例**：
```c
lv_obj_t *title = epd_app_centered_label(screen, "标题", 0, 50, 684, epd_app_font());
```

---

#### epd_app_panel

```c
lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int width, int height)
```

**功能**：创建页面面板容器。

**参数**：
- `parent`：父对象
- `x`, `y`：位置坐标
- `width`, `height`：面板尺寸

**返回值**：创建的面板对象

**示例**：
```c
lv_obj_t *panel = epd_app_panel(screen, 10, 100, 664, 400);
```

---

#### epd_app_icon

```c
lv_obj_t *epd_app_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y)
```

**功能**：使用图像描述符创建图标。

**参数**：
- `parent`：父对象
- `src`：图像描述符指针
- `x`, `y`：位置坐标

**返回值**：创建的图标对象

**重要**：描述符及像素数据需在使用期间保持有效

**示例**：
```c
extern const lv_image_dsc_t icon_wifi;
lv_obj_t *icon = epd_app_icon(panel, &icon_wifi, 20, 30);
```

---

#### epd_app_button

```c
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data)
```

**功能**：创建按钮并自动加入最近的受管输入组。

**参数**：
- `parent`：父对象
- `x`, `y`：位置坐标
- `width`, `height`：按钮尺寸
- `text`：按钮文本
- `callback`：事件回调函数
- `data`：回调用户数据（在回调期间保持有效）

**返回值**：创建的按钮对象

**示例**：
```c
static void on_button_clicked(lv_event_t *e) {
    int *counter = lv_event_get_user_data(e);
    (*counter)++;
}

static int counter = 0;
lv_obj_t *btn = epd_app_button(screen, 100, 200, 200, 60, 
                               "点击", on_button_clicked, &counter);
```

---

#### epd_app_back_button

```c
lv_obj_t *epd_app_back_button(lv_obj_t *screen)
```

**功能**：创建标准返回按钮。

**参数**：
- `screen`：所属屏幕对象

**返回值**：返回按钮对象

**示例**：
```c
lv_obj_t *back = epd_app_back_button(screen);
```

---

#### epd_app_back_button_cb

```c
lv_obj_t *epd_app_back_button_cb(lv_obj_t *parent, lv_event_cb_t callback, void *data)
```

**功能**：创建带自定义回调的返回按钮。

**参数**：
- `parent`：父对象
- `callback`：自定义回调函数
- `data`：回调用户数据

**返回值**：返回按钮对象

---

#### epd_app_back_button_layout

```c
void epd_app_back_button_layout(lv_obj_t *button, int32_t x, int32_t center_y)
```

**功能**：按标题中心纵向对齐返回按钮。

**参数**：
- `button`：返回按钮对象
- `x`：横向位置
- `center_y`：标题中心纵坐标

---

### 文本更新

#### epd_app_text

```c
bool epd_app_text(lv_obj_t *label, const char *text)
```

**功能**：复制文本并检测变化更新标签。同一标签后续文字更新都应使用此函数。

**参数**：
- `label`：标签对象
- `text`：新文本内容

**返回值**：
- `true`：更新成功
- `false`：内存分配失败，业务保留待更新状态重试

**示例**：
```c
if (!epd_app_text(label, "新文本")) {
    // 保留待更新标志，稍后重试
    need_update = true;
}
```

---

#### epd_app_status_text

```c
bool epd_app_status_text(lv_obj_t *label, const char *text)
```

**功能**：更新状态标签文本（使用局部刷新预算）。

**参数**：同 `epd_app_text`

**返回值**：是否成功

---

### 页面声明

#### epd_app_header

```c
void epd_app_header(lv_obj_t *screen)
```

**功能**：声明普通页面并使用缩放的系统页眉。

**参数**：
- `screen`：屏幕对象

**生命周期**：页面声明在 screen 的 DELETE 事件释放

**示例**：
```c
lv_obj_t *screen = lv_obj_create(NULL);
epd_app_header(screen);
```

---

#### epd_app_header_refresh

```c
void epd_app_header_refresh(lv_obj_t *screen)
```

**功能**：刷新平台页眉（电量、时间等）。

**参数**：
- `screen`：屏幕对象

---

#### epd_app_set_immersive

```c
void epd_app_set_immersive(lv_obj_t *screen, bool immersive)
```

**功能**：声明沉浸页面，沉浸页面可在页眉下方绘制。

**参数**：
- `screen`：屏幕对象
- `immersive`：是否沉浸模式

**说明**：普通页面声明 header；沉浸页面绘制在其下。顶部边缘下拉打开通知（左）或控制（右）。

**生命周期**：声明随 screen 删除

---

### 平台服务

#### epd_app_status

```c
typedef struct {
    int battery_percent;
    char clock[16];
} epd_app_status_t;

void epd_app_status(epd_app_status_t *status)
```

**功能**：获取电量和时钟信息。

**参数**：
- `status`：输出参数，写入 `epd_app_status_t` 结构

**示例**：
```c
epd_app_status_t status;
epd_app_status(&status);
printf("电量: %d%%, 时间: %s\n", status.battery_percent, status.clock);
```

---

#### epd_app_set_recent_reading

```c
void epd_app_set_recent_reading(const char *title, int percent)
```

**功能**：发布最近阅读信息供系统展示。

**参数**：
- `title`：书籍/文章标题
- `percent`：阅读进度百分比（0-100）

**示例**：
```c
epd_app_set_recent_reading("三体", 65);
```

---

#### epd_app_open_text_settings

```c
bool epd_app_open_text_settings(void)
```

**功能**：请求打开阅读文字设置页面。

**返回值**：
- `true`：请求已受理
- `false`：请求未受理

---

#### epd_app_request_full_refresh

```c
void epd_app_request_full_refresh(void)
```

**功能**：请求一次全刷新（EPD 完整刷新，清除残影）。

---

#### epd_app_refresh_submission

```c
uint32_t epd_app_refresh_submission(void)
```

**功能**：查询提交序号，用于资源交接时判断刷新状态。

**返回值**：当前提交序号

---

#### epd_app_refresh_done

```c
bool epd_app_refresh_done(void)
```

**功能**：查询显示是否完成。

**返回值**：
- `true`：显示已完成
- `false`：显示进行中

**示例**：
```c
uint32_t submission = epd_app_refresh_submission();
// ... 执行操作 ...
while (!epd_app_refresh_done()) {
    rt_thread_mdelay(10);
}
```

---

#### epd_app_ui_available

```c
size_t epd_app_ui_available(void)
```

**功能**：查询 LVGL 私有堆可用量快照。

**返回值**：
- 可用字节数
- `0` 表示不可取得

**说明**：这是快照，不是预留或最大空闲块保证

---

#### epd_app_clean_draw_buffer

```c
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer)
```

**功能**：按平台绘制路径清理缓冲缓存状态。

**参数**：
- `buffer`：绘制缓冲区指针

---

**相关文档**：
- [界面与输入](../development/touch-ui.md)
- [应用生命周期](../development/application-lifecycle.md)

---

## 输入与尺度

### 输入组管理

#### epd_app_input_group

```c
lv_group_t *epd_app_input_group(lv_obj_t *owner)
```

**功能**：为 screen 或弹窗根创建输入范围。

**参数**：
- `owner`：所有者对象（screen 或模态根）

**返回值**：创建的输入组

**生命周期**：删除 owner 自动释放组，调用者不应手动删除

**说明**：物理按键监听器选择最上层可见范围，全局覆盖层优先于活动屏幕

**示例**：
```c
lv_group_t *group = epd_app_input_group(screen);
lv_group_add_obj(group, button);
```

---

#### epd_app_input_listen

```c
typedef enum { EPD_APP_KEY2 = 2, EPD_APP_KEY3 = 3 } epd_app_key_t;
typedef enum {
    EPD_APP_KEY_PRESSED,
    EPD_APP_KEY_RELEASED,
    EPD_APP_KEY_CLICKED,
    EPD_APP_KEY_LONG_PRESSED
} epd_app_key_event_t;
typedef void (*epd_app_key_callback_t)(epd_app_key_t key, 
                                       epd_app_key_event_t event, 
                                       void *data);

bool epd_app_input_listen(lv_obj_t *owner, epd_app_key_callback_t callback, void *data)
```

**功能**：注册实体键监听回调。

**参数**：
- `owner`：所有者对象
- `callback`：按键回调函数
- `data`：用户数据

**返回值**：
- `true`：注册成功
- `false`：注册失败

**说明**：
- KEY1 由系统管理，应用无权使用
- 长按在 3 秒时触发 `EPD_APP_KEY_LONG_PRESSED`
- 释放触发 `EPD_APP_KEY_RELEASED`，短按后附加 `EPD_APP_KEY_CLICKED`
- 只分发给最上层可见范围

**生命周期**：删除 owner 或关闭注册上下文自动注销

**示例**：
```c
static void key_handler(epd_app_key_t key, epd_app_key_event_t event, void *data) {
    if (key == EPD_APP_KEY2 && event == EPD_APP_KEY_CLICKED) {
        printf("KEY2 短按\n");
    }
}

epd_app_input_listen(screen, key_handler, NULL);
```

---

#### epd_app_input_unlisten

```c
void epd_app_input_unlisten(lv_obj_t *owner)
```

**功能**：移除按键监听。

**参数**：
- `owner`：所有者对象

---

#### epd_app_input_back

```c
void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button)
```

**功能**：将返回动作交给按钮处理。

**参数**：
- `owner`：所有者对象
- `button`：返回按钮

**说明**：隐藏或禁用按钮会阻止该范围的返回动作

---

### UI 尺度

**头文件**：`platform/epd_ui_scale.h`（源码：`src/ui/platform/epd_ui_scale.h`）

#### epd_ui_scale_percent

```c
uint32_t epd_ui_scale_percent(void)
```

**功能**：获取当前缩放百分比。

**返回值**：缩放百分比值

---

#### epd_ui_scale_revision

```c
uint32_t epd_ui_scale_revision(void)
```

**功能**：获取缩放变化版本号。

**返回值**：版本号（变化时递增）

---

#### epd_ui_px

```c
int32_t epd_ui_px(int32_t base)
```

**功能**：将设计尺寸转换为实际像素。

**参数**：
- `base`：设计尺寸

**返回值**：缩放后的像素值

**示例**：
```c
int width = epd_ui_px(200);  // 设计宽度 200，根据缩放比例计算实际像素
```

---

#### epd_ui_touch

```c
int32_t epd_ui_touch(int32_t base)
```

**功能**：将设计尺寸转换为触摸尺寸。

**参数**：
- `base`：设计尺寸

**返回值**：触摸尺寸

---

#### epd_ui_content_area

```c
void epd_ui_content_area(lv_obj_t *screen, lv_area_t *out)
```

**功能**：将当前页面可用区域写入 `lv_area_t`。

**参数**：
- `screen`：屏幕对象
- `out`：输出区域结构

**示例**：
```c
lv_area_t area;
epd_ui_content_area(screen, &area);
int content_height = lv_area_get_height(&area);
```

---

#### epd_ui_font_at

```c
const lv_font_t *epd_ui_font_at(unsigned index, epd_font_role_t role)
```

**功能**：预览五档设置之一的角色字体。

**参数**：
- `index`：设置档位（0-4）
- `role`：字体角色

**返回值**：
- 字体指针（平台拥有）
- `NULL` 表示失败

---

### 平台键盘

**头文件**：`platform/epd_keyboard.h`（源码：`src/ui/platform/epd_keyboard.h`）

#### epd_keyboard_create

```c
typedef enum {
    EPD_KEYBOARD_TEXT,
    EPD_KEYBOARD_ASCII,
    EPD_KEYBOARD_NUMBER,
    EPD_KEYBOARD_PASSWORD
} epd_keyboard_mode_t;

typedef struct {
    epd_keyboard_mode_t mode;
    bool chinese;
    bool voice;
} epd_keyboard_config_t;

lv_obj_t *epd_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea,
                              const epd_keyboard_config_t *config)
```

**功能**：创建平台键盘复合控件。

**参数**：
- `parent`：父对象
- `textarea`：目标文本框（由应用拥有）
- `config`：键盘配置（`NULL` 选择带中文和语音的文本输入）

**返回值**：键盘复合控件（拥有键盘和输入法）

**说明**：
- 密码模式关闭中文和语音
- 删除 textarea 自动解绑
- 删除键盘控件释放所有资源
- READY/CANCEL 事件注册在返回的控件上

**示例**：
```c
lv_obj_t *textarea = lv_textarea_create(screen);
epd_keyboard_config_t config = {
    .mode = EPD_KEYBOARD_TEXT,
    .chinese = true,
    .voice = true
};
lv_obj_t *keyboard = epd_keyboard_create(screen, textarea, &config);

// 注册事件
lv_obj_add_event_cb(keyboard, on_keyboard_ready, LV_EVENT_READY, NULL);
lv_obj_add_event_cb(keyboard, on_keyboard_cancel, LV_EVENT_CANCEL, NULL);
```

---

#### epd_keyboard_set_textarea

```c
void epd_keyboard_set_textarea(lv_obj_t *keyboard, lv_obj_t *textarea)
```

**功能**：更换或查询目标文本框。

**参数**：
- `keyboard`：键盘控件
- `textarea`：新文本框

**说明**：重新绑定丢弃未提交拼音

---

#### epd_keyboard_get_textarea

```c
lv_obj_t *epd_keyboard_get_textarea(lv_obj_t *keyboard)
```

**功能**：获取当前绑定的文本框。

**参数**：
- `keyboard`：键盘控件

**返回值**：绑定的文本框对象

---

#### epd_keyboard_set_mode

```c
void epd_keyboard_set_mode(lv_obj_t *keyboard, const epd_keyboard_config_t *config)
```

**功能**：修改键盘模式。

**参数**：
- `keyboard`：键盘控件
- `config`：新配置

**说明**：改模式丢弃未提交拼音

---

#### epd_keyboard_refresh

```c
void epd_keyboard_refresh(lv_obj_t *keyboard)
```

**功能**：更新缩放字体和布局（在平台缩放变化后调用）。

---

#### epd_keyboard_preferred_height

```c
int32_t epd_keyboard_preferred_height(lv_obj_t *keyboard)
```

**功能**：返回键盘期望高度。

**返回值**：期望高度像素值

---

**相关文档**：
- [平台键盘](../development/touch-ui.md#输入键盘)

---

## 字体与图像

**头文件**：`platform/epd_app_font.h`（源码：`src/ui/platform/epd_app_font.h`）

### 平台字体

#### epd_app_font

```c
const lv_font_t *epd_app_font(void)
```

**功能**：获取默认平台界面字体。

**返回值**：平台拥有的字体指针

---

#### epd_app_font_role

```c
const lv_font_t *epd_app_font_role(epd_font_role_t role)
```

**功能**：获取指定角色的平台字体。

**参数**：
- `role`：字体角色枚举

**返回值**：平台拥有的字体指针

---

#### epd_app_font_style

```c
#define EPD_FONT_STYLE_BOLD 1u
#define EPD_FONT_STYLE_ITALIC 2u

const lv_font_t *epd_app_font_style(epd_font_role_t role, unsigned flags)
```

**功能**：获取合成样式的 MiSans 字体。

**参数**：
- `role`：字体角色
- `flags`：样式标志（可用 `|` 组合 BOLD 和 ITALIC）

**返回值**：
- 合成字体指针（平台拥有，生命周期与系统字体一致）
- 未知标志或分配失败返回普通角色字体

**说明**：
- `flags = 0` 返回普通角色字体
- Emoji 保持不变
- 字形像素分配失败时，绘制跳过该字形但不缓存失败，布局保持稳定，后续绘制可重试

**示例**：
```c
// 粗体
const lv_font_t *bold = epd_app_font_style(ROLE_BODY, EPD_FONT_STYLE_BOLD);
// 粗斜体
const lv_font_t *bold_italic = epd_app_font_style(ROLE_BODY, 
    EPD_FONT_STYLE_BOLD | EPD_FONT_STYLE_ITALIC);
```

---

### 应用自定义字体

#### epd_app_font_create

```c
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback)
```

**功能**：UI 线程同步创建内置 MiSans 界面字体。

**参数**：
- `family`：必须为 `0`
- `size`：字号（像素）
- `weight`：字重
- `identity`：输出指针，必填，接收字体标识
- `fallback`：输出指针，必填，返回 `NULL`

**返回值**：
- 创建的字体（由应用拥有，立即可用）
- `NULL` 表示失败

**所有权**：返回字体由应用拥有，使用完毕后调用 `epd_app_font_destroy` 销毁

**示例**：
```c
uint32_t identity;
lv_font_t *fallback;
lv_font_t *font = epd_app_font_create(0, 24, 400, &identity, &fallback);
if (font) {
    lv_obj_set_style_text_font(label, font, 0);
}
// ... 使用完毕后 ...
epd_app_font_destroy(font);
```

---

### 阅读字体

#### epd_app_reading_font_begin

```c
bool epd_app_reading_font_begin(void)
```

**功能**：新阅读会话开始时恢复可用字体选择。

**返回值**：是否成功

**调用时机**：
- UI 线程
- 打开阅读会话时调用一次
- **不要**在恢复或重排会话时调用
- boot-latched 时保持当前字体，不恢复待定偏好

---

#### epd_app_reading_font_id

```c
uint32_t epd_app_reading_font_id(void)
```

**功能**：获取当前实际字体目录稳定 ID。

**返回值**：字体 ID（`0` 是当前 Flash 字体）

---

#### epd_app_reading_font_revision

```c
uint32_t epd_app_reading_font_revision(void)
```

**功能**：获取当前字体内容版本。

**返回值**：内容版本号

**说明**：重用阅读布局前需同时比较 ID 和 revision

**示例**：
```c
static uint32_t last_font_id = 0;
static uint32_t last_revision = 0;

uint32_t current_id = epd_app_reading_font_id();
uint32_t current_revision = epd_app_reading_font_revision();

if (current_id != last_font_id || current_revision != last_revision) {
    // 字体变化，需要重新布局
    need_reflow = true;
    last_font_id = current_id;
    last_revision = current_revision;
}
```

---

#### epd_app_reading_font_create

```c
lv_font_t *epd_app_reading_font_create(unsigned family, unsigned size, unsigned weight,
                                       uint32_t *identity, lv_font_t **fallback)
```

**功能**：UI 线程创建异步阅读字体。

**参数**：
- `family`：字体目录稳定 ID
- `size`：字号（像素）
- `weight`：字重
- `identity`：输出指针，必填
- `fallback`：输出指针，必填，返回 `NULL`

**返回值**：
- 字体外壳（由应用拥有）
- `NULL` 表示分配/标识失败

**重要**：
- 返回值非 `NULL` 不表示字形已准备好
- 等待 `status==1` 并准备所需字形后才绘制
- 只有确认缺失的字形使用远程 MiSans

**示例**：
```c
uint32_t identity;
lv_font_t *fallback;
lv_font_t *font = epd_app_reading_font_create(font_id, 20, 400, &identity, &fallback);
if (font) {
    // 等待字体准备
    while (epd_app_font_status(font) == 0) {
        rt_thread_mdelay(10);
    }
    if (epd_app_font_status(font) == 1) {
        // 字体就绪，可以使用
    }
}
```

---

#### epd_app_font_destroy

```c
void epd_app_font_destroy(lv_font_t *font)
```

**功能**：销毁自有字体。

**参数**：
- `font`：要销毁的字体

**前提**：去除控件与绘制引用后才能调用

---

#### epd_app_font_status

```c
int epd_app_font_status(const lv_font_t *font)
```

**功能**：查询字体状态（UI 线程发布度量）。

**参数**：
- `font`：字体指针

**返回值**：
- `-1`：终态源/字形错误
- `EPD_APP_FONT_NO_MEMORY (-3)`：准备耗尽边界恢复
- `0`：排队中/进行中
- `1`：就绪

**说明**：
- 只有 status 是 UI 线程专用，度量发布恰好一次
- Worker 完成不会修改已发布的 `lv_font_t`
- 缓存的度量和字形在错误后仍可用

---

#### epd_app_font_metrics

```c
typedef struct { 
    uint32_t line_height; 
    int32_t base_line; 
} epd_app_font_metrics_t;

int epd_app_font_metrics(const lv_font_t *font, epd_app_font_metrics_t *out)
```

**功能**：获取线程安全的字体度量快照。

**参数**：
- `font`：字体指针
- `out`：输出度量结构

**返回值**：同 `epd_app_font_status`

**线程安全**：是

---

#### epd_app_font_glyph_ready

```c
int epd_app_font_glyph_ready(const lv_font_t *font, uint32_t letter, 
                             uint32_t next, bool pixels)
```

**功能**：检查字形就绪状态。

**参数**：
- `font`：字体指针
- `letter`：字符码点
- `next`：下一个字符（用于 kerning）
- `pixels`：是否需要像素数据

**返回值**：同 `epd_app_font_status`

---

#### epd_app_font_prepare

```c
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent)
```

**功能**：请求准备字形（异步）。

**参数**：
- `font`：字体指针
- `letter`：字符码点
- `adjacent`：是否为相邻字符

**返回值**：
- `true`：已受理（不保证立即就绪）
- `false`：推测缓存预算已满

**说明**：
- 异步阅读字体返回 `true` 也可能仍在等待
- 应使用 `glyph_ready` 检查最终状态
- UI 线程空闲准备

---

#### epd_app_font_glyph_acquire

```c
typedef struct {
    uint32_t adv_w, box_w, box_h, stride;
    int32_t ofs_x, ofs_y;
    const uint8_t *pixels;
    void *token;
} epd_app_font_glyph_t;

int epd_app_font_glyph_acquire(const lv_font_t *font, uint32_t letter, uint32_t next,
                              bool pixels, epd_app_font_glyph_t *out)
```

**功能**：获取字形数据的拥有引用。

**参数**：
- `font`：字体指针
- `letter`：字符码点
- `next`：下一个字符
- `pixels`：是否需要像素数据
- `out`：输出字形结构

**返回值**：同 `epd_app_font_status`

**说明**：
- 尺寸/advance/offsets 单位是像素
- A8 行跨度是 `stride` 字节
- `pixels` 为 `NULL` 表示仅描述符或空字形
- 成功时排队缺失数据并返回一个拥有的 token
- 每次成功都配对调用 `epd_app_font_glyph_release`
- 像素借用期到 release 为止
- Token 在取消、页面释放和销毁后仍保留返回的内存

**线程安全**：是

**示例**：
```c
epd_app_font_glyph_t glyph;
int result = epd_app_font_glyph_acquire(font, 'A', 'B', true, &glyph);
if (result == 1) {
    // 使用 glyph.pixels, glyph.box_w 等
    // ...
    epd_app_font_glyph_release(&glyph);
}
```

---

#### epd_app_font_glyph_release

```c
void epd_app_font_glyph_release(epd_app_font_glyph_t *glyph)
```

**功能**：释放字形引用。

**参数**：
- `glyph`：字形结构指针

**说明**：与 `acquire` 成功配对调用

---

### 图像

**头文件**：`platform/app_image.h`（源码：`src/ui/platform/app_image.h`）

#### epd_app_image_set

```c
bool epd_app_image_set(lv_obj_t *image, const char *path, const char *fallback)
```

**功能**：UI 线程为控件保留 EZIP 图像资源。

**参数**：
- `image`：图像对象
- `path`：EZIP 路径
- `fallback`：失败时的后备路径

**返回值**：是否成功

**生命周期**：删除控件自动释放

---

#### epd_app_image_release

```c
void epd_app_image_release(lv_obj_t *image)
```

**功能**：释放图像控件的 EZIP 资源。

**参数**：
- `image`：图像对象

---

**相关文档**：
- [图像与字体](../development/application-resources.md)

---

## 后台任务和摘要

**头文件**：`services/app_service.h`（源码：`src/services/app_service.h`）

### 后台服务定义

#### epd_background_t

```c
typedef struct {
    void (*run)(epd_service_t *service);
    uint32_t stack_size;
    uint8_t priority;
    bool network;
    int (*command)(int argc, char **argv);
    bool (*ui_process)(epd_service_t *service, bool stopping);
    bool (*flush)(void);
} epd_background_t;
```

**成员说明**：

| 成员 | 执行位置与契约 |
|------|--------------|
| `run` | 受管 worker，收到停止后关闭请求和句柄并返回 |
| `stack_size` | 任务栈大小（字节） |
| `priority` | 任务优先级 |
| `network` | 是否接收网络变化唤醒 |
| `command` | 服务命令入口；模块在命令返回前保持有效 |
| `ui_process` | 有界 UI 步进；worker 和 command 返回后 stopping 才为 true，释放完 UI 资源后返回 true |
| `flush` | UI 线程保存屏障；未保存或失败返回 false，保存持久化完成后返回 true |

**示例**：
```c
static void service_run(epd_service_t *service) {
    while (!epd_service_cancelled(service)) {
        uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
        if (events & EPD_SERVICE_STOP) break;
        // 处理任务
    }
}

static bool service_flush(void) {
    // 保存状态
    return true;
}

static const epd_background_t my_service = {
    .run = service_run,
    .stack_size = 4096,
    .priority = 20,
    .network = true,
    .command = NULL,
    .ui_process = NULL,
    .flush = service_flush
};
```

---

### 服务控制

#### app_service_start

```c
epd_service_t *app_service_start(const char *id, const epd_background_t *definition, 
                                 void *module)
```

**功能**：UI 线程启动服务。

**参数**：
- `id`：服务唯一标识（最大 64 字节含 NUL）
- `definition`：服务定义
- `module`：SDK 模块句柄（动态应用传入自身模块句柄，固件服务传 `NULL`）

**返回值**：
- 服务句柄
- `NULL` 表示失败

**示例**：
```c
epd_service_t *service = app_service_start("com.example.myservice", 
                                          &my_service, 
                                          rt_module_self());
```

---

#### app_service_stop

```c
void app_service_stop(const char *id)
```

**功能**：UI 线程请求取消服务。

**参数**：
- `id`：服务标识

**说明**：停止请求不等于已清理完成，使用 `app_service_running` 查询状态

---

#### app_service_running

```c
bool app_service_running(const char *id)
```

**功能**：UI 线程查询服务是否仍存活。

**参数**：
- `id`：服务标识

**返回值**：
- `true`：服务运行中
- `false`：服务已停止

---

### Worker 接口

#### epd_service_wait

```c
#define EPD_SERVICE_WAKE 1u
#define EPD_SERVICE_NETWORK 2u
#define EPD_SERVICE_STOP 4u

uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks)
```

**功能**：等待服务事件。

**参数**：
- `service`：服务句柄
- `ticks`：超时时间（`RT_WAITING_FOREVER` 表示永久等待）

**返回值**：事件位掩码（WAKE、NETWORK、STOP）

**示例**：
```c
uint32_t events = epd_service_wait(service, RT_WAITING_FOREVER);
if (events & EPD_SERVICE_STOP) {
    // 收到停止信号
    break;
}
if (events & EPD_SERVICE_NETWORK) {
    // 网络状态变化
}
```

---

#### epd_service_cancelled

```c
bool epd_service_cancelled(epd_service_t *service)
```

**功能**：查询停止状态。

**参数**：
- `service`：服务句柄

**返回值**：
- `true`：已请求停止
- `false`：正常运行

**说明**：接受新任务前检查

---

#### epd_service_wake

```c
void epd_service_wake(epd_service_t *service)
```

**功能**：唤醒任务。

**参数**：
- `service`：服务句柄

---

#### epd_service_sleep_acquire

```c
bool epd_service_sleep_acquire(epd_service_t *service)
```

**功能**：每个服务幂等的休眠锁。

**参数**：
- `service`：服务句柄

**返回值**：
- `true`：获取成功
- `false`：申请失败，不接受新任务

**说明**：所有工作及清理结束后调用 `epd_service_sleep_release`

---

#### epd_service_sleep_release

```c
void epd_service_sleep_release(epd_service_t *service)
```

**功能**：释放休眠锁。

**参数**：
- `service`：服务句柄

---

#### epd_service_ui_request

```c
void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms)
```

**功能**：请求下一次 UI 步进。

**参数**：
- `service`：服务句柄
- `delay_ms`：延迟毫秒数

**说明**：重复请求保留最早期限

---

### 摘要发布

#### epd_service_publish_summary

```c
void epd_service_publish_summary(epd_service_t *service, const char *text)
```

**功能**：发布文字副本作为服务摘要。

**参数**：
- `service`：服务句柄
- `text`：摘要文本

**调用位置**：Worker 线程

---

#### epd_app_summary_revision

```c
uint32_t epd_app_summary_revision(void)
```

**功能**：读取驻留摘要版本。

**返回值**：版本号（发布/移除时变化）

---

#### epd_app_summary_read

```c
bool epd_app_summary_read(const char *id, char *text, size_t capacity)
```

**功能**：读取摘要副本。

**参数**：
- `id`：服务标识
- `text`：输出缓冲区
- `capacity`：缓冲区容量

**返回值**：
- `true`：读取成功
- `false`：读取失败

---

#### epd_service_is_worker

```c
bool epd_service_is_worker(epd_service_t *service)
```

**功能**：检查调用线程是否为该服务尚未结束的 worker。

**参数**：
- `service`：服务句柄

**返回值**：
- `true`：是 worker 线程
- `false`：不是

**说明**：取消独立于此；接受新工作前检查 `epd_service_cancelled()`

---

**相关文档**：
- [后台服务](../development/application-development.md)
- [应用生命周期](../development/application-lifecycle.md)

---

## 存储与内存

### 存储访问

**头文件**：`services/storage.h`（源码：`src/services/storage.h`）

#### storage_root

```c
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;

const char *storage_root(storage_volume_t volume)
```

**功能**：获取存储卷根路径。

**参数**：
- `volume`：存储卷枚举（FLASH 或 SD）

**返回值**：静态字符串（不需释放）

**示例**：
```c
const char *flash_root = storage_root(STORAGE_FLASH);  // "/flash"
const char *sd_root = storage_root(STORAGE_SD);        // "/sdcard"
```

---

#### storage_path_root

```c
const char *storage_path_root(const char *path)
```

**功能**：从路径获取其根路径。

**参数**：
- `path`：完整路径

**返回值**：借用字符串（不需释放，不分配）

---

#### storage_relative_path

```c
const char *storage_relative_path(const char *path)
```

**功能**：从路径获取相对路径部分。

**参数**：
- `path`：完整路径

**返回值**：借用字符串

---

#### storage_info

```c
typedef struct { 
    bool mounted; 
    uint64_t total, used; 
} storage_info_t;

bool storage_info(storage_volume_t volume, storage_info_t *info)
```

**功能**：复制存储信息。

**参数**：
- `volume`：存储卷
- `info`：输出信息结构

**返回值**：是否成功

**示例**：
```c
storage_info_t info;
if (storage_info(STORAGE_SD, &info)) {
    if (info.mounted) {
        uint64_t free = info.total - info.used;
        printf("SD卡可用: %llu MB\n", free / 1024 / 1024);
    }
}
```

---

#### storage_available

```c
bool storage_available(storage_volume_t volume)
```

**功能**：检查存储卷可访问性。

**参数**：
- `volume`：存储卷

**返回值**：是否可访问

---

#### storage_path_available

```c
bool storage_path_available(const char *path)
```

**功能**：检查路径可访问性。

**参数**：
- `path`：路径字符串

**返回值**：是否可访问

---

#### storage_revision

```c
uint32_t storage_revision(void)
```

**功能**：获取介质变化版本号。

**返回值**：版本号

---

#### storage_changing

```c
bool storage_changing(void)
```

**功能**：检查存储是否正在变化中。

**返回值**：是否变化中

---

#### storage_card_session

```c
uint32_t storage_card_session(void)
```

**功能**：获取 SD 卡会话标识。

**返回值**：会话 ID

**说明**：移除的卡在替换可挂载前使其会话失效

---

#### storage_session_valid

```c
bool storage_session_valid(storage_volume_t volume, uint32_t session)
```

**功能**：检查会话是否仍有效。

**参数**：
- `volume`：存储卷
- `session`：会话 ID

**返回值**：是否有效

---

#### storage_lock / storage_unlock

```c
void storage_lock(void)
void storage_try_lock(void)
void storage_unlock(void)
```

**功能**：线程上下文存储锁，用于序列化原始文件系统访问。

**说明**：同一线程配对使用 lock/unlock

---

### 存储辅助函数

**头文件**：`services/storage_file.h`（源码：`src/services/storage_file.h`）

#### storage_app_path

```c
bool storage_app_path(char *out, size_t size, const char *id, 
                     const char *area, const char *relative)
```

**功能**：将应用路径写入缓冲区。

**参数**：
- `out`：输出缓冲区
- `size`：缓冲区大小
- `id`：应用 ID
- `area`：区域名称
- `relative`：相对路径

**返回值**：
- `true`：成功
- `false`：失败

**说明**：不创建目录

---

#### storage_mkdirs

```c
bool storage_mkdirs(const char *path)
```

**功能**：创建目录（递归）。

**参数**：
- `path`：目录路径

**返回值**：是否成功

---

#### storage_crc32

```c
uint32_t storage_crc32(const void *data, size_t size)
```

**功能**：计算数据的 CRC32 校验值。

**参数**：
- `data`：数据指针
- `size`：数据大小

**返回值**：CRC32 值

---

#### storage_record_load

```c
bool storage_record_load(const char *path, uint32_t magic, uint32_t version,
                        void *data, size_t size)
```

**功能**：按标识、版本、长度及 CRC 读取记录。

**参数**：
- `path`：文件路径
- `magic`：魔数
- `version`：版本号
- `data`：输出缓冲区（调用者拥有）
- `size`：数据大小

**返回值**：是否成功

---

#### storage_record_save

```c
bool storage_record_save(const char *path, uint32_t magic, uint32_t version,
                        const void *data, size_t size)
```

**功能**：按标识、版本、长度及 CRC 写入记录。

**参数**：
- `path`：文件路径
- `magic`：魔数
- `version`：版本号
- `data`：数据缓冲区
- `size`：数据大小

**返回值**：是否成功

---

#### storage_file_replace

```c
bool storage_file_replace(const char *path, const void *data, size_t size)
```

**功能**：同步复制并原子替换完整文件。

**参数**：
- `path`：文件路径
- `data`：文件内容
- `size`：内容大小

**返回值**：是否成功

---

### 应用内存

**头文件**：`platform/epd_memory.h`（源码：`src/platform/epd_memory.h`）

#### epd_app_alloc

```c
void *epd_app_alloc(size_t size)
```

**功能**：从应用内存区域分配。

**参数**：
- `size`：分配大小

**返回值**：
- 内存指针
- `NULL` 表示失败

---

#### epd_app_realloc

```c
void *epd_app_realloc(void *ptr, size_t size)
```

**功能**：重新分配应用内存。

**参数**：
- `ptr`：原内存指针
- `size`：新大小

**返回值**：新内存指针或 `NULL`

---

#### epd_app_free

```c
void epd_app_free(void *ptr)
```

**功能**：释放应用内存。

**参数**：
- `ptr`：内存指针

---

#### epd_app_psram_available

```c
size_t epd_app_psram_available(void)
```

**功能**：查询 PSRAM 可用量。

**返回值**：可用字节数

---

**相关文档**：
- [应用存储](../development/application-storage.md)
- [应用内存](../development/application-memory.md)

---

## 网络服务

**头文件**：`services/net/network.h`（源码：`src/services/net/network.h`）

### 网络状态

#### network_snapshot_t

```c
typedef struct {
    bool ready;
    uint32_t source;
    uint32_t generation;
    uint32_t ipv4_address;  // 网络字节序
} network_snapshot_t;
```

**成员说明**：
- `ready`：链路就绪（不表示公网可达）
- `source`：网络来源
- `generation`：变化代数
- `ipv4_address`：IPv4 地址

---

#### network_snapshot

```c
void network_snapshot(network_snapshot_t *out)
```

**功能**：复制当前上行状态。

**参数**：
- `out`：输出快照结构

**示例**：
```c
network_snapshot_t snap;
network_snapshot(&snap);
if (snap.ready) {
    printf("网络已连接\n");
}
```

---

#### network_ready

```c
bool network_ready(void)
```

**功能**：查询链路就绪状态。

**返回值**：
- `true`：链路就绪
- `false`：链路未就绪

**说明**：ready 不表示公网服务可达

---

#### network_interface_snapshot

```c
rt_err_t network_interface_snapshot(uint32_t source, network_snapshot_t *out)
```

**功能**：读取指定接口快照。

**参数**：
- `source`：接口来源
- `out`：输出快照

**返回值**：
- `RT_EOK`：成功取得副本
- 其他：失败

**说明**：连接情况仍看副本字段

---

### 网络订阅

#### network_subscribe

```c
uint32_t network_subscribe(rt_event_t event, uint32_t bits)
```

**功能**：订阅网络变化事件。

**参数**：
- `event`：事件对象
- `bits`：事件位

**返回值**：订阅标识

**说明**：网络变化向事件发送 bits，网络线程不执行应用回调

---

#### network_unsubscribe

```c
void network_unsubscribe(uint32_t id)
```

**功能**：取消订阅。

**参数**：
- `id`：订阅标识

**说明**：等待在途发送结束，随后才能销毁 event

---

**相关文档**：
- [网络服务](../services/wifi.md)

---

## AI 服务

**头文件**：`platform/epd_ai.h`（源码：`src/platform/epd_ai.h`）

### AI 配置查询

#### epd_ai_profile_list

```c
epd_ai_error_t epd_ai_profile_list(epd_service_t *owner, epd_ai_profile_t *profiles,
                                  size_t capacity, size_t *count)
```

**功能**：使用 owner 读取 AI 配置文件脱敏副本。

**参数**：
- `owner`：服务句柄
- `profiles`：输出配置数组
- `capacity`：数组容量
- `count`：输出实际数量

**返回值**：错误码（`EPD_AI_OK` 表示成功）

---

#### epd_ai_profile_get

```c
typedef struct {
    uint32_t struct_size, version;
    char id[EPD_AI_ID_SIZE], name[EPD_AI_NAME_SIZE];
    epd_ai_protocol_t protocol;
    uint32_t identity_revision, features;
    bool enabled, is_default;
    uint8_t model_count;
    epd_ai_model_t models[EPD_AI_MAX_MODELS];
} epd_ai_profile_t;

epd_ai_error_t epd_ai_profile_get(epd_service_t *owner, const char *id,
                                 epd_ai_profile_t *profile)
```

**功能**：获取指定 AI 配置文件。

**参数**：
- `owner`：服务句柄
- `id`：配置 ID
- `profile`：输出配置结构

**返回值**：错误码

---

### AI 请求

#### epd_ai_perform

```c
typedef struct {
    uint32_t struct_size, version;
    const char *profile_id, *model, *instructions;
    const epd_ai_message_t *messages;
    size_t message_count;
    const epd_ai_tool_t *tools;
    size_t tool_count;
    uint32_t max_output_tokens;
    bool stream;
    epd_ai_format_t format;
    const char *schema_name, *schema_json;
    bool has_temperature, has_top_p;
    double temperature, top_p;
    const char *const *stop;
    size_t stop_count;
    const char *extensions_json;
    uint64_t web_search;
} epd_ai_request_t;

typedef struct {
    uint32_t struct_size, version;
    epd_ai_error_t error;
    int http_status, detail;
    uint32_t tls_verify_flags;
    char error_code[96], request_id[128], retry_after[64], stage[32];
    epd_ai_finish_t finish;
    epd_ai_usage_t usage;
    char *text, *native;
    size_t text_size, native_size, tool_count;
    epd_ai_tool_call_t tools[EPD_AI_MAX_TOOLS];
    bool complete, replayable;
} epd_ai_result_t;

typedef bool (*epd_ai_event_cb_t)(const epd_ai_event_t *event, void *user);

epd_ai_error_t epd_ai_perform(epd_service_t *owner, const epd_ai_request_t *request,
                             epd_ai_cancel_t *cancel, epd_ai_event_cb_t callback,
                             void *user, epd_ai_result_t *result)
```

**功能**：在 owner 的注册 worker 上执行一个 AI 请求。

**参数**：
- `owner`：服务句柄
- `request`：请求结构
- `cancel`：取消令牌
- `callback`：事件回调
- `user`：回调用户数据
- `result`：结果结构

**返回值**：错误码（同步返回终态错误码）

**重要**：
- 调用者保持所有输入和 cancel 在返回前有效
- 失败的 POST 不重试
- 回调返回 `false` 取消
- 不要在回调中执行工具或等待 GUI

**示例**：
```c
epd_ai_request_t req = {
    .struct_size = sizeof(req),
    .version = EPD_AI_VERSION,
    .profile_id = NULL,  // 使用默认
    .model = "gpt-4",
    .instructions = "你是一个助手",
    .messages = messages,
    .message_count = msg_count,
    .stream = true,
    .max_output_tokens = 1000
};

epd_ai_cancel_t cancel;
epd_ai_cancel_init(&cancel);

epd_ai_result_t result = {0};
result.struct_size = sizeof(result);
result.version = EPD_AI_VERSION;

epd_ai_error_t err = epd_ai_perform(service, &req, &cancel, 
                                   event_callback, user_data, &result);
if (err == EPD_AI_OK) {
    // 处理 result
    epd_ai_result_clear(&result);
}
```

---

#### epd_ai_cancel_init / epd_ai_cancel

```c
typedef struct { volatile uint32_t cancelled; } epd_ai_cancel_t;

void epd_ai_cancel_init(epd_ai_cancel_t *cancel)
void epd_ai_cancel(epd_ai_cancel_t *cancel)
```

**功能**：初始化或设置取消令牌。

**参数**：
- `cancel`：取消令牌指针

**说明**：请求结束前保持令牌有效

---

#### epd_ai_result_clear

```c
void epd_ai_result_clear(epd_ai_result_t *result)
```

**功能**：释放成功或失败请求的结果资源。

**参数**：
- `result`：结果结构指针

---

#### epd_ai_select_service

```c
bool epd_ai_select_service(epd_service_t *owner)
```

**功能**：UI 线程打开 AI 服务选择和授权页面。

**参数**：
- `owner`：服务句柄

**返回值**：是否打开成功

---

**相关文档**：
- [AI 服务](../services/ai-services.md)

---

## 语音服务

**头文件**：`platform/epd_speech.h`（源码：`src/platform/epd_speech.h`）

### 语音能力查询

#### epd_speech_capability

```c
typedef struct {
    bool configured, busy, input_available;
    uint32_t max_record_ms;
} epd_speech_capability_t;

typedef enum { EPD_SPEECH_ASR, EPD_SPEECH_TTS } epd_speech_kind_t;

epd_speech_error_t epd_speech_capability(epd_speech_kind_t kind, 
                                        epd_speech_capability_t *out)
```

**功能**：查询 ASR/TTS 能力副本。

**参数**：
- `kind`：语音类型（ASR 语音识别 / TTS 语音合成）
- `out`：输出能力结构

**返回值**：错误码

---

### 语音会话

#### epd_speech_start

```c
typedef struct {
    uint32_t struct_size, version;
    epd_speech_language_t language;
    bool background;
    char title[160];
} epd_speech_request_t;

typedef uint32_t epd_speech_session_t;

epd_speech_error_t epd_speech_start(epd_service_t *owner, epd_speech_kind_t kind,
                                   const epd_speech_request_t *request, 
                                   epd_speech_session_t *session)
```

**功能**：创建异步语音会话。

**参数**：
- `owner`：服务句柄
- `kind`：语音类型
- `request`：请求配置
- `session`：输出会话 ID

**返回值**：错误码

**示例**：
```c
epd_speech_request_t req = {
    .struct_size = sizeof(req),
    .version = EPD_SPEECH_VERSION,
    .language = EPD_SPEECH_ZH_EN,
    .background = false,
    .title = "语音识别"
};

epd_speech_session_t session;
epd_speech_error_t err = epd_speech_start(service, EPD_SPEECH_ASR, &req, &session);
```

---

#### epd_speech_append

```c
int epd_speech_append(epd_service_t *owner, epd_speech_session_t session,
                     const char *text, size_t bytes)
```

**功能**：复制已接受的 UTF-8 前缀（用于 TTS）。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID
- `text`：文本数据
- `bytes`：字节数

**返回值**：
- 正数：已接受的字节数
- `0`：反压（稍后重试）
- 负数：错误码

**说明**：调用者保留/重试后缀。finish 只在所有文本被接受后有效

---

#### epd_speech_finish

```c
epd_speech_error_t epd_speech_finish(epd_service_t *owner, epd_speech_session_t session)
```

**功能**：请求完成会话。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID

**返回值**：错误码

---

#### epd_speech_cancel

```c
epd_speech_error_t epd_speech_cancel(epd_service_t *owner, epd_speech_session_t session)
```

**功能**：取消会话。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID

**返回值**：错误码

---

#### epd_speech_pause / epd_speech_resume

```c
epd_speech_error_t epd_speech_pause(epd_service_t *owner, epd_speech_session_t session)
epd_speech_error_t epd_speech_resume(epd_service_t *owner, epd_speech_session_t session)
```

**功能**：暂停/恢复语音播放。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID

**返回值**：错误码

---

#### epd_speech_stop

```c
epd_speech_error_t epd_speech_stop(epd_service_t *owner, epd_speech_session_t session)
```

**功能**：停止会话。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID

**返回值**：错误码

---

#### epd_speech_snapshot

```c
typedef struct {
    epd_speech_session_t session;
    epd_speech_kind_t kind;
    epd_speech_state_t state;
    epd_speech_error_t error;
    epd_speech_reason_t reason;
    uint32_t revision, elapsed_ms, limit_ms, unit;
    uint64_t submitted_samples;
    bool input_finished;
} epd_speech_snapshot_t;

epd_speech_error_t epd_speech_snapshot(epd_service_t *owner, 
                                      epd_speech_session_t session,
                                      epd_speech_snapshot_t *out)
```

**功能**：复制会话状态。

**参数**：
- `owner`：服务句柄
- `session`：会话 ID
- `out`：输出快照

**返回值**：错误码

---

#### epd_speech_poll

```c
typedef struct {
    epd_speech_event_kind_t kind;
    epd_speech_snapshot_t snapshot;
    uint32_t sentence, revision;
    const char *text;
    size_t text_bytes;
    int detail, http_status;
    char stage[32], request_id[128], code[96];
    void *private_data;
} epd_speech_event_t;

epd_speech_error_t epd_speech_poll(epd_service_t *owner, epd_speech_event_t *out)
```

**功能**：单一消费者读取事件。

**参数**：
- `owner`：服务句柄
- `out`：输出事件

**返回值**：
- `EPD_SPEECH_OK`：成功
- `EPD_SPEECH_NOT_FOUND`：队列为空
- 其他：错误码

**说明**：返回事件拥有其文本直到 release，不要保留超过边界事件预算

---

#### epd_speech_event_release

```c
void epd_speech_event_release(epd_speech_event_t *event)
```

**功能**：释放事件资源。

**参数**：
- `event`：事件指针

**说明**：处理后释放文字和资源

---

**相关文档**：
- [语音服务](../services/speech-services.md)

---

## 通知、媒体和声音

**头文件**：`ui/platform/epd_system.h`（源码：`src/ui/platform/epd_system.h`）

### 通知

#### epd_notification_publish

```c
typedef struct {
    uint32_t event_id;
    char title[128];
    char text[256];
    char page[64];
    char app_id[64];
    bool ongoing;
} epd_notification_t;

bool epd_notification_publish(epd_service_t *service, const epd_notification_t *value)
```

**功能**：发布通知副本。

**参数**：
- `service`：服务句柄
- `value`：通知结构

**返回值**：是否接受

**说明**：
- 任务上下文发布副本
- 字符串在结构数组内必须以零结尾
- 每应用最多一条可见通知
- 被用户删除的同一事件 ID 不重新显示；新业务事件使用新 ID
- app_id 由 owner 指定

---

### 媒体会话

#### epd_media_publish

```c
typedef struct {
    char artist[128];
    char title[128];
    uint32_t audio_type;
    uint32_t capabilities;
    bool playing;
} epd_media_t;

bool epd_media_publish(epd_service_t *service, const epd_media_t *media)
```

**功能**：发布媒体状态。

**参数**：
- `service`：服务句柄
- `media`：媒体状态（传 `NULL` 撤回）

**返回值**：是否接受

---

### 声音

**头文件**：`ui/platform/epd_sound.h`（源码：`src/ui/platform/epd_sound.h`）

#### epd_sound_read

```c
typedef struct {
    uint8_t volume_percent;
    bool muted;
} epd_sound_state_t;

void epd_sound_read(epd_sound_state_t *out)
```

**功能**：复制内存状态（不读取硬件）。

**参数**：
- `out`：输出状态结构

---

#### epd_sound_set_volume

```c
bool epd_sound_set_volume(uint32_t channel, uint8_t percent)
```

**功能**：设置音量并保存偏好。

**参数**：
- `channel`：音频通道
- `percent`：音量百分比（0-100）

**返回值**：
- `true`：成功
- `false`：失败（恢复原状态）

---

#### epd_sound_set_muted

```c
bool epd_sound_set_muted(bool muted)
```

**功能**：设置静音状态并保存偏好。

**参数**：
- `muted`：是否静音

**返回值**：是否成功

---

#### epd_sound_notify

```c
void epd_sound_notify(uint32_t category)
```

**功能**：向常驻声音 worker 请求提示音。

**参数**：
- `category`：提示音分类

**说明**：不在调用线程播放

---

**相关文档**：
- [媒体会话](../development/touch-ui.md#媒体会话)

---

## 内容处理

**头文件**：`ui/platform/app_content.h`（源码：`src/ui/platform/app_content.h`）

### 内容查询与打开

#### app_content_query

```c
int app_content_query(const char *path, uint32_t first, 
                     void *items, size_t capacity, size_t *count)
```

**功能**：UI 线程枚举处理者副本（不加载模块）。

**参数**：
- `path`：文件路径
- `first`：起始索引
- `items`：输出数组
- `capacity`：数组容量
- `count`：输出实际数量

**返回值**：错误码

---

#### app_content_open

```c
rt_err_t app_content_open(const char *id, const char *path)
```

**功能**：UI 线程排队打开内容。

**参数**：
- `id`：处理者 ID
- `path`：文件路径

**返回值**：
- `RT_EOK`：Intent 已入队
- 其他：错误码

---

#### app_content_receive

```c
int app_content_receive(intent_t *intent, char *path, size_t size)
```

**功能**：复制入站路径。

**参数**：
- `intent`：Intent 对象
- `path`：输出缓冲区
- `size`：缓冲区大小

**返回值**：
- `0` 或正数：成功
- `-ENOENT`：普通启动
- 其他负数：介质变化或空间不足

---

### 私有库

**头文件**：`ui/platform/epd_app_library.h`（源码：`src/ui/platform/epd_app_library.h`）

#### epd_app_library_open

```c
void *epd_app_library_open(const char *app_id, const char *name)
```

**功能**：UI 线程打开所属应用的私有库。

**参数**：
- `app_id`：应用 ID
- `name`：库名称

**返回值**：
- 库句柄
- `NULL` 表示失败

---

#### epd_app_library_symbol

```c
void *epd_app_library_symbol(void *library, const char *symbol)
```

**功能**：查找库接口。

**参数**：
- `library`：库句柄
- `symbol`：符号名称

**返回值**：
- 符号地址
- `NULL` 表示未找到

---

#### epd_app_library_close

```c
void epd_app_library_close(void *library)
```

**功能**：在 UI 线程释放库。

**参数**：
- `library`：库句柄

**说明**：worker 及库结果全部结束后才能调用

---

**相关文档**：
- [文件内容交接](../development/content-handlers.md)

---

## 时间与语言

### 系统时间

**头文件**：`services/system_time.h`（源码：`src/services/system_time.h`）

#### system_time_settings_t

```c
typedef struct {
    int16_t timezone_minutes;  // -720 ~ 840
    bool automatic;            // 自动校时
    bool hour24;              // 24 小时显示
} system_time_settings_t;
```

---

#### system_time_get_settings

```c
void system_time_get_settings(system_time_settings_t *out)
```

**功能**：复制当前显示设置。

**参数**：
- `out`：输出设置结构

---

#### system_time_get_utc

```c
int64_t system_time_get_utc(void)
```

**功能**：取得 UTC 秒数。

**返回值**：UTC 时间戳（秒）

---

#### system_time_valid

```c
bool system_time_valid(void)
```

**功能**：判断当前系统时间是否有效。

**返回值**：是否有效

---

#### system_time_local_from_utc

```c
bool system_time_local_from_utc(int64_t utc, struct tm *out)
```

**功能**：将本地日历写入 `struct tm`。

**参数**：
- `utc`：UTC 时间戳
- `out`：输出 tm 结构

**返回值**：
- `true`：成功
- `false`：失败

---

#### system_time_format

```c
bool system_time_format(int64_t utc, const char *locale, bool with_date,
                       char *out, size_t size)
```

**功能**：按语言及 12/24 小时设置格式化时间。

**参数**：
- `utc`：UTC 时间戳
- `locale`：语言代码
- `with_date`：是否包含日期
- `out`：输出缓冲区
- `size`：缓冲区大小

**返回值**：
- `true`：成功
- `false`：无效时间（写占位符）或缓冲不足（不截断 UTF-8）

---

### 语言设置

**头文件**：`ui/platform/epd_locale.h`（源码：`src/ui/platform/epd_locale.h`）

#### epd_locale_get

```c
const char *epd_locale_get(void)
```

**功能**：UI 线程获取当前语言代码。

**返回值**：语言代码字符串（固件持有，生命周期与固件一致）

---

#### epd_locale_revision

```c
uint32_t epd_locale_revision(void)
```

**功能**：获取语言版本号。

**返回值**：版本号

**说明**：应用依据版本更新自己的文字和布局

---

### UI 设置

**头文件**：`ui/ui_settings.h`（源码：`src/ui/ui_settings.h`）

#### ui_settings_value

```c
const char *ui_settings_value(ui_setting_id_t id)
```

**功能**：UI 线程查询设置格式化文字。

**参数**：
- `id`：设置项 ID

**返回值**：格式化文字（借用期到下一次设置文字查询）

**说明**：需要长期保留时复制

---

#### ui_settings_enabled

```c
bool ui_settings_enabled(ui_setting_id_t id)
```

**功能**：查询该设置项是否可操作。

**参数**：
- `id`：设置项 ID

**返回值**：是否可操作

---

#### ui_settings_cycle

```c
bool ui_settings_cycle(ui_setting_id_t id)
```

**功能**：经系统设置服务切换下一项。

**参数**：
- `id`：设置项 ID

**返回值**：是否成功

---

#### ui_settings_index

```c
int32_t ui_settings_index(ui_setting_id_t id)
```

**功能**：获取设置选项下标。

**参数**：
- `id`：设置项 ID

**返回值**：
- 通常返回选项下标
- FONT 返回实际字体稳定 ID，需结合阅读字体 revision 判断内容变化

---

**相关文档**：
- [多语言](../development/multilingual.md)

---

## 图像解码

### JPEG 解码

**头文件**：`platform/epd_image.h`（源码：`src/platform/epd_image.h`）

#### epd_image_jpeg_layout

```c
typedef struct {
    uint32_t width, height, stride;
    size_t work_size, y_size, u_size, v_size, buffer_size;
} epd_jpeg_layout_t;

bool epd_image_jpeg_layout(uint32_t width, uint32_t height, epd_jpeg_layout_t *out)
```

**功能**：查询 HAL 所需的工作区和输出平面大小。

**参数**：
- `width`：图像宽度
- `height`：图像高度
- `out`：输出布局信息

**返回值**：是否成功

**说明**：
- 有效图像为 width × height
- Y 行跨度为 stride

---

#### epd_image_jpeg_decode

```c
bool epd_image_jpeg_decode(const void *input, size_t length,
                          const epd_jpeg_layout_t *layout, void *buffer,
                          bool (*cancel)(void *), void *context)
```

**功能**：在 worker 中同步解码 baseline YCbCr 4:2:0 JPEG。

**参数**：
- `input`：输入数据（32 字节对齐并补齐）
- `length`：输入长度
- `layout`：查询得到的原始 layout
- `buffer`：RAM1 工作和输出缓冲（64 字节对齐，容量为 buffer_size）
- `cancel`：取消回调
- `context`：回调上下文

**返回值**：
- `true`：成功
- `false`：失败或取消

**说明**：
- GPU 锁忙时立即返回 `false`
- 函数返回后（包括取消或失败），硬件已停止访问这些缓冲

---

### SVG 渲染

#### epd_svg_render

```c
typedef struct {
    uint8_t *pixels;
    uint32_t width, height;
} epd_svg_bitmap_t;

int epd_svg_render(const void *svg, size_t length, uint32_t max_width, uint32_t max_height,
                  bool (*load)(const char *, epd_svg_bitmap_t *, void *), void *user,
                  bool (*cancel)(void *), epd_svg_bitmap_t *out)
```

**功能**：从线程调用渲染 SVG 为白底紧密排列的 gray8 位图。

**参数**：
- `svg`：SVG 数据
- `length`：数据长度（上限 256 KiB）
- `max_width`, `max_height`：最大尺寸（输出上限 684×1216）
- `load`：加载图片回调
- `user`：回调用户数据
- `cancel`：取消回调
- `out`：输出位图

**返回值**：
- `0`：成功
- `-1`：参数无效
- `-2`：取消
- `-3`：内存不足
- `-4`：不支持
- `-5`：I/O 失败

**说明**：
- 在 LVGL 初始化后调用
- 单张嵌入图片上限 384×608
- 适配尺寸不放大原图
- 成功后用 `epd_app_free(out->pixels)` 释放
- 失败时 out 为空
- 加载回调用 `epd_app_alloc` 分配像素，返回后像素所有权交给渲染器
- 加载和取消回调不能重入渲染

---

## 附录

### 常用常量定义

```c
// 应用 ABI 版本
#define EPD_APP_ABI 2u

// 服务 ID 容量
#define EPD_SERVICE_ID_SIZE 64u

// 服务事件位
#define EPD_SERVICE_WAKE 1u
#define EPD_SERVICE_NETWORK 2u
#define EPD_SERVICE_STOP 4u

// 存储路径最大长度
#define STORAGE_PATH_MAX 512

// AI 限制
#define EPD_AI_REQUEST_LIMIT (128u * 1024u)
#define EPD_AI_TEXT_LIMIT (64u * 1024u)
#define EPD_AI_MAX_MODELS 8u
#define EPD_AI_MAX_PROFILES 8u

// 语音限制
#define EPD_SPEECH_ASR_LIMIT_MS 600000u
#define EPD_SPEECH_SAMPLE_RATE 16000u
#define EPD_SPEECH_PCM_SLOT_BYTES (2u * 1024u * 1024u)

// 字体错误码
#define EPD_APP_FONT_NO_MEMORY (-3)
#define EPD_APP_FONT_PIN_PENDING (-2)
```

---

### 错误码枚举

```c
// AI 错误码
typedef enum {
    EPD_AI_OK, EPD_AI_INVALID, EPD_AI_UNSUPPORTED, EPD_AI_UNAUTHORIZED,
    EPD_AI_BUSY, EPD_AI_NO_MEMORY, EPD_AI_NO_NETWORK, EPD_AI_DNS,
    EPD_AI_CONNECT, EPD_AI_TLS, EPD_AI_AUTH, EPD_AI_RATE_LIMIT,
    EPD_AI_SERVER, EPD_AI_PROTOCOL, EPD_AI_LIMIT, EPD_AI_TIMEOUT,
    EPD_AI_CANCELLED, EPD_AI_INCOMPLETE, EPD_AI_STORAGE, EPD_AI_NOT_FOUND
} epd_ai_error_t;

// 语音错误码
typedef enum {
    EPD_SPEECH_OK, EPD_SPEECH_INVALID, EPD_SPEECH_BUSY, EPD_SPEECH_NO_MEMORY,
    EPD_SPEECH_NOT_CONFIGURED, EPD_SPEECH_NO_NETWORK, EPD_SPEECH_CONNECT,
    EPD_SPEECH_TLS, EPD_SPEECH_AUTH, EPD_SPEECH_QUOTA, EPD_SPEECH_PROTOCOL,
    EPD_SPEECH_TIMEOUT, EPD_SPEECH_CANCELLED, EPD_SPEECH_STORAGE,
    EPD_SPEECH_INPUT_UNAVAILABLE, EPD_SPEECH_INPUT_OVERRUN,
    EPD_SPEECH_TEXT_TOO_LONG, EPD_SPEECH_AUDIO_TOO_LARGE,
    EPD_SPEECH_OUTPUT_INTERRUPTED, EPD_SPEECH_OUTPUT_TIMEOUT,
    EPD_SPEECH_NOT_FOUND, EPD_SPEECH_UNSUPPORTED
} epd_speech_error_t;
```

---

## 相关文档索引

### 入门教程
- [快速开始](../getting-started/quick-start.md)
- [核心概念](../getting-started/core-concepts.md)
- [第一个应用](../development/first-application.md)

### 开发指南
- [动态应用开发](../development/application-development.md)
- [代码示例集](../development/code-examples.md)
- [开发最佳实践](../development/best-practices.md)
- [应用生命周期](../development/application-lifecycle.md)
- [界面与输入](../development/touch-ui.md)
- [应用存储](../development/application-storage.md)
- [应用内存](../development/application-memory.md)
- [图像与字体](../development/application-resources.md)
- [多语言](../development/multilingual.md)
- [公共组件](../development/common-components.md)

### 平台服务
- [网络服务](../services/wifi.md)
- [AI 服务](../services/ai-services.md)
- [语音服务](../services/speech-services.md)
- [电源管理](../services/power-management.md)

### 系统架构
- [系统架构总览](../architecture/architecture.md)
- [应用运行与生命周期](../architecture/application-runtime.md)
- [内存与存储](../architecture/memory-storage.md)
- [显示、输入与调度](../architecture/display-input.md)

### 其他参考
- [应用接口参考](api-reference.md)（简化版本）
- [界面控件 API](api-ui-controls.md)
- [应用清单](app-manifest.md)
- [常见问题](faq.md)

---

## 版本信息

- **API ABI 版本**：2
- **文档版本**：1.0
- **最后更新**：2024-11

---

**注意事项**：
1. 所有 UI 相关接口必须在 UI 线程调用
2. 异步字体需等待 status==1 后再使用
3. 内存分配失败时应保留状态重试，不应直接放弃
4. Worker 收到 STOP 事件后应尽快清理并返回
5. 删除对象前确保移除所有引用
6. 网络请求失败的 POST 不会自动重试
7. 借用的字符串和指针不要超出其生命周期使用
8. 配对使用的接口（acquire/release, lock/unlock）必须平衡调用

---

如需更详细的使用说明和示例，请参阅对应的开发指南文档。
