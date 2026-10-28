/**
 * @file reader.h
 * @brief 阅读引擎服务 —— 对外接口（UI 阅读页调用）
 *
 * 职责：打开 TXT 书籍、按“字节偏移”取 UTF-8 文本流、读写阅读位置。
 *       分页由 UI 侧基于 LVGL 文本度量实现（字号/行距变化可即时重算），
 *       本服务保证任意偏移读取的文字流一致（编码转换后输出 UTF-8）。
 *
 * 编码支持：UTF-8（含 BOM 跳过）与 GBK（自动检测并转 UTF-8 输出，
 *           复用固件内 FatFs 936 码表，零额外 Flash）；读取时保证不截断
 *           多字节字符；BIG5 转换后续迭代补充（需求：文本设置里的编码选项）。
 *
 * 线程说明：接口线程安全，可在 UI 线程直接调用（文件 IO 已加锁）。
 */
#ifndef __READER_H__
#define __READER_H__

#include <rtthread.h>
#include <stdbool.h>

#include "bookshelf.h" /* 复用 BOOKSHELF_PATH_MAX / BOOKSHELF_NAME_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/** 阅读器信息 */
typedef struct
{
    char path[BOOKSHELF_PATH_MAX]; /**< 文件路径 */
    char title[BOOKSHELF_NAME_MAX]; /**< 文件名 */
    uint32_t file_size;            /**< 文件总字节数 */
    uint32_t data_start;           /**< 原文件正文起点，跳过 UTF-8 BOM */
    char encoding[16];             /**< 检测到的编码："UTF-8" / "GBK" */
    uint32_t position;             /**< 当前读取位置（字节偏移） */
} reader_info_t;

/* ==================== 生命周期 ==================== */

/** 初始化阅读服务（应用入口调用一次） */
rt_err_t reader_service_init(void);
void reader_service_deinit(void);

/** 打开书籍（打开前会自动关闭已打开的书籍） */
rt_err_t reader_open(const char *path);

/** 关闭当前书籍 */
void reader_close(void);

/** 是否有已打开的书籍 */
bool reader_is_open(void);

/** 查询当前书籍信息（线程安全） */
bool reader_get_info(reader_info_t *out);

/* ==================== 文本读取 ==================== */

/**
 * @brief 从指定偏移读取一段 UTF-8 文本（无状态，供 UI 分页取词）
 * @param offset       原文件绝对字节偏移（包含 BOM，调用方应保存字符边界）
 * @param buf          输出缓冲
 * @param buf_size     缓冲大小（实际最多读 buf_size-1 字节）
 * @param next_offset  输出：下一次应读取的偏移（保证落在完整字符边界）
 * @return 读取的字节数（>=0）；负值为错误
 * @note 输出恒为 UTF-8（GBK 源文件自动转码）；尾部若为不完整多字节字符，
 *       会被截掉（下次从该字符开始读）；返回的字节数与 next_offset
 *       前进量可能不等（转码膨胀/收缩所致）
 */
int reader_read_text(uint32_t offset, char *buf, rt_size_t buf_size, uint32_t *next_offset);

/* offsets has buf_size entries. At each UTF-8 character boundary it stores
 * the absolute source-file offset, including offsets[returned_length].
 * CRLF/CR are normalized to LF; invalid characters become '?'. */
int reader_read_mapped(uint32_t offset, char *buf, rt_size_t buf_size,
                       uint32_t *offsets, uint32_t *next_offset);
/* Optional session-owned raw window. NULL uses the shared window. A cursor
 * remains invalid after reader_close; destroy it before creating a new one. */
typedef struct reader_cursor reader_cursor_t;
reader_cursor_t *reader_cursor_create(void);
void reader_cursor_destroy(reader_cursor_t *cursor);
int reader_cursor_read(reader_cursor_t *cursor, uint32_t offset, char *buf,
                       rt_size_t buf_size, uint32_t *offsets, uint32_t *next_offset);
/* 0: automatic detection, 1: UTF-8, 2: GBK. */
rt_err_t reader_set_encoding(unsigned encoding);

/**
 * @brief 从当前 position 读取并自动前进（顺序阅读用）
 * @param buf      输出缓冲
 * @param buf_size 缓冲大小
 * @return 读取的字节数；负值为错误
 */
int reader_read_next(char *buf, rt_size_t buf_size);

/* ==================== 阅读位置（进度） ==================== */

/** 设置当前读取位置（自动裁剪到 [0, file_size]） */
rt_err_t reader_set_position(uint32_t offset);

/** 获取当前读取位置 */
uint32_t reader_get_position(void);

/** 当前进度百分比（0~100，按字节偏移估算） */
int reader_get_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* __READER_H__ */
