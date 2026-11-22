/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOCUMENT_HOST_H
#define DOCUMENT_HOST_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#include <crtdbg.h>

typedef int rt_err_t;
typedef uintptr_t rt_base_t;
typedef uint32_t rt_tick_t;
typedef struct { int unused; } epd_service_t;
typedef struct { uint32_t magic, cf, w, h, stride; } lv_image_header_t;
typedef struct { lv_image_header_t header; const uint8_t *data; uint32_t data_size; } lv_image_dsc_t;
typedef struct { uint8_t *data; uint32_t data_size; } lv_draw_buf_t;
#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RGB565 1
#define RT_WAITING_FOREVER -1
#define RT_TICK_PER_SECOND 1000
#define RT_ALIGN(n, a) (((n) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define EPD_APP_PSRAM 0
#define STORAGE_PATH_MAX 512
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sd"
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)

static CRITICAL_SECTION irq_lock, io_lock;
static __declspec(thread) unsigned irq_depth;
static unsigned io_reads, io_opens, allocation_failures, service_wakes;
static unsigned ui_requests;
static void (*ui_probe)(void);
static void (*allocation_probe)(void), (*replace_probe)(void);
static bool replace_enabled, replace_fail, service_cancelled, sd_media, media_changing;
static unsigned replace_calls;
static uint32_t media_generation = 1;
static rt_tick_t host_ticks;
static unsigned open_logs, lookup_logs, readable_logs;
static char last_open_log[1024], last_lookup_log[1024], last_readable_log[1024];
static volatile LONG64 allocated_bytes;
static volatile LONG allocations;
static size_t psram_capacity = 8u * 1024u * 1024u;
typedef struct { size_t size; void *alignment; } host_allocation_t;
static bool media_present = true;
static rt_base_t rt_hw_interrupt_disable(void) { EnterCriticalSection(&irq_lock); ++irq_depth; return 0; }
static void rt_hw_interrupt_enable(rt_base_t key) { (void)key; --irq_depth; LeaveCriticalSection(&irq_lock); }
static void storage_lock(void) { EnterCriticalSection(&io_lock); }
static void storage_unlock(void) { LeaveCriticalSection(&io_lock); }
static void *epd_app_alloc(size_t n, int kind)
{
    (void)kind;
    if (allocation_probe) allocation_probe();
    if (allocation_failures) { --allocation_failures; return NULL; }
    host_allocation_t *p = malloc(sizeof(*p) + n);
    if (!p) return NULL;
    p->size = n;
    InterlockedAdd64(&allocated_bytes, (LONG64)(sizeof(*p) + n));
    InterlockedIncrement(&allocations);
    return p + 1;
}
static void epd_app_free(void *memory)
{
    if (!memory) return;
    host_allocation_t *p = (host_allocation_t *)memory - 1;
    InterlockedAdd64(&allocated_bytes, -(LONG64)(sizeof(*p) + p->size));
    free(p);
}
static void *epd_app_realloc(void *p, size_t n, int kind)
{
    void *out = epd_app_alloc(n, kind);
    if (out && p) { size_t old = ((host_allocation_t *)p - 1)->size; memcpy(out, p, old < n ? old : n); epd_app_free(p); }
    return out;
}
static size_t epd_app_psram_available(void)
{ return psram_capacity > (size_t)allocated_bytes ? psram_capacity - (size_t)allocated_bytes : 0; }
static void epd_app_clean_draw_buffer(lv_draw_buf_t *p) { (void)p; }
static bool epd_service_cancelled(epd_service_t *s) { (void)s; return service_cancelled; }
static void epd_service_wake(epd_service_t *s) { (void)s; ++service_wakes; }
static void epd_service_ui_request(epd_service_t *s, uint32_t delay)
{ (void)s; (void)delay; ++ui_requests; if (ui_probe) ui_probe(); }
static void epd_service_wait(epd_service_t *s, int n) { (void)s; (void)n; }
static void *epd_app_library_open(const char *a, const char *n) { (void)a; (void)n; return NULL; }
static void *epd_app_library_symbol(void *a, const char *n) { (void)a; (void)n; return NULL; }
static void epd_app_library_close(void *a) { (void)a; }
static rt_tick_t rt_tick_get(void) { return host_ticks; }
static rt_tick_t rt_tick_from_millisecond(unsigned n) { return n; }
static void rt_kprintf(const char *format, ...)
{
    char message[1024];
    va_list args; va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); va_end(args);
    if (!strncmp(message, "[document] open ", 16))
    { ++open_logs; strcpy(last_open_log, message); }
    else if (!strncmp(message, "[document] cache_lookup ", 24))
    { ++lookup_logs; strcpy(last_lookup_log, message); }
    else if (!strncmp(message, "[document] first_readable ", 26))
    { ++readable_logs; strcpy(last_readable_log, message); }
}
static void (*compute_probe)(void);
static bool compute_work;
static bool reader_compute_worker_step(epd_service_t *s)
{ (void)s; if (compute_probe) compute_probe(); return compute_work; }
static const char *storage_path_root(const char *p) { (void)p; return sd_media ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static bool storage_path_available(const char *p) { (void)p; return media_present; }
static bool storage_changing(void) { return media_changing; }
static uint32_t storage_card_session(void) { return media_generation; }
static bool storage_mkdirs(const char *p) { (void)p; return true; }
static bool storage_file_signature(const char *p, uint32_t *n) { (void)p; (void)n; return false; }
static bool storage_file_commit(const char *a, const char *b) { return rename(a, b) == 0; }
static bool storage_file_replace(const char *p, const void *data, size_t n)
{
    ++replace_calls;
    if (replace_probe) replace_probe();
    if (!replace_enabled || replace_fail) return false;
    int fd = _open(p, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return false;
    bool ok = _write(fd, data, (unsigned)n) == n && _commit(fd) == 0;
    if (_close(fd)) ok = false;
    return ok;
}
static bool storage_file_recover(const char *p) { (void)p; return true; }
static bool storage_file_replace_session(const char *p, const void *data, size_t n, uint32_t session)
{ return (!sd_media || session == media_generation) && storage_file_replace(p, data, n); }
static int host_open(const char *p, int flags, ...)
{ ++io_opens; return _open(p, flags | _O_BINARY, _S_IREAD | _S_IWRITE); }
static int host_read(int fd, void *p, size_t n) { ++io_reads; return _read(fd, p, (unsigned)n); }
#define open host_open
#define read host_read
#define write(fd, p, n) _write(fd, p, (unsigned)(n))
#define close _close
#define lseek _lseek
#define unlink _unlink
#define fsync _commit
typedef struct { int unused; } DIR;
struct dirent { char d_name[256]; };
static DIR *opendir(const char *p) { (void)p; return NULL; }
static struct dirent *readdir(DIR *p) { (void)p; return NULL; }
static void closedir(DIR *p) { (void)p; }
#endif
