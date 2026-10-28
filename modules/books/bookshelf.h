/**
 * @file bookshelf.h
 * @brief 书库管理服务 —— 对外接口（UI 书架页调用）
 *
 * 职责：扫描内部存储与 TF 卡根目录中的电子书文件和分卷 EPUB，
 *       提供书籍列表快照（文件名 / 大小 / 阅读进度）。
 *
 * 数据流：bookshelf_scan() 在后台生成调用方独占的文件列表，
 *         UI 数据层读取历史后交给 GUI 接收完整快照。
 *         bookshelf_refresh() 保留命令行使用的同步列表接口。
 *
 * 线程说明：列表操作受互斥锁保护；事件回调在扫描调用方线程执行，
 *           后台调用方不能在回调中直接操作 UI。
 */
#ifndef __BOOKSHELF_H__
#define __BOOKSHELF_H__

#include <rtthread.h>
#include <stdbool.h>
#include "../../src/ui/platform/epd_reader_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOKSHELF_PATH_MAX  EPD_READER_PATH_MAX
#define BOOKSHELF_NAME_MAX  EPD_READER_TITLE_MAX
#define BOOKSHELF_MAX_ITEMS 32 /**< 单次最多枚举的书籍数量 */

/** 书籍条目 */
typedef epd_reader_file_t bookshelf_item_t;

/** 列表变化回调（扫描完成后在扫描调用方线程触发） */
typedef void (*bookshelf_event_cb_t)(int count);

/* ==================== 生命周期 ==================== */

/** 初始化书库服务（应用入口调用一次） */
rt_err_t bookshelf_service_init(void);
void bookshelf_service_deinit(void);

/* ==================== 数据查询 ==================== */

/**
 * @brief 同步扫描并在成功后替换服务列表；失败保留原列表
 * @return 发现的书籍数量（>=0）；负值为错误
 */
int bookshelf_refresh(void);

/* Scan into caller-owned storage. Cancellation is checked between directory
 * entries; no shared listing or UI object is changed. */
int bookshelf_scan(bookshelf_item_t *items, bool (*cancelled)(void *), void *user);

/* Reuse a content signature only while the runtime file version is unchanged. */
bool bookshelf_file_signature(const char *path, uint32_t size, uint32_t *signature);

/** 当前列表中的书籍数量 */
int bookshelf_count(void);

/**
 * @brief 拷贝书籍列表（线程安全）
 * @param items     输出数组
 * @param max_items 数组容量
 * @return 实际拷贝的条数
 */
int bookshelf_list(bookshelf_item_t *items, int max_items);

/** 获取指定序号书籍（0 起始）；成功返回 true */
bool bookshelf_get(int index, bookshelf_item_t *out);

/** 书库目录说明：TF 卡和内部文件系统的挂载根目录 */
const char *bookshelf_dir(void);

/* ==================== 事件 ==================== */

/** 注册列表变化回调（可传 NULL 注销） */
void bookshelf_set_event_cb(bookshelf_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* __BOOKSHELF_H__ */
