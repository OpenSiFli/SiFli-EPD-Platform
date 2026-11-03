# 阅读字体计算组件

`epd/reader-font` 提供独立字体上下文、批量字符测量和 A8 字形栅格化。核心算法直接包含仓库内 `SiFli-SDK/external/lvgl_v9/src/libs/tiny_ttf/stb_truetype_htcw.h`，编译与运行均不依赖 LVGL。组件支持带 `glyf` 轮廓的 TTF/TTC，字体文件最大 32 MiB；CFF 字体返回不支持错误。字体提供方应提供经过导入验证的、会话期间内容不变的字体文件。

## 算法接口

公共头文件为 `include/compute_reader_font.h`，实现文件为 `compute_reader_font.c`。

1. `crf_config_init()` 设置默认配置，调用方填入 `read_at`、`alloc`、`free` 和可选取消回调。
2. `crf_create()` 分配私有上下文和有界输入缓存。
3. `crf_open()` 绑定字体身份并解析字体目录。身份包含 `source_id`、`signature`、`generation`、`file_size`、`face_index`，由内容提供方生成；组件不扫描整字体来生成签名。
4. `crf_batch()` 接收最多 1024 个 `crf_codepoint` 字符对。`render=0` 测量，`render=1` 同时输出 A8。`crf_measure_pair()` 是单字符对测量入口。
5. `crf_destroy()` 释放组件拥有的全部内存。输入、结果数组和像素缓冲区始终由调用方持有。

一个上下文绑定一个字体文件/face，字号及样式随批次传入。同一上下文的调用必须串行；多个上下文的读取位置、缓存、分配链和错误状态彼此独立。回调可挂起当前协作线程，但在恢复前必须保持上下文、输入数组、输出缓冲区和 callback 参数有效，不能重入或销毁正在执行的上下文。

`read_at` 是唯一字节来源。每次请求对齐到 4096 字节块，长度不超过 4096，文件尾块按剩余字节请求。成功必须填满请求长度。回调可以在缺块时保存当前线程栈、等待供数、恢复后返回成功；stb 从原调用位置继续。若回调直接返回 `CRF_NEED_DATA`，本次操作退出并报告 `wait_offset/wait_length`，组件不重试、不重启解析。取消通过回调在流读写、缓存命中、分配、栅格扫描行和样式变换边界检查；检查间隔取决于当前 stb 运算，不承诺固定毫秒延迟。

字体撤销时，提供方必须取消该上下文的作业。已命中的私有缓存不会重新访问提供方；因此仅修改文件内容而保持身份和取消状态不变，不会使缓存失效。重新打开字体会清空全部缓存。

## 尺寸与像素约定

`crf_style` 共 8 字节：`size_px` 是 16 位 em 像素字号，范围 1..256；`weight`、`kerning`、`oblique` 各为一个字节；末尾三个保留字节必须为零。

- `weight=0` 保持原始灰度；`weight=1` 采用相邻左像素最大值加粗，非空字形的 advance 增加 1；`weight=2` 按现有阅读字体规则做半像素水平收细。
- `kerning=1` 使用下一 codepoint 的字偶距，advance 按 `scale*(advance+kern)+0.5` 截断，与 TinyTTF 一致；`kerning=0` 仅使用字形自身 advance。
- `oblique=1` 在字重处理后右移每行像素，位移为 `(box_h-1-row)/4`。`box_w` 和 advance 增加 `(box_h-1)/4`，offset 不变。

`crf_glyph` 共 44 字节。`box_w/box_h` 包含 TinyTTF 的右、下各一像素边界，`ofs_x=x_min`，`ofs_y=-y_max`。位图是逐行排列的 8 位灰度透明度，0 透明、255 完全覆盖，`stride=align4(box_w)`，padding 为零。位图左上位置使用 TinyTTF 的 offset 语义；行高和基线由 `crf_result.metrics.line_height/base_line` 提供。

`bitmap_offset` 相对于调用方像素缓冲区，`bitmap_size=stride*box_h`。仅测量时 `bitmap_offset=CRF_NO_BITMAP`、`bitmap_size=0`，尺寸和 stride 仍然有效。缺字输出 `CRF_GLYPH_MISSING`，不使用 `.notdef`、不自动选择其它字体；控制字符 `<0x20`、`U+F8FF`、`U+200C` 输出 `CRF_GLYPH_CONTROL` 和零尺寸。缺字本身不是整个批次的错误。

`completed` 和 `bitmap_bytes` 指定已提交结果前缀。出错的当前字形不会提交 descriptor；像素缓冲区未提交的尾部可能已被写入，调用方不得使用。输出不足时，`required_bytes` 表示包含当前字形所需的总像素容量。调用方可从 `input+completed` 开始下一批，新批次的像素 offset 重新从零计算。

## 内存边界

纯算法默认总分配额度 256 KiB，配置硬上限 2 MiB。额度包括上下文、输入缓存、组件分配头及全部 stb 临时分配；底层 allocator 自身的块头、线程栈和调用方输入/输出缓冲区另计。输入缓存默认 16 KiB，可配置为 1..64 个 4 KiB 块（最大 256 KiB），采用 LRU；上下文内还有固定 256 条字符对测量缓存。组件不常驻整个字体，也不常驻整批字形位图。`crf_get_stats()` 返回分配当前值及峰值。

单字形最终 box 最大 512×512，单字形像素最多 256 KiB。每次打开及每个字形最多进行配置数量的流 seek/read 检查，默认及硬上限为 1048576。本组件在 `compute_reader_font.c` 中配置轮廓预算：4096 个点、256 个轮廓、8192 个顶点、16 层复合字形及 256 个组件。SDK 对这些可选预算默认不设上限；本组件通过 `STBTT_MAX_GLYPH_*` 宏启用自己的限制。纯算法不创建线程；其栈由执行方提供。

FTZ1 来源另在会话总额度内扣除 96 KiB 解压块缓存、16 KiB 输入片段、块索引及 inflater 状态。解压缓存容纳六个 16 KiB 块或三个 32 KiB 块，采用 LRU。每个压缩片段读取前检查取消和调度让出，块完成时验证 zlib/Adler32、输入消耗及精确输出长度；未完成或错误的块不能进入缓存。原始 TTF 来源不分配这部分工作区。

remote 默认每会话总额度 704 KiB，并以 704 KiB 为可配置上限。该额度先扣除 remote 状态（含 16 KiB OUTPUT 暂存、64 项输入和 descriptor）、16 KiB fiber 栈、4 KiB 公共 fiber 控制结构预留，余量才交给纯算法。公共 allocator 的底层管理开销与共享 pipe 缓冲区另计；所有会话的总预算由调度层统一管理。4 KiB 预留对应当前公共 fiber 的线程、信号量及控制字段，公共 fiber 扩展时需重新核对。字体 session 按文件/face 复用，避免为不同字号重复创建输入缓存。平台外置字体为 ACPU 算法配置 256 KiB 输入缓存，内置字体保持 16 KiB；HCPU 外置源块缓存单源上限 256 KiB、总上限 512 KiB，按 4 KiB 分块，元数据保留仍给普通读取留下至少八个块的位置。HCPU 源缓存不计入 ACPU 每会话 704 KiB 的额度。

## 错误类型

| 状态 | 含义与后续行为 |
| --- | --- |
| `CRF_INVALID_ARGUMENT` | 参数、Unicode 标量、样式或容量描述不合法。 |
| `CRF_NOT_OPEN` / `CRF_BUSY` | 上下文尚未成功打开，或同一上下文正在执行。 |
| `CRF_NEED_DATA` | 同步回调未供数；返回精确块范围，不自动重试。 |
| `CRF_IO_ERROR` / `CRF_SHORT_READ` | 提供方错误或成功读取长度不足；算法上下文失效。 |
| `CRF_STALE_SOURCE` | 预期身份不匹配或提供方撤销字体。提供方返回此错误时上下文失效；仅请求身份不匹配不改变原上下文。 |
| `CRF_INVALID_FONT` / `CRF_UNSUPPORTED_FONT` | 字体结构非法或轮廓/cmap 不受支持。 |
| `CRF_NO_MEMORY` | 达到额度或 allocator 失败；临时分配回收，后续批次可重新尝试。 |
| `CRF_OUTPUT_FULL` | descriptor 或像素容量不足，有效前缀仍可消费。 |
| `CRF_CANCELLED` | 取消检查或提供方取消；未完成字形不提交。 |
| `CRF_LIMIT` | 字形尺寸或流操作数超出边界。 |

## Pipe 协议

字体协议及组件版本为3，两端注册表需要使用同一版本。协议定义在 `include/font_wire.h`；组件 manifest 为 `compute.json`；远端入口为 `epd_reader_font_remote_ops`，实现位于 `font_remote.c`。所有 wire 字段使用固定宽度整数、小端布局；私有 context 不跨核传递。

| 数据 | 布局 |
| --- | --- |
| OPEN | 56 字节 `epd_font_open_t`，包含字体身份、HCPU handle、内存/缓存/操作数配置、来源长度和可选内置源地址；0 配置值选默认。handle 必须非零。 |
| COMMAND | 52 字节 `epd_font_command_t` + 最多 64 个 8 字节字符对；操作为 MEASURE、RENDER 或 END。 |
| OUTPUT | 88 字节 `epd_font_output_t` + `completed` 个 44 字节 descriptor + A8 区，整个包不超过 16 KiB。 |

初始 pipe message 使用 `EPD_PIPE_OPEN`。字体成功打开后发送 `OPEN|FINAL` OUTPUT，`request_id=0`；此时没有指定字号，metrics 为零。HCPU 发送带 style、`count=0` 的 MEASURE 即可获得该字号的行高和基线。后续 COMMAND 使用非零 `request_id`、完整字体身份及样式。每个 OUTPUT 的 `first` 指向原命令中的首项，FINAL 标记该命令结束；同一字体会话可连续处理多个字号和多个批次。

OUTPUT descriptor 的 `bitmap_offset` 相对于包尾 A8 区，而不是整包。批次自动按字形拆成多个 OUTPUT；单个字形不能跨包分割，若单个字形超过包容量，则最终 OUTPUT 返回 `CRF_OUTPUT_FULL`，`required_bytes` 给出该字形所需 A8 字节数。正常包的 `required_bytes` 无需使用。

READ 请求通过 `EPD_PIPE_READ` 挂起算法栈，由 HCPU 提供源字节。协议要求 HCPU 回显 handle、offset、size、身份和字体协议版本；成功返回的 length 必须等于请求。`values[0]` 是字体协议版本，`values[1..5]` 是字体身份，`values[6]` 是 `crf_status`，`values[7]=0`；公共 pipe 的 `message.status` 仍使用 `epd_compute_status_t`。原始TTF使用 `flags=0`，`source_bytes=0` 或等于 `font.file_size`。FTZ1使用 `flags=EPD_FONT_SOURCE_FTZ1`，`source_bytes` 是完整压缩包的字节数，READ 的 offset/size 对应压缩源，HCPU直接复制这些字节；解压由ACPU完成。`font.file_size` 始终是包首字体的逻辑长度，FTZ1中后续emoji数据不属于该字体的可读范围。

COMMAND、OUTPUT 同样携带 `values[0]=EPD_FONT_VERSION`。响应必须保留请求 operation 和 sequence；OUTPUT ACK 保留 length。公共 pipe 校验自身版本和 response_sequence，字体组件另行校验字体协议版本、身份和操作。HCPU 在收到 COMMAND 后应先通过 `epd_compute_pipe_command()` 提交命令，再继续 STEP。

取消通过公共 pipe 的 cancel/shutdown 进行。shutdown 成功后清空句柄，失败时保留句柄和缓冲，调用方必须保留它们直到后续清理成功。远端 close 恢复挂起的 fiber，使 stb 分配链和引擎上下文正常回收，然后释放 fiber 与 remote 状态。HCPU 应在 close 完成后释放字体 handle/lease；remote 不打开或关闭 HCPU 文件。读取失败或字体撤销会发送错误 OUTPUT 并终止字体会话，协议错误直接终止；不会调用 HCPU 栅格化 fallback。

SF32LB57 上的固件内置 FTZ 可以通过 `source_address` 提供 HCPU 资源分区中的只读地址。ACPU 在 OPEN 时检查来源类型及分区边界，按 SDK 的 CBUS→SBUS 地址转换读取，不再为每个压缩块请求 HCPU 搬运。外置字体及没有提供映射地址的来源继续使用 READ；字体会话仍通过同一组件和 pipe 调度、取消及释放。

## SDK 宏接入

`STBTT_STREAM_TYPE/SEEK/READ` 映射到上下文与 `read_at`；`STBTT_STREAM_ERROR/FAIL` 传播解析错误；`STBTT_USERDATA_ERROR` 传播分配失败、取消及扫描行检查；`STBTT_malloc/free` 映射到有总额限制的可注入 allocator。实现使用 `STBTT_STATIC` 隔离符号，`STBTT_GLYF_ONLY` 限制轮廓类型，并与 SDK TinyTTF 一样使用 50/20/10 的内部 heap factor。
