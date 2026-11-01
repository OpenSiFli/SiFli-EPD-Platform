# 阅读图片计算库：接口、资源预算与验证

PNG、JPEG、SVG 和灰度页面合成已提供 ACPU 同步计算接口。SVG 使用现有 SDK 的 LVGL SVG 前端和 ThorVG 软件光栅化器，正常 SVG 可以直接进入文档阅读链路。SVG 分配失败通过 C++ 异常展开、单次解码分配域回收和 SDK 私有状态复位恢复；失败后可以在同一进程继续解码。

## 交付文件与接口

公共接口位于 include/reader_image.h；两个 compute.json 使用 acpu_library: true，不注册操作入口。epd/reader-image 提供纯位图计算，epd/reader-image-svg 传递依赖前者并提供 SVG。清单路径全部位于组件内部；sources.json 列出底层源文件。

| 接口 | 行为 |
| --- | --- |
| ri_decode | 根据签名解码 PNG/JPEG，等比缩小，输出白底紧凑 gray8。 |
| ri_svg_init / ri_svg_shutdown | 开启、关闭单一串行 SVG 后端；每次解码独立建立和销毁 SDK 状态。 |
| ri_svg_decode | 用原 SDK 解析、绘制 SVG，按预算调整画布及嵌图尺寸，输出 gray8。 |
| ri_bitmap_release | 使用分配时保存的回调回收输出，必须在 ACPU 执行。 |
| ri_blit | 将 gray8 图片或 A8 字形按指定行批次合成到 gray8 页面，支持裁剪、负坐标和独立 stride。 |

ri_io.read(user, resource, offset, dst, capacity, actual) 接受调用方资源键。dst=NULL、capacity=0、offset=0 表示查询总长度；普通读取允许短读，单次最多请求 4096 字节。SVG href 原样交给同一回调，EPUB 相对路径由调用方资源上下文解析。回调可以挂起协作线程并在供数后继续，算法栈保持有效。

alloc/free 管理工作内存；可选 output_alloc/output_free 必须成对设置，用于最终 gray8 及 SVG 嵌图解码的临时 gray8。分配器按实际请求字节计费，包含库的所有权头，满足 max_align_t 对齐，失败返回 NULL。输出的 allocator user 必须活到 ri_bitmap_release 之后。共享端只发布像素地址、尺寸和资源引用，不在 HCPU 调用所有权头中的函数指针。

新增的可选 available(user) 返回整个文档的实时剩余配额。它应扣除协作线程栈、控制块、输入缓存、现有图片、输出和调用方分配头。硬性限制仍由 alloc/output_alloc 执行，available 只辅助估算尺寸。文档层可直接传请求宽高，无需在 SVG 外再次按 remaining/4 缩图。

cancel 在读取、PNG 行、JPEG 输出块、SVG 阶段边界和页面合成行之间检查。一次 ThorVG 栅格化及 stb 渐进 JPEG 整帧解码内部没有新增让出点。整个库要求单一串行执行者；挂起期间的重入返回 RI_BUSY，该检查不是跨线程互斥锁。

## 实现来源与格式

当前项目 modules/common/image/raster_decode.c 的阅读位图入口支持 PNG、JPEG，src/platform/epd_svg.c 另行支持 SVG。SDK 中存在 BMP/GIF 实现，不代表现有阅读入口支持这些格式。

| 格式 | 实现与覆盖 |
| --- | --- |
| PNG | 原项目 libpng/zlib，回调式读取和分配；覆盖 RGBA、调色板透明度、1/16 位灰度、灰度 alpha、tRNS、Adam7。 |
| 基线 JPEG | 原 TJpgDec，软件灰度输出及 1/2、1/4、1/8 缩小；四个 jd_* 公共符号在 wrapper 中改名 ri_jd_*。 |
| 渐进/CMYK JPEG | 原项目使用的 SDK stb_image，STBI_ONLY_JPEG、STBI_NO_STDIO；头部重放缓存最多64 KiB，源像素最多1,048,576，维度最多2048。渐进JPEG预算不足且存在有效EXIF基线JPEG缩略图时，使用流式解码器输出该缩略图的真实像素。 |
| SVG | 项目 epd_svg 包装逻辑改为纯回调，SDK SVG tokenizer/parser/render、vector、ThorVG 源码通过 wrapper 原样包含。覆盖路径、变换、百分比根视口、渐变、虚线描边及 PNG/JPEG 嵌图。 |
| 页面合成 | 项目面积平均、77/150/29 灰度及白底 alpha 合成，加按行 gray8/A8 合成接口。 |

SVG 沿用现有包装层的支持范围：文字、嵌套 SVG、style、clip-path、mask、filter 等仍返回 RI_UNSUPPORTED；嵌图不递归解码 SVG。文本最多 256 KiB，节点最多 256，深度检查上限 32，路径属性总长度最多 32 KiB，输出上限 684×1216。gray4 打包和抖动由后续页面/pipe 层处理。

纯算法没有必须留在 HCPU 的部分。当前架构中 HCPU 提供实际存储访问、界面对象、共享结果提交和显示硬件控制；资源路径解析及图片计算可在 ACPU 完成。本库不引用 HCPU runtime API。

## SVG 内存失败恢复

image_api.c 对 SVG 调用期间的每个分配维护所有权链。分配失败时，在 SDK 收到 NULL 并解引用之前抛出 ri_svg_oom。svg_transaction.cpp 捕获异常，保留成功输出或清空失败输出，然后回收整个分配域。

C++ 栈对象先正常展开；其余 C 资源和未完成的 SDK 图结构直接按分配记录回收，不遍历残缺树。四个 ThorVG wrapper 复位初始化计数、活动加载器、软件渲染器池和单线程调度器，lv_memory.c 清空私有 lv_global。下一次解码重新初始化。

所有经过这条调用链的 C/C++ 翻译单元必须使用 -fexceptions，并且它应排在继承的 -fno-exceptions 之后。image_internal.h 与 SVG 本地配置使用 __EXCEPTIONS 检查有效配置，错误时编译立即失败。C++ 标准为 C++14，禁用 RTTI。reader_image_cxx.h 先包含标准库头，再仅对后续 SDK 声明移除 noexcept，使异常能够到达外层 catch；标准库自身的 noexcept 约定保持原样。SDK 文件没有被修改。

每次解码在进入分配域前经调用方分配器预留 1024 字节异常空间；这一分配失败时直接返回 RI_NOMEM。GCC 的异常分配入口使用该空间，不向系统 emergency pool 额外借用堆。预留区前缀为 512 字节；GCC 14.2.1 Cortex-M33 hard-float 库的 eh_alloc.o 反汇编显示实际异常头为 128 字节。单次后端只允许一个正在展开的异常，不支持嵌套/依赖异常。

本库为当前 ACPU 提供全局 new/delete 和 GCC 异常分配入口，链接中应只有一份对应实现。RT_USING_CPLUSPLUS 可以继续关闭；独立 ARM 链接没有 C++ 全局初始化表，也不依赖 RT-Thread C++ 初始化。需要链接 libstdc++、libm 和正常 libgcc/newlib，并保留 .ARM.extab/.ARM.exidx 与 __exidx_start/end；现有 ACPU 链接脚本已提供这些段。

数值适配在调用 SDK 转换器前限制整数有效位数不超过 18、指数绝对值不超过 38，避免 uint64_t 溢出及无界指数循环；超限返回 RI_INVALID。小数、正负号、科学记数法和百分比视口均保留，并有数值 SVG 用例。最终 ARM 链接没有 strtod、_Balloc、备用文件加载器或名称解码器符号。

## 动态预算及实测边界

SVG 在解析和嵌图建立后，以剩余配额减去 64 KiB、嵌图复制量及几何预留，再按每像素 4 字节 ARGB 加 1 字节 gray8 选择输出尺寸。嵌图也单独预留解码、ARGB 转换、ThorVG 图案复制及画布空间。每次 ri_svg_decode 只执行一次解码；任一阶段分配失败后，清空输出、回收本次资源并返回 RI_NOMEM，不会自动缩小尺寸重试。调用方可在之后显式发起尺寸更小的新请求。

下表列出单次解码的 GCC Linux ASan/UBSan 预算测试结果。分配器执行硬配额，预先计入 48 KiB 协作栈、4 KiB 控制块、16 KiB 输入/缓存，共 68 KiB。峰值包括工作、输出、异常存储及库所有权头；不含测试进程的宿主输入提供者和 sanitizer 元数据。

| 输入/条件 | 文档配额 | 输出 | 分配峰值 |
| --- | ---: | --- | ---: |
| 684×1216 矩形 SVG，available 开启 | 1 MiB | 319×567 | 980,368 B |
| 同图，无 available | 1 MiB | RI_NOMEM，输出清空 | 79,583 B |
| 10 组曲线/虚线描边 | 512 KiB | RI_NOMEM，随后可重用 | 521,994 B |
| 同一曲线图 | 1 MiB | RI_NOMEM，输出清空 | 1,045,911 B |

同一曲线图在 1 MiB 配额下失败后，显式发起 216×384 的新请求可成功解码；这是独立请求，不是失败请求内部的重试。

超出输入、节点、深度或几何限制时返回对应错误；边界测试每次都在同一进程立即成功重读 basic.svg，结束未释放字节数为零。MSVC ASan 同组测试也通过，受类型布局影响，峰值与 GCC 略有差别。

大幅渐进 JPEG 存在独立的固定工作集限制。684×1216 灰度渐进 JPEG 嵌图在 1 MiB、2 MiB 配额下均安全返回 RI_NOMEM；4 MiB 配额下成功输出 518×921，峰值 3,655,957 B。SDK stb_image.h:3524、3533 同时分配整帧像素平面和 short 系数平面，随后还需要输出。这个实例仅对齐后的前两块就约 2.39 MiB，缩小最终画布不能消除它们。该输入无法在 2 MiB 总堆中由现有整帧算法完成；正常 PNG、基线 JPEG 和小幅渐进 JPEG 继续支持。

当 available 提供剩余预算时，渐进JPEG在整帧分配前估算像素平面、系数平面和输出工作集。预算不足时只从有界头部缓存解析EXIF IFD1的JPEGInterchangeFormat及其长度，校验偏移和JPEG边界，并检查主图结束标记；存在有效缩略图才改走一次基线JPEG解码。预算充足时仍解码原图。缩略图输出采用其实际分辨率，例如471×837原图附带的90×160缩略图；不会用缺图标记替代。缩略图解码失败、分配失败和取消继续返回错误，不恢复分配器的失败状态，也不重新尝试原图。没有有效缩略图的超预算输入仍返回RI_NOMEM。

SDK tvgSwRle.cpp:856 的 rleRender 单帧经 ARM -fstack-usage 测得 18,200 B，包含 16,384 B Cell 缓冲。48 KiB 协作栈还需要真机水位验证；上述预算是容量模拟，不是设备栈实测。最大连续运行时间、音频共存及设备上的异常展开也需固件真机验证。

## 独立构建与 Flash 预算

epd/reader-image 包含 22 个 C 源；SVG 增加 62 个 C/C++ 源，共 84 个。所有 SVG wrapper 在包含 SDK 之前直接包含本地 reader_image_lv_conf.h，使用 LV_CONF_SKIP 和 LV_CONF_H guard，不使用需要 shell 转义的 LV_CONF_PATH。

关键配置为 LV_USE_SVG/VECTOR_GRAPHIC/THORVG/THORVG_INTERNAL/DRAW_SW/FLOAT/MATRIX=1，LV_USE_OS=LV_OS_NONE，LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM，字符串和格式化使用 CLIB，单绘制单元、32 位色深、缓冲对齐 4、stride 对齐 1。软件颜色格式仅保留 ARGB8888 与预乘 ARGB8888；最终 gray8 在适配层生成。显示、文件驱动、UI 和硬件绘图均未初始化；lv_memory.c 不调用 lv_init。

为减少 Flash，占用优化已经进入组件：

- 关闭 ThorVG 备用 SVG/Lottie 文件加载器，保留 LVGL SVG 前端及 RAW 图案。SDK 把关闭宏定义为零，但加载器使用 ifdef，因此 wrapper 在读取配置后明确 undef，并从 manifest 删除三个 Lottie 源。
- 未捕获异常仍终止执行，但去掉 verbose terminate 的类型名称解码器和 stderr 格式化；可恢复 OOM 仍正常 catch。
- 软件绘图只编译本后端使用的 ARGB 格式。
- ARM ThorVG 版本号的整数格式化使用 newlib sniprintf；stb 内部断言保留终止检查，不带 stderr 格式化。
- 两个组件均采用 -Oz。
- 数值解析通过 wrapper 调用 SDK ThorVG 自带 strToFloat，替代会在 newlib 内部申请大整数堆的 strtof。SDK SVG/ThorVG 算法源码保持不变。

GCC 14.2.1 使用 Cortex-M33、armv8-m.main+dsp+fp+cdecp1、Thumb、fpv5-sp-d16、hard-float、短枚举、短 wchar。component.elf 以 -nostartfiles 保留全部公开接口，是大小/依赖探针，不是设备启动固件。

| 公开入口独立链接 | text | data | bss | text+data |
| --- | ---: | ---: | ---: | ---: |
| PNG/JPEG/页面 | 56,508 | 1,368 | 392 | 57,876 B |
| PNG/JPEG/SVG，裁剪前 | 260,456 | 2,120 | 988 | 262,576 B |
| PNG/JPEG/SVG，当前 manifest | 188,532 | 2,104 | 976 | 190,636 B |

普通构建累计减少 71,940 B。产品清单未启用 LTO，测试脚本提供 --lto 实验入口；LTO 使用支持插件的归档器，公开入口探针按对象直接链接。独立探针包括使用到的标准库，完整固件中 libc/libm 共享会影响净节省。当前整个独立图片/SVG 包约 186.2 KiB，保留这些功能的组件裁剪不能再提供 256 KiB 的空间；整体 Flash 缺口仍须由整机链接和资源布局共同核算。

## 复现与证据

在项目根目录执行：

    python tests/reader_image_compute/fixtures.py
    python tests/reader_image_compute/run.py
    python tests/reader_image_compute/run.py --svg
    python tests/reader_image_compute/resource_audit.py
    python tests/reader_image_compute/oom_probe.py 12
    python tests/reader_image_compute/run.py --arm
    python tests/reader_image_compute/run.py --arm --svg
    python tests/reader_image_compute/unwind_audit.py

WSL Ubuntu 的同源 GCC 验证：

    wsl -d Ubuntu -- python3 /mnt/d/program/LV_EpdReder/tests/reader_image_compute/run.py --gnu --svg
    wsl -d Ubuntu -- python3 /mnt/d/program/LV_EpdReder/tests/reader_image_compute/resource_audit.py

可选体积实验：

    python tests/reader_image_compute/run.py --arm --svg --lto

fixture 生成需要 Pillow。测试、编译器临时文件、日志、映射、命令及归档全部位于 tmp/reader_image_compute。ARM/GCC host 直接使用 manifest 的 84 源；MSVC 需要补充未用 SDK 翻译单元满足其 COMDAT 链接过程，共 413 源。

已通过：10 种 PNG/JPEG 输入；32 个位图分配失败点；SVG 七类输入逐分配失败（GCC 单次解码 661 点），每个失败点均返回 RI_NOMEM、清空输出且不再发起分配，之后同进程立即成功重读；渐变清空的分配器一致性；短读、缺失/截断资源、取消、灰度/A8 分行合成；全部资源边界及预算测试。旧第 12 次分配崩溃探针现在返回 RI_NOMEM、进程退出 0。

主要证据文件为 host-svg/test.log、gnu-svg/test.log、两者的 resource-audit.json、arm-svg/component-size.txt、component.map、component-link.json、commands.json。before-code-budget 保存裁剪前独立 ELF/映射。unwind-audit.json 核对全部 84 个翻译单元的有效异常选项、关键 C/C++ 帧表和三个无栈帧尾调用包装函数；unwind.txt 保存 ARM 表。最终 ELF 无未定义符号，无 .init_array/.fini_array。SDK LVGL 源目录 1,149 个文件与初始 SHA-256 快照一致；原 epd_svg.c 及公共图片客户端没有本组件引入的改动。
