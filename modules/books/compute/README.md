# ACPU 文档转换

公共接口是 `book_compute.h` 和 `document_wire.h`。`book_compute_convert()` 保留整书同步转换入口；`book_compute_convert_at()` 支持独立 EPUB 章节、TXT 源字节切片和独立 HTML/XHTML。原 miniz、libxml2、EPUB、Markdown、MOBI 算法通过资源回调在 ACPU 执行。

## 同步 API

```c
int book_compute_convert_at(const char *path, const book_sink_t *sink,
    const book_allocator_t *allocator,
    const book_compute_resource_ops_t *resources, void *resource_user,
    const book_compute_selection_t *selection,
    const book_compute_observer_t *observer,
    char *error, size_t error_capacity);
```

`selection` 包含 `spine/anchor_byte/flags/slice_bytes/context_bytes/skip_utf8/format_hint`。`SELECT_CHAPTER` 选择 EPUB spine 或 TXT 切片，`CONTEXT` 请求前文；`format_hint=6` 明确指定独立 markup。`.xhtml/.html/.htm/.xht` 自动识别，使用同目录资源，章节索引为 0。`observer` 输出章节映射、目录和源位置。回调字符串仅在同步调用期间有效。

资源接口为 `stat/open/read_at/close/dir_open/dir_next/dir_close`。`read_at` 允许短读，0 只表示 EOF。等待缺块时在回调中挂起协作线程，恢复后继续同一调用栈。取消由 `sink.cancelled` 或资源 `BOOK_CANCELLED` 返回值触发。所有退出路径释放解析内存并关闭资源。单个解析实例只允许一个转换；重入在 I/O 前返回 `BOOK_COMPUTE_BUSY=-7`。入口由同一协作调度器串行调用，跨原生线程调用需要外部互斥。

## 章节和源定位

EPUB 每次先解析包信息和全书目录，发出全部 CHAPTER 及 METADATA TOC，再打开指定 spine 正文。其他章节正文不解压、不解析。章节稳定 ID 为规范 href，分包文件为 `zip!href`。章节可按任意顺序独立转换和缓存。BODY_UTF8 偏移是本章节规范化 TEXT 的 UTF-8 字节计数，段落和图片不增加计数。解析目标章开头以恢复样式和链接，再从目标或上下文输出到本章末尾。`anchor_byte=UINT32_MAX, CONTEXT` 请求章节末尾前文，此时在所选章内先统计正文长度，再重开该章输出末尾上下文。

TXT 使用 FILE_BYTE，即原文件字节位置。编码检测读取前 4096 字节，之后在目标位置前最多扫描 64KiB 寻找段落边界。无边界时输出 FRAGMENT；UTF-8/UTF-16 对齐字符边界，GBK 需要窗口内有可确定字符相位的单字节字符，否则返回 UNSUPPORTED。无 BOM 编码检测是启发式判断。支持 UTF-8、UTF-16LE/BE、GBK；非法码元及 NUL 替换为 U+FFFD，CR/LF/CRLF 转成段落事件。GBK 表占 47,880 字节只读数据。

持久化位置使用 `{spine, source_offset, unit, skip_utf8}`。每条选区 TEXT 前都发送真实文本起点的 SOURCE_ANCHOR，包括段落尾部和最后一块。恢复 TXT 时，从锚点解码并按规范 TEXT 的 UTF-8 字节数前进 `skip_utf8`，不计换行事件；EPUB 则将它加到章节正文偏移。落在 UTF-8 字符中间时向前对齐。TARGET 返回实际源位置，BEGIN/END 给出本次输出区间。TXT forward slice 默认及最大 64KiB 原文件字节；context 默认 12KiB、最大 64KiB，调用方可提供三页前文的源字节估计。转换器不执行分页。

MOBI/AZW/AZW3 静态编译现有 libmobi 和 `codecs/mobi/mobi_converter.c`，通过兼容端口把 vendor 的随机读取映射到资源 API。保留原压缩、正文、目录和资源算法；支持整书转换，暂不提供 MOBI 目标章节选择。加密或现有 vendor 不支持的内容仍返回 UNSUPPORTED。兼容头只用于 MOBI 翻译单元，不引入 RT-Thread/DFS 实现。

## Wire v2

组件 `epd/reader-document`，组件和 wire 版本均为 2；注册符号 `epd_document_remote_ops`。

| OPEN 字段 | 含义 |
| --- | --- |
| path | 源文件路径 |
| values[0] | wire version=2 |
| values[1] | 私有预算，0=1MiB，最大1MiB |
| values[2..3] | 图片最大宽高，0=684/1216 |
| values[4] | 目标 spine，0 起始 |
| values[5] | source_offset / anchor_byte |
| values[6] | SELECT_CHAPTER=1、CONTEXT=2 |
| values[7] | TXT forward slice 原字节数，0=64KiB |
| offset | context 字节预算，0=12KiB |
| size | skip_utf8，最大UINT32_MAX |
| flags | 0=检测；6=独立markup |

OUTPUT 由小端、4 字节对齐的记录组成，头部始终为八个 uint32：`type,bytes,a,b,c,d,length1,length2`，共32字节。载荷依次为 length1、length2 字节，无 NUL。TEXT 最多1024字节且保持完整UTF-8字符。首次约2KiB发送，随后按16KiB容量、时间预算或结束发送。

| 事件 | 标量与载荷 |
| --- | --- |
| TEXT=1 | a=style，payload1=UTF-8 |
| PARAGRAPH=2 | 无载荷 |
| ANCHOR=3 | a=level，b=有标题；payload1=id，payload2=title |
| LINK=4 | a=begin；payload1=target |
| METADATA=5, a=0 | payload1=书名，payload2=作者 |
| METADATA=5, a=1 | b=spine或UINT32_MAX，c=level，d=总章数；payload1=目标id，payload2=目录标题 |
| IMAGE_BEGIN=6 | a=图片序号，b=width，c=height，d=stride；payload1=资源id |
| IMAGE_DATA=7 | a=图片序号，b=起始行，c=行数，d=stride；payload1=gray4 |
| IMAGE_END=8 | a=图片序号 |
| CHAPTER=9 | a=spine，b=总数，c=selected，d=format；payload1=稳定id，payload2=标题或href |
| SOURCE_ANCHOR=10 | a=spine或TXT的UINT32_MAX，b=实际offset，c=OPEN原anchor，d=unit及flags；payload1=chapterid或txt-byte-N |

SOURCE unit 低8位：FILE_BYTE=1、BODY_UTF8=2。标志 BEGIN=0x100、TARGET=0x200、END=0x400、CLAMPED=0x800、FRAGMENT=0x1000。无标志锚点在每条选区 TEXT 前定义准确源起点。DONE 的 values[4..7] 分别为 selected spine、actual target、unit、END next offset，values[1] 为预算峰值。data/length 携带可选错误文本，使用公共 `epd_compute_pipe_payload` 读取。DONE 的 values[2]=`BOOK_DOCUMENT_TIMING_MAGIC`、values[3]=48 时，path 的前48字节为 `book_document_timing_t`，HCPU 用 memcpy 读取；path 此时不再是资源路径。汇总复用已有控制消息，不新增 RPC，component/wire 版本仍为2。

资源 pipe 操作 `OPEN/STAT/READ/CLOSE/DIR_OPEN/DIR_NEXT/DIR_CLOSE` 的 status 使用 BOOK_*；传输层使用 EPD_COMPUTE_*。STAT 用 size/flags，不传 struct stat。HCPU 会话负责在取消/关闭时回收剩余资源句柄，被取消的 fiber 无法再发 CLOSE。成功 DONE 才表示本次输出完整。

IMAGE 使用 `stride=(width+1)/2`；高半字节是左像素，0黑15白，奇数宽行末低半字节填白。数据块包含连续完整行。

## 内存与图像

文档默认及最大总预算1MiB，计入48KiB fiber栈、4KiB控制预留、文档状态/16KiB输出缓冲和所有动态分配管理头。解析器、受限读取缓冲、解码工作区、gray8及gray4输出使用同一硬预算。共享pipe缓冲由host管理，不计入ACPU私有堆。字体及其他组件预算由产品集成分配。

默认依赖 `epd/reader-image-svg`，同时包含 PNG/JPEG 和 SDK SVG。PNG/JPEG 压缩输入通过 book_stream/ri_io 顺序回调读取，入口只重放8字节格式探测前缀，不按整个文件大小分配；大于1MiB的输入不会仅因文件大小被文档额度拒绝。扩展JPEG解码器使用现有解码功能和有界回调，头部重放最多64KiB；渐进JPEG仍受像素、系数工作区和文档总预算限制。

SVG外部引用通过 book_resources/book_stream 读取，不需要HCPU解析。同时保留一个引用输入流，引用切换、重复引用或缩小画布重试需要倒读时，先完成当前流再重新打开；不累计完整引用图片的压缩输入。SVG XML/DOM及引用位图仍使用原有受限内存。`ri_io.available` 返回实时剩余文档预算；SVG建立DOM/引用图片后限制光栅大小，并可在输出分配失败后缩小重试。PNG/JPEG也按剩余预算预估目标尺寸并执行硬分配检查。

解码结束或拒绝图片后，完成原始流读取以取得容器原有的完整性结果，不重新打开全文复核。容器完整、且仅图片编码不支持/损坏或解码工作区不足时，输出16×16缺图标记并继续正文；ZIP CRC/解压错误、资源读取失败、取消和输出错误仍终止转换，不被占位吞掉。

SVG使用项目组件的C++异常分配域清理。经过SVG调用链的C源必须真正带 `-fexceptions`，C++使用 `-fexceptions -fno-rtti`；最终链接需要stdc++、libgcc、newlib与m。不能只把 `-fexceptions` 放入 CXXFLAGS 而让C回调保留 `-fno-exceptions`。

## 内部阶段计时

`book_timing.c` 对每次文档转换累计互斥的阶段 tick 和进入次数。默认阶段为 parse；进入解压或图像阶段时暂停上一阶段，返回时恢复。资源请求、OUTPUT 和时间片主动让出的整个 pipe exchange 单独记为 wait，包括 fiber 挂起、HCPU I/O/输出消费和等待下一次 STEP。等待没有计入前四项，嵌套解压也不会同时累加到图片/SVG。

| 字段 | 范围与计数含义 |
| --- | --- |
| parse | 格式识别、编码转换、XML/正文解析、事件组织及未归入下面阶段的转换管理/清理；每次转换计1次，不是仅XML函数耗时 |
| inflate | ZIP迭代器的既有解压、stored条目复制、CRC及释放，按读取块/结束调用计数；MOBI按压缩记录的LZ77/Huffman调用计数 |
| image | 单次图片入口的解码、缩放、gray4打包及流收尾，扣除ZIP处理和等待；PNG内部压缩解码仍归此项 |
| svg | 单次SVG入口的DOM/光栅、引用位图解码、打包及流收尾，扣除ZIP处理和等待；引用位图不再重复计入image，外围SVG文本收集归parse |
| wait | 每次pipe交换/主动让出的暂停区间，不参与计算阶段合计 |

时钟来自ACPU `rt_tick_get()`，汇总携带本核 `RT_TICK_PER_SECOND`，不与HCPU时间戳相减。前四项是扣除显式等待后的ACPU活动区间，仍可能包含中断/抢占和tick量化误差，不是硬件指令计数；各阶段不足一个tick时可能显示0。使用32位无符号差值处理一次时钟回绕，单个测量区间须小于一次完整回绕；累计时间/次数饱和时置sat=1，不继续回绕。

取时只发生在转换起止、阶段进入/退出和实际pipe让出边界，不在每字节、字形或取消轮询处新增取时。HCPU在DONE消费处复制48字节并输出一条 `[document] stages`；`parse/inflate/image/svg/wait` 的数值格式都是 `ticks/calls`，`acpu_hz`给出频率。ACPU不打印阶段汇总。关闭时才取消的fiber会完成本地累计，但不新增RPC或强制日志，取消结果/耗时沿用公共日志。正常转换只输出一条阶段汇总，清理重试不重复输出。

ARM ABI下每个活动文档新增60字节计时状态，已计入文档预算；另有4字节全局绑定指针。DONE复用path的48字节，不增加共享缓冲。HCPU只有48字节局部副本，没有新增常驻统计对象。统计不分配内存、不重新解析、不新增全文读取或校验。

## 编译与文件

完整远端清单是 `modules/books/compute.json`，根目录为 `modules/books`，共47个C翻译单元；图像库由 requires 引入。以下为完整文档源列表：

```text
compute/book_compute.c
compute/book_timing.c
compute/book_fs_callbacks.c
compute/book_compute_txt.c
compute/book_compute_slice.c
compute/book_compute_markup.c
compute/book_compute_mobi.c
compute/mobi_port.c
compute/mobi_buffer.c
compute/mobi_compression.c
compute/mobi_index.c
compute/mobi_memory.c
compute/mobi_meta.c
compute/mobi_parse_rawml.c
compute/mobi_read.c
compute/mobi_structure.c
compute/mobi_util.c
formats/formats.c
formats/book_stream.c
formats/book_xml.c
formats/book_markup.c
compute/vendor_markup.c
formats/book_markdown.c
formats/book_epub.c
third_party/libxml2/SAX.c
third_party/libxml2/SAX2.c
third_party/libxml2/entities.c
third_party/libxml2/encoding.c
third_party/libxml2/error.c
third_party/libxml2/parserInternals.c
third_party/libxml2/parser.c
third_party/libxml2/tree.c
third_party/libxml2/hash.c
third_party/libxml2/list.c
compute/vendor_xml_io.c
third_party/libxml2/xmlmemory.c
third_party/libxml2/uri.c
third_party/libxml2/valid.c
third_party/libxml2/globals.c
third_party/libxml2/threads.c
third_party/libxml2/xmlstring.c
third_party/libxml2/dict.c
third_party/libxml2/chvalid.c
third_party/libxml2/HTMLparser.c
third_party/libxml2/HTMLtree.c
third_party/miniz/miniz.c
compute/document_remote.c
```

Include目录：

```text
modules/books/compute
modules/books/compute/mobi_include
modules/books/codecs/mobi
modules/books/third_party/libmobi/src
modules/books/formats
modules/books/third_party/libxml2
modules/books/third_party/libxml2/include
components/compute/include
components/compute_reader_image/include
```

宏：`BOOK_FORMAT_ACPU`、`LIBXML_STATIC`，架构集成提供 `CORE_ACPU`。MOBI wrapper内部局部定义 BOOK_MOBI/RT_USING_DFS/HAVE_STRDUP，关闭vendor调试；这些宏不进入其他翻译单元。miniz自带只读ZIP/无原生文件和malloc配置。

`vendor_xml_io.c` 替代原xmlIO.c，`vendor_markup.c` 包含原book_svg_markup.c并让旧libxml的HTML探测完成当前有界输入块再切换解析器，避免SAX回调中stop后访问失效输入。不要同时编译被替代源。`book_fs_callbacks.c` 替代原book_fs.c。纯转换清单 `sources.py:configuration()` 返回 sources/includes/defines/force_includes，不含remote和图像依赖；原libxml源的force_include必须应用。

HCPU仅需 `formats/formats_info.c` 提供detect/name。定义 BOOK_FORMAT_TARGET 使用已有storage锁与短stat/readdir，不依赖parser、bf allocator或libxml。ACPU及旧对照路径通过formats.c内嵌复用该文件，因此编译formats.c时不再独立编译formats_info.c。`book_file_version/valid` 已位于轻量 `modules/books/file_version.h`，`storage_file_signature` 来自现有 `src/services/storage_file.c`。

新增适配公共/私有头：book_compute.h、book_compute_internal.h、document_wire.h、book_timing.h、document_timing_port.h、book_compute_xml_port.h、mobi_compute_port.h、mobi_include/dfs_file.h、mobi_include/rtthread.h、gbk_table.h。配置与清单为 compute.json、compute/sources.py；转换源文件清单以 sources.py 为准。

## 验证

```text
python3 -B tests/reader_document_compute/run.py
python3 -B tests/reader_document_compute/selection_run.py
python3 -B tests/reader_document_compute/remote_run.py --real-image
python -B tests/reader_document_compute/arm_compile.py
python -B tests/reader_document_compute/hcpu_run.py
python -B tests/reader_document_compute/cache_v3_run.py
```

流式图片与内部阶段计时使用 `tests/reader_document_images/run.py` 的23个真实入口回归（ASan/UBSan）及独立计时单测。`tests/reader_document_images/timing_hcpu.py` 验证生产HCPU的DONE消费，`tests/reader_document_images/arm_compile.py` 按已保存的生产参数检查六个相关ARM单元、统计状态大小和对象增量。输入生成、运行命令及资源峰值口径见该测试目录README。

前三项在Linux/WSL运行。run.py对比原文件后端与回调后端的完整EPUB/分包/Markdown事件；`--epub PATH`可加入已有样本。它还覆盖TXT转码、独立HTML/XHTML、MOBI、短读、取消、重入和逐分配失败。selection_run.py验证只读选章、全书目录、四种TXT编码的精确锚点/skip、末尾块、64KiB回扫范围及章节末尾前文。remote_run.py使用保持调用栈的host fiber、实际pipe及图像库，验证批处理、暂停、取消、预算、资源清理及v2选区协议。ARM检查按manifest编译全部文档源并审计不存在原生文件访问符号；它不代替完整固件链接和真机性能验证。

`hcpu_run.py` 在Windows运行，通过WSL实际转换器生成EPUB、UTF-16、GBK的v2事件，再用MSVC编译当前生产document.c并回放事件。测试实际spool持久化、TOC/源锚点重建、pending异步读取、UI查询无文件I/O、缓存重开不重复转换、取消与内存清理；cache身份和平台服务使用host替身。自定义资源测试验证原始stream的STAT/OPEN/READ/CLOSE。测试只编译和调用生产文件，不修改document.c。

宿主worker将host、pipe、资源句柄、输出文件和缓存观察状态保留到转换结束。每次转换推进最多8次pipe交互，在交互之间检查4ms预算；单次同步I/O、准入等待和其他worker工作不受这个时间预算硬性限制。最近打开并持有的目标优先于邻章预取；新的前台目标可取消尚未完成的旧目标。旧pipe关闭成功之前不启动第二个解析器，关闭失败时保留会话及资源供后续worker重试。

调用 `document_prefetch_enable(d, false)` 后，转换先推进到请求位置可读，随后仅在前台读取需要更多记录时继续。无读取需求时保留当前pipe和fiber，worker返回空闲；恢复不重新解析章节。应用挂起由 `reader.h` 的纯内存接口 `epd_reader_suspended()` 提供，此时暂停转换、正常记录/图片读取、索引恢复和预取，取消、关闭及已排队保存仍可完成。恢复时调用现有prefetch接口即可唤醒worker，即使prefetch参数未变也会检测挂起状态变化。

图像使用转换阶段已经生成的GRAY4数据，经reader-render缩放，不再打开document转换会话。render关闭失败时，document同时保留render句柄、packed输入及frame输出；后续worker独立重试关闭，成功后才释放缓冲。闲置的hash客户端同样由worker重试清理，不依赖下一次图片请求或关闭文档。

`hcpu_run.py`还验证索引的队列快照、磁盘/异步恢复和损坏拒绝。外层index v3为24字节header，HCPU只验证该头及长度，不再对完整索引负载重复计算FNV；inner index v2保留一次ACPU负载验证。图像使用实际render kernel。等待对象与传输客户端使用可注入取消/关闭失败的替身，验证取消OPEN、取消单个图像请求、保留图像缓冲区、哈希任务清理、服务关闭重试和分步调度。重复打开与邻章发布测试在分配点插入另一调用，检查二次查重和双向链接。真实EPUB两章事件验证旧章尚未完成时切换到目标章。缓存身份服务使用替身，固件调度与真机延迟需单独验证。

生命周期回归还覆盖输入错误、ACPU内存不足/不可用、资源移除、源主动取消、快速切书、恢复目录时内存不足、文档链发布和服务关闭期间保存任务的引用寿命。暂停回归验证无新准入/STEP/文件读取、同一解析上下文恢复、暂停时仍可取消和清理；图片回归验证章节转换尚未完成时可完成渲染且没有第二次document OPEN。文档缓存元数据使用独立frame v3（dc4前缀），由reading-data在ACPU校验结构并计算FNV，旧frame不复用。负载上限64KiB、依赖上限512项；超限仅禁用持久复用，当前正文及临时spool继续可读。文件读写每4KiB释放存储锁，依赖版本逐项短锁检查，最终以两卷代数和卡会话复核后调用storage_file_commit替换。

阶段日志以 `id` 关联同一文档：`convert_open`记录chapter/anchor/skip/budget，`first_readable`记录首次可读位置，`convert_done`记录转换阶段结果及steps/slices/read_bytes/output_bytes，`terminal`记录文档终态result/error/end/cancelled。`flow`记录prefetch/suspended；`restore_done`记录恢复结果及scanned/end；`cleanup_pending`的kind为1（pipe）、2（render）、4（hash），`cleanup_done`表示该文档清理完成。除flow外，上述阶段均记录elapsed_ms。

测试源码清单：`run.py`、`host_test.c`、`info_test.c`、`selection_run.py`、`selection_cases.h`、`remote_run.py`、`remote_host.c`、`hcpu_run.py`、`hcpu_test.c`、`hcpu_host.h`、`hcpu_compute_stubs.h`、`hcpu_lifecycle.h`、`hcpu_scheduler.h`、`hcpu_audit.h`、`hcpu_shutdown_source.h`、`cache_v3_run.py`、`cache_v3_tests.h`、`cache_v3_io.h`、`arm_compile.py`、`generate_gbk.py`。

Markdown/MOBI的保存位置使用 `DOCUMENT_SOURCE_SPOOL=3`：chapter为0，offset为当前转换版本的spool记录边界，skip_utf8为该记录内UTF-8字节位置。缓存命中直接访问该记录；缓存缺失仍由ACPU转换，在记录发布后恢复，不进行HCPU正文解析或从头排版。此坐标要求相同源身份和DOCUMENT_CONVERSION_VERSION，序列化规则变化时须更新版本。TXT/EPUB继续使用原文件/正文UTF-8源锚点。

`document_worker_shutdown`只有在所有document及缓存计算客户端清理完成后才返回。CLOSE失败时保留worker、会话和借用缓冲，先短暂重试再以250ms间隔继续；永久失败不会让worker退出或将仍有远端所有者的资源交给模块卸载。`shutdown_wait`标识等待，`shutdown_done`记录重试次数。
