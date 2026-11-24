---
title: "常见问题"
---

# 常见问题（FAQ）

本文收集 SiFli EPD Platform 开发过程中的常见问题和解决方案。

## 环境和构建

### Q: `git submodule update` 很慢或失败？

**A**: 子模块仓库可能网络不佳，尝试以下方法：

```powershell
# 方法 1: 使用代理
git config --global http.proxy http://proxy.example.com:port

# 方法 2: 增加超时时间
git config --global http.postBuffer 524288000
git config --global http.lowSpeedLimit 0
git config --global http.lowSpeedTime 999999

# 方法 3: 使用 SSH 而非 HTTPS
# 编辑 .gitmodules，将 https:// 改为 git@
```

### Q: `scons` 命令找不到？

**A**: 每次打开新终端都需要激活 SDK 环境：

```powershell
. .\SiFli-SDK\export.ps1
```

建议将此命令添加到 PowerShell 配置文件中：

```powershell
# 编辑配置文件
notepad $PROFILE

# 添加以下内容（修改为实际路径）
Set-Location D:\projects\SiFli-EPD-Platform
. .\SiFli-SDK\export.ps1
```

### Q: 构建时报错"找不到 arm-none-eabi-gcc"？

**A**: SDK 工具链未正确安装或未激活环境：

```powershell
# 重新安装 SDK
.\SiFli-SDK\install.ps1

# 激活环境
. .\SiFli-SDK\export.ps1

# 验证
Get-Command arm-none-eabi-gcc
```

### Q: `python tools/prepare_build.py` 报错？

**A**: 常见原因和解决方法：

1. **子模块未初始化**：
```powershell
git submodule update --init --recursive
```

2. **uv 未安装**：
```powershell
winget install --id=astral-sh.uv -e
```

3. **网络问题**：使用代理或切换网络

4. **权限问题**：以管理员身份运行 PowerShell

### Q: 构建应用时报错"undefined reference"？

**A**: 应用使用了固件未导出的符号：

1. **检查固件版本**：确保应用和固件版本匹配
2. **查看导出符号**：检查 `exports.map` 是否正确
3. **重新构建固件**：确保使用最新的固件

```powershell
# 重新构建固件和应用
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu -j8
scons -C modules APPS=myapp -j8
```

## 烧录和调试

### Q: 烧录卡在"等待芯片连接"？

**A**: 按以下步骤排查：

1. **按复位键**：让芯片重新进入下载模式
2. **检查串口连接**：确认 COM 口正确且未被占用
3. **检查电源**：确保开发板供电正常
4. **更换 USB 线**：某些线缆只能充电不能传输数据
5. **尝试其他 COM 口**：某些板子有多个串口

### Q: 烧录完成但设备无法启动？

**A**: 可能的原因：

1. **分区地址错误**：使用生成的下载脚本，不要手动修改地址
2. **镜像不完整**：重新构建并烧录全部镜像
3. **波形文件缺失**：确保烧录了 `epd_waveform.bin`
4. **硬件问题**：检查电源和屏幕连接

### Q: 串口无输出？

**A**: 检查串口参数：

- **波特率**：1000000（非常重要！）
- **数据位**：8
- **校验位**：无
- **停止位**：1

```powershell
# 使用 PuTTY
putty -serial COM18 -sercfg 1000000,8,n,1,N

# 或使用其他串口工具，确保波特率为 1000000
```

### Q: 如何进入 DFU 恢复模式？

**A**: 使用设备菜单或按键组合：

**方法 1：从系统进入**
1. 打开"设置 → 固件更新"
2. 选择"进入固件恢复模式"
3. 设备重启进入 DFU

**方法 2：手动进入**（开发板相关）
1. 按住特定按键组合
2. 按复位键
3. 释放按键组合

详见：[恢复模式](../troubleshooting/recovery.md)

## 应用开发

### Q: 应用安装失败，提示"ABI 不兼容"？

**A**: 应用和固件的二进制接口不匹配：

**解决方法**：
1. 确保应用使用配套固件构建
2. 固件更新后重新构建应用

```powershell
# 先构建固件
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu -j8

# 再构建应用（自动读取固件信息）
scons -C modules APPS=myapp -j8
```

### Q: 应用运行时闪退？

**A**: 按以下步骤排查：

1. **查看日志**：串口会输出崩溃信息
2. **检查内存分配**：是否检查了 `epd_app_alloc` 的返回值
3. **检查指针**：是否访问了空指针或已释放的内存
4. **检查生命周期**：是否在对象删除后访问
5. **查看堆栈跟踪**：使用 `addr2line` 定位崩溃位置

```bash
# 定位崩溃地址
arm-none-eabi-addr2line -e project/build_xxx/main.elf 0x12345678
```

### Q: 页面显示不正常或控件位置错误？

**A**: 可能的原因：

1. **未响应缩放变化**：
```c
// 监听缩放变化
static void on_scale_changed(uint32_t events, void* ctx) {
    if (page->scale_revision != epd_ui_scale_revision()) {
        relayout_page(page);
        page->scale_revision = epd_ui_scale_revision();
    }
}
```

2. **硬编码坐标**：使用 `epd_ui_px()` 和 `epd_ui_touch()` 转换尺寸
3. **未考虑内容区域**：使用 `epd_ui_content_area()` 获取可用区域

### Q: 文本更新不生效？

**A**: 使用 `epd_app_text()` 更新文本：

```c
// ✅ 正确
bool updated = epd_app_text(label, new_text);
if (!updated) {
    // 分配失败，保留旧文本，稍后重试
}

// ❌ 错误：直接使用 LVGL API
lv_label_set_text(label, new_text);  // 无变化检测
```

### Q: 后台服务无法启动？

**A**: 检查以下问题：

1. **导出符号**：确保导出了 `epd_app_background`
```c
const epd_background_t epd_app_background = {
    .run = my_service_run,
    .stack_size = 8192,
    .priority = 10,
    // ...
};
```

2. **栈大小**：是否足够（至少 4096 字节）
3. **返回错误码**：`app_service_start` 返回 NULL 时检查日志
4. **模块句柄**：是否传入了正确的模块句柄

### Q: 应用内存不足？

**A**: 优化内存使用：

1. **及时释放**：不用的资源立即释放
2. **分步处理**：大文件分块读取
3. **压缩数据**：使用更紧凑的数据结构
4. **实现 flush()**：支持内存压力回收
5. **检查泄漏**：确保所有分配都配对释放

```c
// 检查可用内存
size_t available = epd_app_psram_available();
LOG_I("myapp", "PSRAM available: %zu bytes", available);
```

## 存储和文件

### Q: 文件保存失败？

**A**: 按以下步骤排查：

1. **检查存储可用性**：
```c
if (!storage_available(STORAGE_VOLUME_SD)) {
    LOG_E("myapp", "SD card not available");
    return -ENOENT;
}
```

2. **检查空间**：
```c
storage_info_t info;
storage_info(STORAGE_VOLUME_SD, &info);
if (info.used + file_size > info.total) {
    return -ENOSPC;
}
```

3. **使用正确路径**：通过 `storage_app_path` 获取
4. **原子操作**：使用 `storage_file_commit` 避免损坏

### Q: TF 卡无法识别？

**A**: 检查以下问题：

1. **卡格式**：确保是 FAT32 格式
2. **卡容量**：某些大容量卡可能不兼容
3. **接触不良**：重新插拔 TF 卡
4. **系统状态**：查看串口日志的挂载信息

```bash
# 在 msh 中检查
msh> ls /sdcard
```

### Q: USB 读卡器模式下无法写入？

**A**: 读卡器模式下，TF 卡由电脑独占：

1. **正确流程**：
   - 打开 USB 读卡器
   - 在电脑复制文件
   - **安全弹出磁盘**
   - 关闭 USB 读卡器
   - 设备重新挂载 TF 卡

2. **错误做法**：
   - 未安全弹出直接拔线
   - 电脑访问期间设备同时访问

## 网络和在线服务

### Q: Wi-Fi 无法连接？

**A**: 排查步骤：

1. **检查密码**：确保密码正确
2. **信号强度**：靠近路由器测试
3. **路由器设置**：
   - 2.4GHz 频段（不支持 5GHz）
   - WPA2 加密（不支持 WEP、企业级认证）
   - 允许新设备接入
4. **查看日志**：串口会输出连接失败原因

### Q: 网络请求超时？

**A**: 可能的原因：

1. **网络不稳定**：检查信号强度
2. **服务器问题**：使用浏览器测试同一 URL
3. **超时时间太短**：增加超时参数
4. **DNS 解析失败**：尝试使用 IP 地址

```c
// 增加超时时间
http_config.timeout_ms = 30000;  // 30 秒
```

### Q: AI 或语音服务不可用？

**A**: 检查配置：

1. **服务配置**：在网页管理中配置 API 密钥
2. **网络连接**：确保网络可用
3. **应用授权**：在 AI 设置中授权应用
4. **配额检查**：确认服务商账户配额

## 显示和刷新

### Q: 屏幕残影严重？

**A**: 请求全刷清除残影：

```c
// 在合适时机请求全刷
epd_app_request_full_refresh();
```

平台会自动在累计一定次数局刷后执行全刷。

### Q: 刷新太慢或太快？

**A**: 刷新策略由平台自动管理：

- **局部刷新**：约 300-500ms，用于频繁更新
- **全局刷新**：约 1-2 秒，用于清除残影

应用可以请求全刷但无法强制局刷。

### Q: 页面显示不完整？

**A**: 检查绘制区域：

```c
// 使用内容区域而非全屏
lv_area_t area;
epd_ui_content_area(screen, &area);

// 在此区域内布局控件
lv_obj_set_pos(obj, area.x1 + padding, area.y1 + padding);
```

## 性能和功耗

### Q: 应用响应慢？

**A**: 优化建议：

1. **避免 UI 线程阻塞**：耗时操作放到后台
2. **减少刷新频率**：只在实际变化时更新
3. **使用缓存**：避免重复计算
4. **分批处理**：大量数据分批处理
5. **优化算法**：使用更高效的算法

### Q: 耗电快？

**A**: 功耗优化：

1. **减少网络活动**：合理设置轮询间隔
2. **避免频繁刷新**：墨水屏刷新耗电大
3. **及时休眠**：没有活动时让设备休眠
4. **关闭不用的服务**：停止后台服务
5. **实现电源管理**：响应休眠请求

```c
// 服务接收休眠事件
const epd_background_t epd_app_background = {
    .flush = my_flush,  // 保存状态
    // ...
};
```

## 升级和兼容性

### Q: 固件升级后应用无法使用？

**A**: 重新构建应用：

```powershell
# 使用新固件重新构建应用
scons -C modules APPS=myapp -j8

# 重新安装应用
```

### Q: 如何保持向后兼容？

**A**: 注意以下事项：

1. **数据版本**：在 `app.json` 中声明 `data_version`
2. **配置格式**：使用可扩展的格式（JSON）
3. **版本检查**：读取旧数据时检查版本
4. **升级迁移**：提供数据迁移代码

```c
// 数据版本检查和迁移
if (config_version == 1) {
    migrate_v1_to_v2(config);
}
```

## 更多帮助

如果以上内容没有解决你的问题：

1. **查看日志**：串口输出包含详细错误信息
2. **搜索文档**：使用文档站搜索功能
3. **查看示例**：参考现有应用的实现
4. **阅读源码**：查看平台源码中的注释
5. **社区支持**：在 GitHub Issues 提问

## 相关文档

- **故障排查**：[问题排查与最终处理](../troubleshooting/troubleshooting.md)
- **日常调试**：[开发调试](../troubleshooting/development-debugging.md)
- **最佳实践**：[开发最佳实践](../development/best-practices.md)
