---
title: "恢复模式"
---

# 恢复模式

恢复模式用于停止动态应用装载，让用户继续进入固件内置的设置页安装、更新或卸载应用。恢复模式同时拦截 catalog 的后台启动和手动打开入口，拦截发生在 `app_module_open()` 之前，因此这些入口不会执行 ELF 模块的 `module_init`、应用入口或后台服务。

## 普通固件的持久化开关

finsh/MSH 提供以下命令，需要启用 `RT_USING_FINSH` 及 MSH 命令表：

```text
recovery status
recovery on
recovery off
```

`recovery on` 保存下次启动进入恢复模式的选择。保存成功后手动重启，再从“设置 → 应用管理 → 应用卸载”移除有问题的应用。`recovery off` 保存下次启动恢复正常运行的选择，同样需要手动重启。`recovery` 不带参数等同于 `recovery status`。

本次启动模式在初始化时确定，运行期间保持不变。正常启动后执行 `on` 不会停止已有页面和服务；恢复启动后执行 `off` 不会立即加载应用。重复调用初始化也不会改变本次模式。

状态输出中的 `current` 表示本次启动模式；`next` 表示持久化选择在普通固件下的下次启动模式；`forced=1` 表示当前固件强制恢复；`load_result` 表示本次初始化读取开关的结果。读取失败时 `next=unknown`，明确保存成功后才更新 `next`。`load_result` 保留本次初始化结果。

恢复开关保存在内部存储的 `/flash/system/recovery.bin`，独立于显示设置和应用数据。记录采用 `storage_record_save/load`，包含标识 `0x45505231`、版本 `1`、长度、CRC32 校验和一个值为 `0` 或 `1` 的字节。保存沿用存储层的临时文件写入、同步、关闭和替换流程。

普通固件在记录不存在时正常启动，不创建记录，也不增加等待时间。记录损坏、取值非法或内部存储无法读取时，本次启动保持恢复模式并返回读取错误。

## 启动时立即崩溃的应用

后台模块如果在启动时立即崩溃，用户可能无法及时执行 finsh 命令。这种情况使用编译时定义 `APP_RECOVERY=1` 的恢复固件。该固件无论持久化选择为何值，都禁止 catalog 装载动态应用；初始化不会为此改写恢复记录。

在 `SiFli-EPD-Platform` 平台源码根目录，加载 SDK 环境后执行：

```powershell
scons -C project --board=sf32lb57-epd-gyd7n6_hcpu --board_search_path=../boards APP_RECOVERY=1 -j8
```

恢复后编译普通固件时去掉 `APP_RECOVERY=1`。

恢复时仅更新固件及配套启动镜像，保留内部文件系统，不烧录 `fs_root`。进入内置设置卸载故障应用后，再刷回普通固件。普通固件仍按保存的开关启动；如果曾执行 `recovery on`，需要成功执行 `recovery off` 才能重新启用动态应用。恢复固件内执行 `recovery off` 只更新保存的选择，不能解除当前固件的强制恢复。

`APP_RECOVERY` 未定义或定义为 `0` 时均使用普通固件策略。编译开关不增加启动延迟，不触发自动重启。

## 初始化与执行入口

`app_recovery_init()` 在 `storage_init()` 完成后、首次执行动态应用之前调用。`app_catalog_init()` 先初始化恢复策略，再注册动态装载回调。

其他启动流程可以在 `board_start_filesystem()` 返回后提前调用 `app_recovery_init()`。读取失败时仍应继续初始化内置 UI，利用 `app_recovery_active()` 决定是否进入内置设置页面。

在 `ui_app_init()` 成功后，当前启动流程使用以下调用请求进入内置设置，实际页面切换由既有 UI 循环处理：

```c
if (app_recovery_active()) launcher_open(UI_PAGE_SETTINGS);
```

`app_recovery_check_dynamic()` 提供可复用的执行检查。catalog 在手动装载回调和后台扫描入口调用检查；应用设置页在创建动态应用 intent 前也调用检查。初始化之前不允许动态装载。内置页面通过固件注册表解析，不受动态装载检查影响。catalog 元数据刷新、包读取和安装卸载请求继续工作。

| 接口或操作 | 结果 |
| --- | --- |
| 检查允许执行、初始化或保存成功 | `RT_EOK` |
| 恢复模式拒绝执行 | `APP_RECOVERY_BLOCKED`，即 `-EACCES` |
| 初始化前检查或保存 | `-RT_EBUSY` |
| 开关读取或保存失败 | `-RT_EIO` |
| finsh 命令参数错误 | `-RT_EINVAL` |

catalog 的 SDK 回调类型为 `bool`，拒绝时返回 `false`，清空入口与模块句柄，并将上述检查错误写入线程 errno；调用方也可直接调用 `app_recovery_check_dynamic()` 获取原因。保存失败时不会更新本次模式或内存中的下次启动选择，finsh 输出 `save failed` 并返回错误。

执行保护的边界是平台应用入口。SDK 自带的 `msh_exec_module()` 可以直接调用 `dlmodule_exec()`，原始 `dlopen()` 和 `dlmodule_load()` 也不经过 catalog。需要覆盖这些底层执行入口的固件，须在项目侧为入口接入同一检查；仅定义 `APP_RECOVERY=1` 不会修改 SDK 装载器。动态模块仍与固件共享地址空间。
