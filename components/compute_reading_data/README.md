# Reading-data 组件

`epd/reading-data` 版本 1 在 ACPU 执行增量 FNV-1a、reader v2索引及document-cache v3元数据校验。HCPU只打包输入和检查返回值，没有本地算法fallback。每个STEP最多处理 `min(step_limit, 4096)` 字节，remote每64字节检查取消。会话只保留固定大小的解析状态；host复用4160字节输入和32字节输出缓冲区。

产品组件列表加入 `components/compute_reading_data`；板级策略需启用 ready、`step_shared=true`，推荐 `step_limit=4096`。支持 foreground/background。`compute.json` 分别提供 HCPU app、HCPU firmware 和 ACPU 源文件；HCPU 不编入 `reading_data_remote.c`。固件构建导出客户端 RTM 符号，动态模块用 `BUILD_DLMODULE`。

## 增量签名

包含 `epd_reading_data.h`。创建不会等待 compute 准入，首次非空哈希才调用可取消的 `epd_compute_open_wait`。一个客户端可连续处理多个存储块，直到 owner 关闭会话。

```c
epd_reading_data_t client = NULL;
int status = epd_reading_data_create(EPD_COMPUTE_BACKGROUND, &client);
if (status == EPD_COMPUTE_OK) {
    /* Publish client to the task's cancellation owner before hashing. */
    bool ok = storage_file_signature_with(path, &signature,
        epd_reading_data_signature_update, client);
    /* After the worker and cancellation callers have returned: */
    status = epd_reading_data_close(&client);
    /* Retain client for cleanup retry when status != EPD_COMPUTE_OK. */
}
```

`epd_reading_data_signature_update(void *, const void *, size_t, uint32_t *)` 与 `storage_signature_update_t` 类型一致。输入 hash 是上一段的 FNV seed；整次调用成功后才更新 hash，失败不发布部分结果。`epd_reading_data_hash` 提供相同操作的 compute 状态码返回值。`hash_skip` 排除一个连续字节区间，可跨 STEP 边界。

同一客户端由 worker 串行调用。取消所有者可并发执行 `epd_reading_data_cancel(client)`；客户端内部持有 OPEN 的 wait handle，取消在准入、OPEN 交接和 STEP 阶段均有效。取消为粘性。操作失败后关闭客户端，再为新任务创建客户端。`epd_reading_data_status` 返回客户端错误或取消状态。CLOSE 失败保留句柄及缓冲区，成功将调用方句柄置空；重复关闭空句柄成功。

`epd_reading_data_set_purpose` 更新已有会话用途，不重新 OPEN。普通 hash/index 是 worker 同步接口。

## 持有其他 compute 会话的 provider

provider 必须先调用 `epd_reading_data_try_open(client)`，仅在成功后执行 hash。该接口使用非阻塞准入；BUSY 不改变客户端错误状态，可以稍后重试。持有 document 会话时不能转为等待准入：FIFO 队首的独占请求可能正在等待该 document 会话释放。

document cache 使用非阻塞准入。BUSY 或哈希失败禁用本次可选缓存持久化，转换仍可继续。`document_cache_file_signature` 通过 storage 的公共签名 LRU 读取签名；缓存命中不创建 data 客户端。每次文件哈希的所有读取块共用一个会话。dependency 观察期间多个资源也共用同一个 data 会话，在 observe_end 关闭。大 dependency payload 的 identity hash 在 storage_lock 外执行，提交前重新验证资源版本。

## 索引

`epd_reading_data_index_check(client, blob, bytes, &spec)` 校验现有 reader v2 布局：36 字节 header、`count+1` 个 `{record, byte}`、`toc_count` 个 `{record, level}`，字段均为小端 32 位整数。`epd_reading_index_spec_t` 指定 magic/version/layout/end/page_max/text_limit。

remote 校验完整长度、checksum、header 标识、游标范围与严格递增关系、TOC record 范围和顺序。空文档允许 complete、count=1、end=0 的两个零游标。跨块解析保留未完整的字段和上一条游标，输入在调用返回后不再被借用。

索引保存时以 `EPD_READING_DATA_FNV_SEED` 为 seed，调用 `epd_reading_data_hash_skip(client, blob, bytes, offsetof(epd_reading_index_header_t, checksum), sizeof(uint32_t), &hash)`，成功后再写 checksum。

索引校验完成后汇总输出 `backend=ACPU`、payload bytes、实际对齐的 tx/rx bytes。增量 FNV 默认不打印日志，避免存储每 4 KiB 回调引发同步串口输出；调试时定义 `EPD_READING_DATA_TRACE=1` 可开启每次 hash 调用的详细汇总。响应必须包含 ACPU 标识及对应 sequence，错误响应不会发布结果。

## 文档缓存元数据

`epd_reading_data_cache_check(client, blob, bytes, &spec, &checksum)` 流式校验document-cache v3负载并返回FNV-1a值。spec给出source/title定长数组容量及显式身份标志。负载包含12个小端uint32字段、source/title数组和依赖条目；每项为5个uint32字段及含终止符的相对路径。ACPU检查总长度、条目数量、字段范围、字符串终止和路径段，最后返回从 `EPD_READING_DATA_FNV_SEED` 开始计算的校验值。调用失败不修改checksum。

负载最多64KiB、依赖最多512项、source最多256字节、title最多128字节，依赖路径含终止符最多256字节。wire新增 `RD_CACHE_BEGIN=4`、`RD_CACHE_DATA=5`，原有请求/响应尺寸和组件版本保持不变。HCPU和ACPU需一起集成新增操作；没有该操作时缓存未命中，不在HCPU执行替代校验。
