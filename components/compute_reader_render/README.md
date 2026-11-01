# 阅读器软件合成组件

`epd/reader-render` 版本 3 在 ACPU 上直接生成 RGB565 或 L8 帧，执行白底初始化、A8 文本混合、GRAY4 图像合成、RGB565 图像复制、最近邻缩放和实心矩形绘制。组件通过现有 `epd_compute` 的 OPEN/STEP/CLOSE 接口运行，不创建 fiber。每次 STEP 最多处理 32 个相交的目标行，在每行开始前检查取消，并仅同步本批帧和源数据的缓存窗口。

## 接口与构建

| 文件 | 用途 |
| --- | --- |
| `include/reader_render.h` | 无平台依赖的格式、命令和纯内核 API。 |
| `include/render_wire.h` | 固定宽度 wire 结构、组件名及 remote ops 声明。 |
| `include/epd_reader_render.h` | HCPU worker 专用同步客户端。 |
| `render_kernel.c` | 直接操作借用 frame 的像素内核。 |
| `render_format.c` | 命令初始化、格式校验、裁剪范围和缓存窗口计算。 |
| `render_remote.c` | OPEN 绑定 frame，STEP 校验请求并推进不超过 32 行。 |
| `render_client.c` | 共享缓冲区管理、逐 STEP 执行、结果校验和取消。 |

`compute.json` 导出 `epd_reader_render_remote_ops`，支持 foreground/background。项目 manifest loader 支持 `hcpu_app` 和 `hcpu_firmware`，两者均编入 `render_client.c` 与 `render_format.c`，固件额外编入 `render_image.c` 并导出 L8 图像初始化接口；ACPU 编入 remote、格式和像素内核。固件客户端通过 RTM 导出客户端 API 和轻量命令初始化函数 `rr_command_init`，动态模块构建使用 `BUILD_DLMODULE` 避免重复导出。客户端没有本地像素 fallback。

## Frame 与源数据

`rr_frame.format` 显式选择目标格式：`RR_FORMAT_RGB565=0` 为小端 RGB565，`RR_FORMAT_L8=1` 为单字节不透明亮度，0 黑、255 白。默认零初始化仍使用 RGB565。`stride` 单位是字节，必须满足 `stride % 4 == 0` 且不小于有效行字节数；L8 可取 `(width+3)&~3u`，RGB565 可取 `(width*2+3)&~3u`。宽高范围为 1..8192，`stride*height` 最大 32 MiB。INIT 把有效 frame 的全部字节设为 `0xff`，包含行 padding；其它命令只修改被裁剪后的像素，保留 padding。

HCPU 传入实际拥有完整容量的 `epd_compute_buffer_t`。共享地址和容量按公共协议要求对齐到 32 字节，容量至少覆盖 `stride*height`，应由 `epd_compute_buffer_alloc()` 分配，或由调用方提供等价的实际对齐分配。客户端独占借用整个 frame，不复制、不释放主 frame；并发会话使用互不重叠的 frame。frame 在会话 CLOSE 成功前一直保持有效。FINISH 成功且命令返回后，其它线程可以只读消费完整像素，无需等待 CLOSE；再次 INIT 前必须结束所有读取。

A8 为逐行 8 位 alpha，0 透明、255 黑色完全覆盖。`src_stride >= src_width`，整个源输入 `src_stride*src_height` 最大 16 KiB。默认使用已有 frame 作为底色，RGB565 的每个分量或 L8 的亮度执行 `round(component*(255-alpha)/255)`，具体整数公式是 `(component*(255-alpha)+127)/255`。INIT 后自然得到白底黑字；`RR_A8_WHITE_BG` 可显式使字形矩形内每个像素都以白色作为底色。

GRAY4 的 0 表示黑色、15 表示白色，先乘 17 转为灰度，按目标格式直接写入 L8 或转换成 RGB565。默认按行排列，每行左像素占高半字节，`src_stride >= ceil(src_width/2)`；奇数宽度的行尾低半字节和额外 padding 不参与绘制。`RR_GRAY4_CONTIGUOUS` 表示整图连续打包、`src_stride=0`，像素索引为 `y*src_width+x`。这种布局与现有 document 图像缓存一致，奇数宽度时相邻两行可能共用一个字节。

RGB565 源为小端像素，`src_stride >= src_width*2`，每行可以有额外 padding。`RR_BLIT_RGB565` 的 flags 为零，按裁剪后的范围逐行复制像素字节，RGB565 目标保留原始颜色；L8 目标直接写入灰度，不分配 RGB565 中间帧。RGB565 分量先按位复制扩展到 8 位，再计算 `(77*r+150*g+29*b+128)>>8`，矩形颜色采用相同转换。源与目标仍须分离。

## 命令语义

`rr_command_init(&command, operation, frame_format)` 初始化版本、结构大小和覆盖整个 frame 的默认裁剪矩形。`x/y` 是目标矩形左上角，可为负数；`width/height` 是目标矩形大小。`clip_x/clip_y/clip_w/clip_h` 是 frame 坐标中的额外裁剪矩形，最终范围取目标、clip 和 frame 的交集。零面积裁剪不修改像素。

| 命令 | 行为 |
| --- | --- |
| `RR_INIT` | 分行清空整个 frame 为白色，开始新一轮合成。忽略目标矩形和 clip。 |
| `RR_BLIT_A8` | 从源左上角取 `width*height` 范围，按 alpha 绘制黑色文本。尺寸不得超过源尺寸。 |
| `RR_BLIT_GRAY4` | 从源左上角取对应范围，以 GRAY4 灰度覆盖 frame。 |
| `RR_SCALE_GRAY4_TO_RGB565` | 将完整 GRAY4 源缩放到目标矩形，使用 `sx=floor((x-dst_x)*src_width/dst_width)`，y 同理。裁剪后仍按原目标矩形计算采样点。 |
| `RR_FILLRECT` | 用 `fill_rgb565` 填充目标矩形，黑色值为 0，可绘制下划线和删除线。 |
| `RR_BLIT_RGB565` | 操作编号 7，从源左上角取 `width*height` 范围，按目标格式复制颜色或转换成 L8。尺寸不得超过源尺寸。 |
| `RR_FINISH` | 确认 INIT 已完成并封存当前帧，不执行像素转换。 |

FINISH 后只接受新的 INIT。空白字形可由调用方跳过；带源命令即使目标完全在 clip 外，也必须提供有效的源尺寸和长度。INIT/FINISH/FILLRECT 的源字段应为零。未定义的 flags、非法版本、长度不符、源与 frame 重叠、整数溢出或越界范围均返回错误。

## HCPU worker 客户端

客户端同步等待远端执行，所有 open/command/close 调用都应在文档 worker 上串行完成。图片加载等需要等待计算资源的任务使用 `epd_reader_render_open_wait(frame, format, purpose, wait, &render)`。该接口直接调用公共调度器的排队准入，冲突会话释放后按准入顺序继续，不轮询 BUSY、不重复 OPEN。等待不占用 ACPU 私有 render 状态；HCPU 保留客户端、128 字节 control、16 KiB 输入暂存区和借用 frame。没有固定等待期限；调用方通过取消句柄结束等待。原 `epd_reader_render_open()` 保持非阻塞语义，冲突时返回 `EPD_COMPUTE_BUSY`，适用于已有 pending 调度的 worker。

| 阶段 | 调用方与客户端的责任 |
| --- | --- |
| 开始排队前 | 调用方执行 `epd_compute_wait_create(&wait)`，在任务状态中发布 wait，供取消所有者访问，再调用 `epd_reader_render_open_wait(...)`。每个 wait 同时服务一个 OPEN。 |
| 排队或远端 OPEN 中 | 取消所有者调用 `epd_compute_wait_cancel(wait)`。取消是粘性的，创建后即取消也有效。单独改变 `cancelled(user)` 的返回值不会唤醒排队中的 OPEN。 |
| OPEN 返回 | 失败且 `render == NULL` 时，客户端私有分配已回收；失败且非空表示补救 CLOSE 失败，必须保留 render 和 frame，随后重试 shutdown。成功时在任务锁保护下把取消目标切换到 render，同时检查任务是否已取消；若已取消，调用 `epd_reader_render_cancel(render)`。 |
| 释放 wait | 确认 OPEN 已返回且所有 wait 的取消调用者已退出后，执行 `epd_compute_wait_destroy(wait)`。取消过的 wait 不用于新任务。 |
| 绘制 | 顺序调用 INIT、BLIT/FILLRECT/SCALE、FINISH；检查每条命令状态。 |
| 结束 | 命令返回、取消调用者退出后执行 `epd_reader_render_shutdown(&render)`。只有成功时 render 被置空，调用方才可以释放 frame。FINISH 成功后可以提前读取完整帧，但必须保留其存储直到 CLOSE 成功。 |

共享任务状态应持有 render 和 frame 的所有权，不能仅把 render 存放在即将返回的函数局部变量中。任务锁用于发布或撤下取消目标，不应在执行阻塞的 OPEN/command/shutdown 时一直持有。

`epd_reader_render_command()` 把不超过 16 KiB 的源复制到客户端共享暂存区，适合 glyph 和小图层。客户端填写 version/bytes/id，再连续提交最多 32 行的 STEP，直至完成、取消或出错；每步验证响应版本、命令 id、进度和行数，不接受无进展的 MORE。

`epd_reader_render_command_shared()` 借用不可变共享源，支持大于 16 KiB 的 GRAY4 或 RGB565 图像，源总长最大 32 MiB；该函数不会复制整幅源图。A8 在共享模式下仍限制为 16 KiB。源共享缓冲区必须与 frame/control 分离，并在同步命令返回前保持有效、不可修改。

### 字形批处理

`epd_reader_render_a8_enqueue(render, command, pixels, bytes)` 将一个 A8 字形复制进已有暂存区，不执行跨核调用。返回 `EPD_COMPUTE_OK` 表示已接收，调用方可以立即释放字形；返回 `EPD_COMPUTE_MORE` 表示尚未接收，应先提交已有批次再尝试。空批次仍返回 MORE 时，使用原 `command()` 接口绘制这个字形。

`epd_reader_render_a8_flush(render, cancelled, user)` 同步提交已有字形，空批次直接成功。每批最多 8 个字形，单个目标高度最多 64 行，命令与位图合计不超过原有 16 KiB 暂存区。ACPU 复用原像素内核及逐行取消检查；一次批次执行结束后归还公共调度器。取消或传输失败后，部分帧不能提交显示，按原 shutdown 流程回收。

调用方在图片、矩形、清屏、FINISH 之前，以及本轮工作结束或让步之前调用 flush。存在未提交字形时，原 command 接口返回 BUSY，防止覆盖暂存位图或改变绘制顺序。enqueue 成功后推进字节游标；MORE 时保持游标不变。flush 失败后不重放该批次。

批次通过 opcode `RR_BATCH_A8` 使用原 STEP 布局，`command.width` 为字形数，`command.src_width` 为暂存长度。暂存区前 544 字节为 8 个命令槽，随后依次存放已使用槽对应的 A8 位图；子命令 ID 连续递增并跳过零。frame region 是所有目标像素的一个对齐缓存包络，source ref 长度是有效暂存长度，source region 容量向上对齐至 32 字节。批次结果的 `rows_*` 字段表示完成字形数，外层 ID 回显首个子命令 ID；原逐行命令的结果语义保持不变。批次只由客户端及 remote 处理，不直接传给 `rr_kernel_step()`。

使用 `command()` 分块复制 RGB565 时，源指针指向当前块的首行，`src_height=height=当前块行数`，`y` 为该块的目标纵坐标，提交长度为 `src_stride*src_height`。每块长度必须不超过 16 KiB。分块调用与共享整图调用使用同一颜色和裁剪规则。

每个命令可提供 `cancelled(user)` 回调，客户端与公共调度器在提交前检查。另一个取消所有者可在会话存活期间调用 `epd_reader_render_cancel()`，通过公共 signal 取消正在执行的 STEP。该所有者必须与 CLOSE 协调生命周期。`epd_reader_render_set_purpose()` 调整已有会话的前后台用途。

已提交的命令执行失败或开始关闭后，客户端不再接受绘制命令，部分 frame 不应交给 UI；提交前的参数校验失败不改变会话。`epd_reader_render_shutdown(&render)` 先发出取消，再尝试一次 CLOSE；成功后释放客户端私有资源并置空 render，重复传入空句柄仍返回成功。失败时原样返回公共错误码（例如 BUSY 或 UNAVAILABLE），保留同一个 render 和私有缓冲区，不强制释放，也不循环重试。原 `epd_reader_render_close(render)` 同样保留失败时的句柄，但不会替调用方置空。

关闭失败时，调用方保存 render、frame 及尚未归还的借用资源作为待清理记录，分别记录绘制状态与关闭状态，在后续 worker 调度中重试 shutdown。只保留 buffer、丢弃 render 会导致无法完成会话回收。显式 CLOSE 成功前，借用 frame 必须持续有效；共享 source 的借用期限是对应同步 command 返回。持续的传输故障需要公共 compute 层提供恢复保证，本客户端不能证明远端停止后自行释放 frame。

## 纯内核与 wire

纯内核先调用 `rr_kernel_bind()`，之后重复 `rr_kernel_step()`。一个正在执行的命令跨 MORE 必须保持命令所有字段、源指针和源长度不变。命令 id 非零，首条可选任意 id，后续按 32 位加一递增，溢出跳过零。`rr_result` 返回版本、大小、id、状态、已完成行数、总行数和本 STEP 行数。

`RR_OK` 表示命令完成，`RR_MORE` 表示继续同一命令，`RR_INVALID` 表示参数或续传不匹配，`RR_BAD_STATE` 表示合成阶段不允许该操作。`RR_CANCELLED` 会使部分 frame 进入不可提交状态；纯内核可通过下一 id 的 INIT 恢复。remote 将这些状态映射为公共 OK/MORE/CANCELLED/INVALID，详细内核状态保存在结果中。

render v3 在 OPEN 中新增显式 frame 格式，客户端与远端必须同时使用 v3；公共 compute 协议及缓存窗口语义不变。OPEN 参数 `rr_open_wire` 为 36 字节，STEP 参数 `rr_command_wire` 为 92 字节，从 control 偏移 0 开始；28 字节 `rr_result` 位于偏移 96。

OPEN 使用 control(0)、完整 frame(1)、输入暂存区(2)，frame ref 长度为 `stride*height`，绑定 frame 和 control 地址。STEP 使用 control(0)，随后按需追加 frame 窗口和 source 窗口。每个窗口的 region 地址和容量都按 32 字节对齐，ref 从偏移 0 开始，长度等于窗口容量；缺省 ref 为 `{EPD_COMPUTE_REGION_NONE,0,0}`。frame/control 具有 READ|WRITE 权限，source 具有 READ 权限。

frame 窗口是本批第一行首个目标像素到最后一行末个目标像素的连续范围，向外对齐缓存行，包含行间空隙；INIT 包含行 padding。source 窗口覆盖本批实际采样的首行到末行，包含完整源行及中间跳过的行；缩放使用原目标坐标推导采样行，连续 GRAY4 的奇数宽度按半字节边界计算。完全裁掉的带源命令只携带源首个 32 字节缓存行以保持源地址身份，不携带 frame。FINISH 不携带 frame/source。

客户端先通过 `rr_validate_command()` 检查尺寸、flags 和源长度，再计算窗口，并在形成子 region 前检查 `offset/span` 不超过原始分配容量。整块 source 与 frame 的重叠也在提交前拒绝。remote 同样先校验命令，再根据会话实际行进度推导窗口，检查 frame 绑定及跨 MORE 的源地址。公共协议继续执行每个窗口的完整缓存所有权交接；每次 STEP 返回后不存在等待 FINISH 才写回的 frame 脏缓存。OPEN 仍同步完整 frame 一次；每个 STEP 不再同步整帧或整图。

## 内存与验证

ACPU 只分配一个固定会话状态，编译时限制为不超过 256 字节，不分配输入副本、私有 frame 或 fiber 栈。当前 ARM 会话状态为 144 字节。底层 allocator 管理开销与普通调用栈另计。HCPU 客户端分配一个小状态、128 字节 control 和 16 KiB 共享输入暂存区；主 frame 和大图输入的共享存储归调用方所有。

`tests/reader_render_compute/run.py --sanitize` 编译实际内核、remote 和客户端，验证独立参考像素、边界、排队接口的取消与回收。Windows 下 `--scheduler` 额外运行现有公共调度器的八项多线程准入、取消及关闭失败测试；`cache_run.py` 联合生产调度器和 remote，以独立 HCPU/RAM/ACPU 存储模拟缓存，统计实际缓存函数参数字节数，并检查窗口外写入、并发、取消和音频共存。`--arm-cc` 可额外对生产源执行 Cortex-M33 C99 严格告警编译。设备实际 IPC、硬件缓存和最终固件布局仍需板端验证。


## L8 图像显示

在 LVGL 线程调用 `epd_reader_render_image_init()`，成功后再将 L8 图片交给 `lv_image_set_src()`。图像描述符的 `header.cf=LV_COLOR_FORMAT_L8`，宽高、stride 与 render frame 一致，`data_size=stride*height`，data 直接指向计算缓冲。分配容量仍向上对齐 32 字节；图像及像素必须保留到绘制结束，渲染线程不能覆盖正在显示的帧。

项目解码器为普通 L8 变量图片提供一张全局 256 项 ARGB8888 灰度表。原生 EPIC 的 L8 输入使用查色表，不能省略这一步。解码器只分配一个小型 draw-buffer 描述符，直接借用像素，不做整帧转换；关闭时只释放描述符。初始化可重复调用，不重复注册。表占 HCPU 1024 字节，所有阅读帧共用；ACPU 不持有该表。LVGL 的现有显示缓冲格式保持不变。

`RR_BLIT_RGB565`、GRAY4、缩放和 A8 批处理均支持两种目标格式。RGB565 输出保持原算法；L8 混合保留 8 位灰度精度，重叠混合后的像素不承诺与逐次量化的 RGB565 输出逐位相同。

`tests/reader_render_compute/l8_run.py` 验证真实字形、独立参考像素、裁剪、取消、内存边界和三份缓存模型；`image_run.py` 验证真实 LVGL 图像控件；`epic_run.py` 从当前 SDK 提取原生绘制与查色表函数执行，替换硬件提交和寄存器，验证 L8 指针、stride、palette 与裁剪。后两项使用 `tests/notes/ui` 配置构建的 LVGL host 库，可通过 `--lvgl-lib` 指定。硬件提交测试不替代板端显示验证。
