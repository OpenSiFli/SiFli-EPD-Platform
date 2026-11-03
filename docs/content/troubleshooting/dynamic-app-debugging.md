---
title: "故障取证"
---

# 故障取证

## 发布文件

固件和动态应用沿用 SDK 构建入口。固件生成 `main.elf`，模块生成安装用 `.so` 和带完整调试信息的 `.so.nostrip`。

开发者保存每次发布的主固件、应用包、模块和私有库符号、源码及构建配置。故障分析使用设备实际运行版本的配套文件。仓库内模块的符号位于 `modules/<应用ID>/output/`，私有库位于对应库的 `output/`；这些目录会在后续构建时更新，发布时由开发者留存。

应用清单中的 `build_id` 记录打包配置，应用源码版本和模块摘要另行保存。符号定位使用设备实际运行固件及模块对应的调试文件。安装文件为 `project/build_<board>/app-packages/<应用ID>.sap`。

## 现场采集

保留完整启动和故障日志，并使用 SDK 已有的 J-Link、AssertDump 或对应芯片支持的导出方式获取寄存器和内存。现场应包含相关线程对象、线程栈、模块对象，以及动态模块所在的 SRAM 或 PSRAM。

系统异常和断言沿用 SDK 的处理流程。动态模块主线程触发断言时，SDK 按模块线程退出分支处理；主 UI 线程或其他系统线程中的终止性故障按系统故障路径处理。

SDK 的 `saved_stack_frame`、`saved_stack_pointer`、`saved_scb_reg` 和 `error_reason` 用于解释已保存现场。调试器读到的当前 PC 可能处于后续等待循环；断言日志中的源码位置、SDK 保存的寄存器和暂停时的寄存器应分别核对。

处理器无法调度时，使用调试器暂停并导出寄存器与内存。

### SF32LB57GY：J-Link 导出后转为 GDB core

以下路径适用于本项目 16 MiB PSRAM 的 57GY 板型，使用 J-Link 通过 SWD 连接 HCPU。先保留故障日志和现场，不复位、不重新烧录。导出工具随 SDK 提供，接口说明见 SDK dump 死机调试指南（源码：`SiFli-SDK/docs/source/zh_CN/app_development/debuging/dump_assert_debug.md`）。

当前 SDK 的 `tools/AssertDump/sf32lb57x_psramless.jlink` 只导出 SRAM。将该文件复制为工作目录中的 `capture-57gy.jlink`，在 `exit` 前增加下面一行，保留原来的暂停和 SRAM 导出命令：

```text
savebin psram.bin 0x60000000 0x1000000
```

以上范围覆盖当前板型的 16 MiB PSRAM，包括其中的动态模块、堆和线程栈。将脚本副本与采集文件一同保存。57 系列使用 J-Link；SDK 的 `capture-live --transport uart` 适用于 52/56 系列。

在 `SiFli-EPD-Platform` 平台源码根目录、已加载 SDK 环境且能找到 `JLink.exe` 的终端执行本节命令：

```powershell
sdk.py crash-dump capture-live --transport jlink --chip SF32LB57 --core hcpu --script ./capture-57gy.jlink --output ./crash-57gy --elf ./project/build_sf32lb57-epd-gyd7n6_hcpu/main.elf
sdk.py crash-dump readcore --package ./crash-57gy --elf ./project/build_sf32lb57-epd-gyd7n6_hcpu/main.elf --output ./crash-57gy/coredump.elf
```

其他板型替换为设备实际运行版本的 `main.elf`。第一条命令生成内存文件、采集日志和 `sdk_manifest.json`；先检查清单中的完整性、警告、寄存器来源，以及 `psram.bin` 是否为 16,777,216 字节，再执行转换。第二条命令生成下面 `core-file` 使用的 `coredump.elf`。保留原始采集目录，以便重新转换和核查寄存器来源。

已有 AssertDump 目录可以按 SDK 指南分析。转换前检查导出范围包含所需 PSRAM，并确认故障寄存器有效。

## 模块地址与符号

SDK 的 `rt_dlmodule` 对象保存模块执行地址 `exec_mem_space` 和 ELF 首个装载段地址 `vstart_addr`。当前连续 RAM 装载路径的地址偏移为：

```text
load_bias = exec_mem_space - vstart_addr
```

通过调试器或包含模块对象的转储取得地址，为涉及的应用和私有库分别加载其发布时的符号：

```text
arm-none-eabi-gdb <配套main.elf>
(gdb) core-file <SDK转换得到的core文件>
```

模块对象登记在 RT-Thread 的模块对象链表中，入口见 源码：`SiFli-SDK/rtos/rtthread/src/object.c`，对象布局见 源码：`SiFli-SDK/rtos/rtthread/components/libc/libdl/dlmodule.h`。加载主固件和 core 后，可只读遍历该链表，取得模块名、对象地址及装载地址：

```text
(gdb) set $head = &rt_object_container[RT_Object_Info_Module].object_list
(gdb) set $node = $head->next
(gdb) while $node != $head
 >set $module = (struct rt_dlmodule *)((char *)$node - (unsigned long)&((struct rt_object *)0)->list)
 >printf "%.8s object=%p exec=%p vstart=0x%x\n", $module->parent.name, $module, $module->exec_mem_space, $module->vstart_addr
 >set $node = $node->next
 >end
```

从全局模块链表的输出中选择目标应用或私有库对象。离线 core 通过读取对象内存定位模块；模块内部的 `object_list` 管理该模块自己的内核对象。读取失败或链表损坏时，先核对采集范围与配套 ELF，取得有效装载地址后再计算偏移。

用选定对象的 `exec_mem_space - vstart_addr` 计算偏移，再加载符号：

```text
(gdb) add-symbol-file <配套books.so.nostrip> -o <books的load_bias>
(gdb) add-symbol-file <配套bk_mobi.so.nostrip> -o <私有库的load_bias>
(gdb) info registers
(gdb) bt
(gdb) list *<原始PC地址>
```

`-o` 的参数为上述整体装载偏移。需要按节指定时，先用 `arm-none-eabi-readelf -SW` 查看节地址，再加上装载偏移。开发机源码路径变化时，使用 `set substitute-path <原路径> <本机路径>`。

调用栈分析使用与栈内存对应的寄存器上下文。SDK 的单寄存器 core 保存一个上下文；分析其他线程时读取该线程保存的寄存器和栈，再通过符号及指令核对调用关系。

## 设备恢复

启动受到问题应用影响时，使用[应用恢复入口](recovery.md)进入内置管理界面，处理应用包后重新启动。恢复模式与故障现场采集分别使用各自已有的接口。
