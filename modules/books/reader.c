/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file reader.c
 * @brief 阅读引擎服务实现：TXT 打开 / 按偏移读取文本流（UTF-8·GBK 自动转码）/ 阅读位置
 */
#include "epd_memory.h"
#include "reader.h"
#include "storage.h"
#include "platform/epd_app.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DBG_TAG "reader"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* UTF-8 BOM */
static const unsigned char UTF8_BOM[3] = { 0xEF, 0xBB, 0xBF };

/* GBK→Unicode：复用 FatFs 936 码表（ffunicode.c 已随 elmfat 编入固件，零额外 Flash；
   原型见组件头 ff.h:368，输入为 (首字节<<8)|次字节；码页须与 CONFIG_RT_DFS_ELM_CODE_PAGE 一致） */
extern unsigned short ff_oem2uni(unsigned short oem, unsigned short cp);
#define READER_GBK_CODE_PAGE 936

/* Reuse physical reads while preserving the decoder's 1024-byte input span. */
#define RAW_PREFETCH_SIZE 8192u
#define DECODE_SPAN 1024u
static unsigned char g_raw[RAW_PREFETCH_SIZE];

struct reader_cursor
{
    struct reader_cursor *next;
    unsigned char *raw;
    uint32_t start, size;
    int fd;
};
static reader_cursor_t *g_cursors;

typedef struct
{
    rt_bool_t opened;
    int fd;
    char path[BOOKSHELF_PATH_MAX];
    char title[BOOKSHELF_NAME_MAX];
    uint32_t data_start; /* 文本起点（跳过 BOM 后） */
    uint32_t file_size;  /* 文本总字节数（含 data_start 之前的内容） */
    char encoding[16];
    rt_bool_t gbk;       /* true = GBK 源文件（读取时转 UTF-8 输出） */
    rt_bool_t detected_gbk;
    uint32_t position;
    uint32_t raw_start, raw_size;
    storage_volume_t volume;
    uint32_t session;
} reader_ctx_t;

static reader_ctx_t g_reader;
static rt_mutex_t g_lock;
static rt_bool_t g_inited;

static bool reader_available(void)
{
    return g_reader.opened && storage_path_available(g_reader.path) &&
           storage_session_valid(g_reader.volume, g_reader.session);
}

/*---------------------------------------------------------------------------*/
/* UTF-8 工具 */
/*---------------------------------------------------------------------------*/
static int utf8_char_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1; /* 非法字节按单字节处理 */
}


/*---------------------------------------------------------------------------*/
/* GBK → UTF-8（复用 FatFs 936 码表）                                          */
/*---------------------------------------------------------------------------*/
static rt_bool_t is_gbk_lead(unsigned char c)
{
    return (c >= 0x81 && c <= 0xFE) ? RT_TRUE : RT_FALSE;
}

/* Unicode(BMP) → UTF-8，返回写出字节数 */
static int utf8_emit(unsigned short uni, char *out)
{
    if (uni < 0x80)
    {
        out[0] = (char)uni;
        return 1;
    }
    if (uni < 0x800)
    {
        out[0] = (char)(0xC0 | (uni >> 6));
        out[1] = (char)(0x80 | (uni & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (uni >> 12));
    out[1] = (char)(0x80 | ((uni >> 6) & 0x3F));
    out[2] = (char)(0x80 | (uni & 0x3F));
    return 3;
}

/* 采样校验 UTF-8：出现非法序列即认定为非 UTF-8（样本尾部截断忽略） */
static rt_bool_t utf8_valid(const unsigned char *p, int n)
{
    int i = 0;

    while (i < n)
    {
        unsigned char c = p[i];
        int len;
        int k;

        if (c < 0x80)
        {
            i++;
            continue;
        }

        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            return RT_FALSE; /* 孤立续字节或非法首字节 */

        if (c < 0xC2 || c > 0xF4) /* 拒绝 C0/C1 超长编码与 F5+ */
            return RT_FALSE;

        if (i + len > n)
            break; /* 样本被截断，忽略尾部 */

        for (k = 1; k < len; k++)
        {
            if ((p[i + k] & 0xC0) != 0x80)
                return RT_FALSE;
        }
        i += len;
    }

    return RT_TRUE;
}

/* 编码检测：UTF-8 合法（或纯 ASCII）→ UTF-8；否则按 GBK */
static rt_bool_t detect_gbk(int fd, uint32_t size)
{
    static unsigned char sample[2048];
    rt_size_t want = (size > sizeof(sample)) ? sizeof(sample) : size;
    int n;

    if (lseek(fd, 0, SEEK_SET) < 0)
        return RT_FALSE;

    n = read(fd, sample, want);
    if (n <= 0)
        return RT_FALSE;

    return utf8_valid(sample, n) ? RT_FALSE : RT_TRUE;
}


/*---------------------------------------------------------------------------*/
/* 生命周期 */
/*---------------------------------------------------------------------------*/
rt_err_t reader_service_init(void)
{
    if (g_inited)
        return RT_EOK;

    g_lock = rt_mutex_create("rd_lock", RT_IPC_FLAG_PRIO);
    if (g_lock == RT_NULL)
        return -RT_ENOMEM;

    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd = -1;

    g_inited = RT_TRUE;
    return RT_EOK;
}

void reader_close(void)
{
    if (!g_inited)
        return;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (g_reader.opened && g_reader.fd >= 0)
    {
        close(g_reader.fd);
    }
    g_reader.opened = RT_FALSE;
    g_reader.fd = -1;
    g_reader.raw_size = 0;
    for (reader_cursor_t *cursor = g_cursors; cursor; cursor = cursor->next)
    {
        if (cursor->fd >= 0) close(cursor->fd);
        cursor->fd = -1;
        cursor->size = 0;
    }
    rt_mutex_release(g_lock);
}

static rt_err_t reader_open_file(const char *path)
{
    int fd;
    unsigned char bom[3];
    uint32_t start = 0;
    uint32_t size;
    off_t sz;
    rt_bool_t gbk;
    uint32_t session = storage_card_session();

    if (path == RT_NULL || path[0] == '\0' || !g_inited)
        return -RT_EINVAL;

    /* 先关闭旧书 */
    reader_close();

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        LOG_W("open(%s) failed", path);
        return -RT_EIO;
    }

    /* 文件大小 */
    sz = lseek(fd, 0, SEEK_END);
    if (sz < 0)
    {
        close(fd);
        return -RT_EIO;
    }
    size = (uint32_t)sz;

    /* BOM 检测（UTF-8 BOM 跳过） */
    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        close(fd);
        return -RT_EIO;
    }
    if (size >= 3 && read(fd, bom, 3) == 3)
    {
        if (memcmp(bom, UTF8_BOM, 3) == 0)
            start = 3;
    }

    /* 编码检测：带 BOM → UTF-8；否则采样校验，不合法按 GBK（读取时转 UTF-8） */
    gbk = (start == 0) ? detect_gbk(fd, size) : RT_FALSE;
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    if (!storage_session_valid(volume, session)) { close(fd); return -RT_EIO; }

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.opened = RT_TRUE;
    g_reader.fd = fd;
    g_reader.volume = volume;
    g_reader.session = session;
    rt_snprintf(g_reader.path, sizeof(g_reader.path), "%s", path);
    rt_snprintf(g_reader.title, sizeof(g_reader.title), "%s", path);
    {
        /* 标题取文件名（去掉目录部分） */
        char *slash = strrchr(g_reader.title, '/');
        if (slash != RT_NULL)
            memmove(g_reader.title, slash + 1, strlen(slash));
    }
    g_reader.data_start = start;
    g_reader.file_size = size;
    g_reader.position = start;
    g_reader.gbk = gbk;
    g_reader.detected_gbk = gbk;
    rt_snprintf(g_reader.encoding, sizeof(g_reader.encoding), gbk ? "GBK" : "UTF-8");
    rt_mutex_release(g_lock);

    return RT_EOK;
}

bool reader_is_open(void)
{
    bool opened;

    if (!g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    opened = g_reader.opened ? true : false;
    rt_mutex_release(g_lock);
    return opened;
}

bool reader_get_info(reader_info_t *out)
{
    if (out == RT_NULL || !g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (!reader_available())
    {
        rt_mutex_release(g_lock);
        return false;
    }

    rt_snprintf(out->path, sizeof(out->path), "%s", g_reader.path);
    rt_snprintf(out->title, sizeof(out->title), "%s", g_reader.title);
    out->file_size = g_reader.file_size;
    out->data_start = g_reader.data_start;
    rt_snprintf(out->encoding, sizeof(out->encoding), "%s", g_reader.encoding);
    out->position = g_reader.position;
    rt_mutex_release(g_lock);
    return true;
}

/*---------------------------------------------------------------------------*/
/* 文本读取 */
/*---------------------------------------------------------------------------*/
rt_err_t reader_set_encoding(unsigned encoding)
{
    if (!g_inited || encoding > 2) return -RT_EINVAL;
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    g_reader.gbk = encoding == 0 ? g_reader.detected_gbk : encoding == 2;
    rt_snprintf(g_reader.encoding, sizeof(g_reader.encoding), g_reader.gbk ? "GBK" : "UTF-8");
    rt_mutex_release(g_lock);
    return RT_EOK;
}

rt_err_t reader_open(const char *path)
{
    if (!path || !*path) return -RT_EINVAL;
    storage_lock();
    rt_err_t result = storage_path_available(path) ? reader_open_file(path) : -RT_EIO;
    storage_unlock();
    return result;
}

/* Called with g_lock held. Cache belongs to the open file, not its encoding. */
static int raw_read(reader_cursor_t *cursor, uint32_t offset, const unsigned char **data)
{
    uint32_t *start = cursor ? &cursor->start : &g_reader.raw_start;
    uint32_t *size = cursor ? &cursor->size : &g_reader.raw_size;
    unsigned char *raw = cursor ? cursor->raw : g_raw;
    int fd = cursor ? cursor->fd : g_reader.fd;
    if (fd < 0) return -RT_EIO;
    uint32_t remaining = g_reader.file_size - offset;
    uint32_t span = remaining < DECODE_SPAN ? remaining : DECODE_SPAN;
    if (offset < *start || offset - *start > *size || span > *size - (offset - *start))
    {
        uint32_t retained = 0;
        if (*size && offset >= *start && offset - *start <= *size)
        {
            retained = *size - (offset - *start);
            memmove(raw, raw + (offset - *start), retained);
        }
        else if (lseek(fd, offset, SEEK_SET) < 0) return -RT_EIO;
        *start = offset;
        *size = retained;
        uint32_t want = remaining < RAW_PREFETCH_SIZE ? remaining : RAW_PREFETCH_SIZE;
        while (*size < want)
        {
            int n = read(fd, raw + *size, want - *size);
            if (n <= 0 || !reader_available())
            { *size = 0; return -RT_EIO; }
            *size += n;
        }
    }
    *data = span ? raw + (offset - *start) : raw;
    return (int)span;
}

reader_cursor_t *reader_cursor_create(void)
{
    if (!g_inited) return NULL;
    reader_cursor_t *cursor = epd_app_alloc(sizeof(*cursor), EPD_APP_PSRAM);
    if (!cursor) return NULL;
    memset(cursor, 0, sizeof(*cursor));
    cursor->raw = epd_app_alloc(RAW_PREFETCH_SIZE, EPD_APP_PSRAM);
    if (!cursor->raw) { epd_app_free(cursor); return NULL; }
    storage_lock();
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    cursor->fd = reader_available() ?
                 open(g_reader.path, O_RDONLY) : -1;
    if (cursor->fd >= 0) { cursor->next = g_cursors; g_cursors = cursor; }
    rt_mutex_release(g_lock);
    storage_unlock();
    if (cursor->fd < 0) { epd_app_free(cursor->raw); epd_app_free(cursor); return NULL; }
    return cursor;
}

void reader_cursor_destroy(reader_cursor_t *cursor)
{
    if (!cursor) return;
    if (g_inited) rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    reader_cursor_t **link = &g_cursors;
    while (*link && *link != cursor) link = &(*link)->next;
    if (*link) *link = cursor->next;
    if (cursor->fd >= 0) close(cursor->fd);
    if (g_inited) rt_mutex_release(g_lock);
    epd_app_free(cursor->raw);
    epd_app_free(cursor);
}

int reader_cursor_read(reader_cursor_t *cursor, uint32_t offset, char *buf,
                       rt_size_t buf_size, uint32_t *offsets, uint32_t *next_offset)
{
    int out = 0, i = 0, n;
    const unsigned char *raw;
    if (!buf || buf_size < 5 || !g_inited) return -RT_EINVAL;
    buf[0] = '\0';
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (!g_reader.opened) { out = -RT_ERROR; goto done; }
    if (!reader_available()) { out = -RT_EIO; goto done; }
    if (offset < g_reader.data_start) offset = g_reader.data_start;
    if (offset > g_reader.file_size) offset = g_reader.file_size;
    n = raw_read(cursor, offset, &raw);
    if (n < 0) { out = -RT_EIO; goto done; }
    while (i < n)
    {
        unsigned char c = raw[i];
        char encoded[4];
        int source_len = 1, length = 1;
        encoded[0] = '?';
        if (c < 0x80)
        {
            encoded[0] = (char)c;
            if (c == '\r')
            {
                if (i + 1 == n && offset + n < g_reader.file_size) break;
                if (i + 1 < n && raw[i + 1] == '\n') source_len = 2;
                encoded[0] = '\n';
            }
            else if (c == '\t') encoded[0] = ' ';
            else if (c < 32 && c != '\n') encoded[0] = ' ';
        }
        else if (g_reader.gbk)
        {
            if (is_gbk_lead(c))
            {
                if (i + 1 == n && offset + n < g_reader.file_size) break;
                if (i + 1 < n && raw[i + 1] >= 0x40 &&
                    raw[i + 1] <= 0xFE && raw[i + 1] != 0x7F)
                {
                    unsigned short uni = ff_oem2uni((c << 8) | raw[i + 1], READER_GBK_CODE_PAGE);
                    if (uni) length = utf8_emit(uni, encoded);
                    source_len = 2;
                }
            }
        }
        else
        {
            int len = utf8_char_len(c);
            rt_bool_t valid = c >= 0xC2 && c <= 0xF4 && len > 1;
            if (valid && i + len > n && offset + n < g_reader.file_size) break;
            if (i + len > n) valid = RT_FALSE;
            for (int k = 1; valid && k < len; ++k)
                if ((raw[i + k] & 0xC0) != 0x80) valid = RT_FALSE;
            if (valid && ((c == 0xE0 && raw[i + 1] < 0xA0) ||
                          (c == 0xED && raw[i + 1] >= 0xA0) ||
                          (c == 0xF0 && raw[i + 1] < 0x90) ||
                          (c == 0xF4 && raw[i + 1] >= 0x90))) valid = RT_FALSE;
            if (valid)
            {
                source_len = length = len;
                memcpy(encoded, &raw[i], len);
            }
        }
        if ((rt_size_t)(out + length) >= buf_size) break;
        if (offsets)
            for (int k = 0; k < length; ++k) offsets[out + k] = offset + i;
        memcpy(buf + out, encoded, length);
        out += length;
        i += source_len;
    }
    buf[out] = '\0';
    if (offsets) offsets[out] = offset + i;
    if (next_offset) *next_offset = offset + i;
done:
    rt_mutex_release(g_lock);
    return out;
}

int reader_read_mapped(uint32_t offset, char *buf, rt_size_t buf_size,
                       uint32_t *offsets, uint32_t *next_offset)
{
    return reader_cursor_read(NULL, offset, buf, buf_size, offsets, next_offset);
}

int reader_read_text(uint32_t offset, char *buf, rt_size_t buf_size, uint32_t *next_offset)
{
    return reader_read_mapped(offset, buf, buf_size, RT_NULL, next_offset);
}

int reader_read_next(char *buf, rt_size_t buf_size)
{
    uint32_t offset;
    uint32_t next = 0;
    int n;

    if (!g_inited)
        return -RT_ERROR;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    offset = g_reader.position;
    rt_mutex_release(g_lock);

    n = reader_read_text(offset, buf, buf_size, &next);
    if (n >= 0)
        reader_set_position(next);

    return n;
}

/*---------------------------------------------------------------------------*/
/* 阅读位置 */
/*---------------------------------------------------------------------------*/
rt_err_t reader_set_position(uint32_t offset)
{
    if (!g_inited)
        return -RT_ERROR;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (!g_reader.opened)
    {
        rt_mutex_release(g_lock);
        return -RT_ERROR;
    }

    if (offset < g_reader.data_start)
        offset = g_reader.data_start;
    if (offset > g_reader.file_size)
        offset = g_reader.file_size;

    g_reader.position = offset;
    rt_mutex_release(g_lock);
    return RT_EOK;
}

uint32_t reader_get_position(void)
{
    uint32_t pos;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    pos = g_reader.position;
    rt_mutex_release(g_lock);
    return pos;
}

int reader_get_percent(void)
{
    int percent = 0;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (g_reader.opened && g_reader.file_size > g_reader.data_start)
    {
        uint32_t text_size = g_reader.file_size - g_reader.data_start;
        uint32_t pos = (g_reader.position > g_reader.data_start)
                           ? (g_reader.position - g_reader.data_start)
                           : 0;
        if (pos > text_size)
            pos = text_size;
        percent = (int)((uint64_t)pos * 100 / text_size);
    }
    rt_mutex_release(g_lock);

    return percent;
}

void reader_service_deinit(void)
{
    if (!g_inited) return;
    reader_close();
    rt_mutex_delete(g_lock);
    g_lock = NULL;
    g_inited = RT_FALSE;
}
