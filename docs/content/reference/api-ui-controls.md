---
title: "界面控件 API"
---

# 界面控件 API

本文档详细介绍平台提供的界面控件创建和管理接口。所有接口均在 **UI 线程**调用。

::: tip 头文件
```c
#include "platform/epd_app.h"
```
:::

## 页面管理

### epd_app_header

声明使用标准页眉的普通页面。

```c
void epd_app_header(lv_obj_t *screen);
```

**参数**：
- `screen` - 页面屏幕对象

**说明**：
- 普通页面使用平台标准页眉（显示时间、电量等）
- 页眉声明随 screen 删除自动释放
- 页面内容应避开页眉区域，使用 `epd_ui_content_area()` 获取可用区域

**示例**：
```c
static void page_message(gui_app_msg_type_t message, void *param) {
    if (message == GUI_APP_MSG_ONSTART) {
        lv_obj_t *screen = lv_screen_active();
        
        // 声明使用标准页眉
        epd_app_header(screen);
        
        // 获取内容区域
        lv_area_t area;
        epd_ui_content_area(screen, &area);
        
        // 在内容区域内布局
        create_content(screen, &area);
    }
}
```

### epd_app_header_refresh

刷新页眉显示（时间、电量等）。

```c
void epd_app_header_refresh(lv_obj_t *screen);
```

**参数**：
- `screen` - 已声明页眉的屏幕对象

**使用场景**：
- 应用从后台恢复时（ONRESUME）
- 检测到时间或电量变化时

### epd_app_set_immersive

声明沉浸式页面（无页眉）。

```c
void epd_app_set_immersive(lv_obj_t *screen, bool immersive);
```

**参数**：
- `screen` - 页面屏幕对象
- `immersive` - true 为沉浸式，false 为普通模式

**说明**：
- 沉浸式页面可以使用全屏区域
- 顶部边缘下拉仍可打开通知或控制中心
- 阅读器等全屏内容应用适合使用沉浸模式

**示例**：
```c
// 阅读器全屏显示
lv_obj_t *screen = lv_screen_active();
epd_app_set_immersive(screen, true);

// 绘制全屏内容
lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES);
```

## 基础控件创建

### epd_app_object_init

为应用对象初始化平台基础样式。

```c
void epd_app_object_init(lv_obj_t *obj);
```

**参数**：
- `obj` - 要初始化的对象

**说明**：
- 设置基础样式（背景、边框、文字颜色等）
- 对象仍由创建者拥有和管理

### epd_app_label

创建文本标签（左对齐）。

```c
lv_obj_t *epd_app_label(lv_obj_t *parent, const char *text, 
                        int x, int y, int width, const lv_font_t *font);
```

**参数**：
- `parent` - 父对象
- `text` - 初始文本（可为空字符串）
- `x`, `y` - 位置坐标
- `width` - 宽度
- `font` - 字体

**返回值**：
- 创建的标签对象，归属于 parent

**示例**：
```c
const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
lv_obj_t *label = epd_app_label(screen, "Hello", 80, 200, 500, font);
```

### epd_app_centered_label

创建居中文本标签。

```c
lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text,
                                 int x, int y, int width, const lv_font_t *font);
```

**参数**：与 `epd_app_label` 相同

**返回值**：
- 创建的居中标签对象

### epd_app_panel

创建面板容器。

```c
lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int width, int height);
```

**参数**：
- `parent` - 父对象
- `x`, `y` - 位置坐标  
- `width`, `height` - 尺寸

**返回值**：
- 创建的面板对象

**示例**：
```c
// 创建卡片面板
lv_obj_t *card = epd_app_panel(screen, 40, 200, 604, 400);

// 在面板内添加内容
lv_obj_t *title = epd_app_label(card, "标题", 20, 20, 564, title_font);
```

### epd_app_icon

创建图标。

```c
lv_obj_t *epd_app_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y);
```

**参数**：
- `parent` - 父对象
- `src` - 图像描述符
- `x`, `y` - 位置坐标

**返回值**：
- 创建的图标对象

**重要**：
- 图像描述符及像素数据需在使用期间保持有效
- 不要使用栈上的临时图像数据

### epd_app_button

创建标准按钮。

```c
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data);
```

**参数**：
- `parent` - 父对象
- `x`, `y` - 位置坐标
- `width`, `height` - 尺寸
- `text` - 按钮文本
- `callback` - 点击回调函数
- `data` - 传递给回调的用户数据

**返回值**：
- 创建的按钮对象

**说明**：
- 按钮自动加入最近的受管输入组
- data 在回调期间必须保持有效

**示例**：
```c
static void on_button_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    // 处理点击
    page->count++;
    update_display(page);
}

lv_obj_t *btn = epd_app_button(screen, 80, 400, 524, 100,
                               "点击我", on_button_click, page);
```

## 文本更新

### epd_app_text

更新标签文本（带变化检测）。

```c
bool epd_app_text(lv_obj_t *label, const char *text);
```

**参数**：
- `label` - 要更新的标签
- `text` - 新文本

**返回值**：
- `true` - 更新成功
- `false` - 分配失败，保留旧文本

**说明**：
- 复制文本进行变化检测
- 文本未变化时不触发重绘
- 分配失败时保留旧状态，可稍后重试
- 同一标签的所有文本更新都应使用此函数

**示例**：
```c
char text[64];
snprintf(text, sizeof(text), "计数: %d", count);

bool updated = epd_app_text(label, text);
if (!updated) {
    // 分配失败，保留待更新状态
    page->pending_update = true;
}
```

### epd_app_status_text

更新状态文本（使用局刷预算）。

```c
bool epd_app_status_text(lv_obj_t *label, const char *text);
```

**参数**：
- `label` - 状态标签
- `text` - 新文本

**返回值**：
- `true` - 更新成功
- `false` - 分配失败

**说明**：
- 用于频繁更新的状态信息（如时间、进度）
- 自动使用局部刷新预算优化
- 其他行为同 `epd_app_text`

## 返回按钮

### epd_app_back_button

创建标准返回按钮。

```c
lv_obj_t *epd_app_back_button(lv_obj_t *screen);
```

**参数**：
- `screen` - 页面屏幕对象

**返回值**：
- 创建的返回按钮

**说明**：
- 点击时关闭当前页面
- 自动处理返回导航

### epd_app_back_button_cb

创建带自定义回调的返回按钮。

```c
lv_obj_t *epd_app_back_button_cb(lv_obj_t *parent, lv_event_cb_t callback, void *data);
```

**参数**：
- `parent` - 父对象
- `callback` - 点击回调
- `data` - 用户数据

**返回值**：
- 创建的返回按钮

**使用场景**：
- 需要在返回前保存数据
- 需要显示确认对话框
- 自定义返回行为

**示例**：
```c
static void on_back_click(lv_event_t *e) {
    page_t *page = lv_event_get_user_data(e);
    
    if (page->has_unsaved_changes) {
        show_save_dialog(page);
    } else {
        gui_app_close_this_page();
    }
}

lv_obj_t *back = epd_app_back_button_cb(screen, on_back_click, page);
```

### epd_app_back_button_layout

对齐返回按钮与标题中心。

```c
void epd_app_back_button_layout(lv_obj_t *button, int32_t x, int32_t center_y);
```

**参数**：
- `button` - 返回按钮对象
- `x` - X 坐标
- `center_y` - 标题中心 Y 坐标

**示例**：
```c
const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
lv_obj_t *back = epd_app_back_button(screen);
lv_obj_t *title = epd_app_label(screen, "页面标题", 104, 81, 540, title_font);

// 对齐返回按钮与标题中心
int title_center_y = 81 + title_font->line_height / 2;
epd_app_back_button_layout(back, epd_ui_px(24), title_center_y);
```

## 状态查询

### epd_app_status

获取系统状态（电量、时间）。

```c
void epd_app_status(epd_app_status_t *status);
```

**参数**：
- `status` - 输出状态的结构体指针

**结构体定义**：
```c
typedef struct {
    int battery_percent;  // 电池电量百分比（0-100）
    char clock[16];       // 格式化的时钟字符串
} epd_app_status_t;
```

**示例**：
```c
epd_app_status_t status;
epd_app_status(&status);

char text[32];
snprintf(text, sizeof(text), "电量: %d%%", status.battery_percent);
epd_app_text(battery_label, text);
```

## 刷新控制

### epd_app_request_full_refresh

请求一次全刷（清除残影）。

```c
void epd_app_request_full_refresh(void);
```

**使用场景**：
- 页面内容完全稳定后
- 检测到明显残影时
- 重要内容显示前

**示例**：
```c
static void on_page_complete(void) {
    // 页面加载完成，请求全刷清除残影
    epd_app_request_full_refresh();
}
```

### epd_app_refresh_done

检查显示刷新是否完成。

```c
bool epd_app_refresh_done(void);
```

**返回值**：
- `true` - 刷新完成
- `false` - 刷新进行中

**使用场景**：
- 资源释放前确认不再使用
- 页面切换时机判断

### epd_app_refresh_can_prepare

检查是否可以准备下一帧。

```c
bool epd_app_refresh_can_prepare(void);
```

**返回值**：
- `true` - 可以准备新内容
- `false` - 应等待当前刷新完成

### epd_app_refresh_submission

获取刷新提交序号。

```c
uint32_t epd_app_refresh_submission(void);
```

**返回值**：
- 当前刷新序号（递增）

**使用场景**：
- 检测是否有新刷新提交
- 资源交接时机判断

**示例**：
```c
static uint32_t last_submission = 0;

void check_refresh_change(void) {
    uint32_t current = epd_app_refresh_submission();
    if (current != last_submission) {
        // 有新刷新提交
        on_refresh_changed();
        last_submission = current;
    }
}
```

## 其他功能

### epd_app_set_recent_reading

发布最近阅读信息。

```c
void epd_app_set_recent_reading(const char *title, int percent);
```

**参数**：
- `title` - 书名或内容标题
- `percent` - 阅读进度百分比（0-100）

**说明**：
- 系统会在合适位置展示最近阅读
- 用于快速返回阅读

### epd_app_open_text_settings

请求打开阅读文字设置。

```c
bool epd_app_open_text_settings(void);
```

**返回值**：
- `true` - 请求已受理
- `false` - 请求被拒绝

### epd_app_open_font_install

请求打开字体安装页面。

```c
bool epd_app_open_font_install(void);
```

**返回值**：
- `true` - 请求已受理
- `false` - 请求被拒绝

### epd_app_open_reading_fonts

请求打开阅读字体选择页面。

```c
bool epd_app_open_reading_fonts(void);
```

**返回值**：
- `true` - 请求已受理
- `false` - 请求被拒绝

### epd_app_ui_available

查询 LVGL 堆可用量。

```c
size_t epd_app_ui_available(void);
```

**返回值**：
- 可用字节数
- `0` 表示无法获取

**说明**：
- 这是快照值，不是预留量
- 不保证存在同样大小的连续块

## 完整示例

### 创建简单页面

```c
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *content;
    lv_obj_t *button;
    uint32_t locale_revision;
    uint32_t scale_revision;
    int count;
} my_page_t;

static void layout_page(my_page_t *page) {
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    
    const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
    const lv_font_t *body_font = epd_app_font_role(EPD_FONT_BODY);
    
    int x = area.x1 + epd_ui_px(20);
    int y = area.y1 + epd_ui_px(20);
    int width = area.x2 - area.x1 + 1 - epd_ui_px(40);
    
    lv_obj_set_pos(page->title, x, y);
    lv_obj_set_size(page->title, width, title_font->line_height);
    
    y += title_font->line_height + epd_ui_px(40);
    lv_obj_set_pos(page->content, x, y);
    lv_obj_set_width(page->content, width);
    
    y += lv_obj_get_height(page->content) + epd_ui_px(40);
    lv_obj_set_pos(page->button, x, y);
    lv_obj_set_size(page->button, width, epd_ui_touch(100));
    
    page->scale_revision = epd_ui_scale_revision();
}

static void update_text(my_page_t *page) {
    epd_app_text(page->title, epd_get_str(my_page_title));
    
    char text[64];
    EPD_FMT_my_count_text(text, sizeof(text), page->count);
    epd_app_text(page->content, text);
    
    epd_app_text(lv_obj_get_child(page->button, 0), 
                 epd_get_str(my_button_text));
    
    page->locale_revision = epd_locale_revision();
}

static void on_settings_changed(uint32_t events, void *ctx) {
    my_page_t *page = ctx;
    
    if (page->locale_revision != epd_locale_revision()) {
        update_text(page);
    }
    
    if (page->scale_revision != epd_ui_scale_revision()) {
        layout_page(page);
    }
}

static void on_button_click(lv_event_t *e) {
    my_page_t *page = lv_event_get_user_data(e);
    page->count++;
    
    char text[64];
    EPD_FMT_my_count_text(text, sizeof(text), page->count);
    epd_app_text(page->content, text);
}

static void page_message(gui_app_msg_type_t message, void *param) {
    my_page_t *page = gui_app_this_page_memory();
    
    if (message == GUI_APP_MSG_ONSTART) {
        lv_obj_t *screen = lv_screen_active();
        page->screen = screen;
        
        // 声明页眉
        epd_app_header(screen);
        
        // 创建控件
        const lv_font_t *title_font = epd_app_font_role(EPD_FONT_TITLE);
        const lv_font_t *body_font = epd_app_font_role(EPD_FONT_BODY);
        
        page->title = epd_app_label(screen, "", 0, 0, 0, title_font);
        page->content = epd_app_label(screen, "", 0, 0, 0, body_font);
        page->button = epd_app_button(screen, 0, 0, 0, 0, "", 
                                      on_button_click, page);
        
        // 初始化状态
        page->count = 0;
        page->locale_revision = epd_locale_revision() - 1;
        
        // 更新内容和布局
        update_text(page);
        layout_page(page);
        
        // 监听变化
        epd_app_ui_listen(screen, 
            EPD_SERVICE_UI_SETTINGS | EPD_SERVICE_UI_DISPLAY,
            on_settings_changed, page);
            
    } else if (message == GUI_APP_MSG_ONRESUME) {
        // 刷新页眉
        epd_app_header_refresh(page->screen);
        // 重新检查文本和布局
        on_settings_changed(0, page);
        
    } else if (message == GUI_APP_MSG_ONSTOP) {
        // 取消监听
        epd_app_ui_listen(page->screen, 0, NULL, NULL);
    }
}

int app_main(intent_t intent) {
    return gui_app_regist_msg_handler_ext("myapp", page_message, 
                                          NULL, sizeof(my_page_t));
}
```

## 相关文档

- **输入管理**：[输入与尺度 API](api-reference.md#输入与尺度)
- **字体接口**：[字体 API](api-reference.md#字体与图像)
- **完整参考**：[应用接口参考](api-reference.md)
- **开发指南**：[界面与输入](../development/touch-ui.md)
