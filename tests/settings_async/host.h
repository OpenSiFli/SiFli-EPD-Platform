#ifndef SETTINGS_ASYNC_HOST_H
#define SETTINGS_ASYNC_HOST_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ui_settings_internal.h"
#include "ui_settings_store.h"
typedef int rt_err_t;
typedef int rt_base_t;
typedef int32_t rt_int32_t;
typedef uint32_t rt_tick_t;
typedef unsigned rt_uint32_t;
typedef unsigned char rt_uint8_t;
typedef struct host_thread *rt_thread_t;
struct rt_mutex { CRITICAL_SECTION lock; };
struct rt_semaphore { HANDLE handle; bool barrier; };
struct rt_event { HANDLE handle; CRITICAL_SECTION lock; uint32_t bits; };
#define RT_EOK 0
#define RT_ENOSYS 38
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_PRIO 0
#define RT_IPC_FLAG_FIFO 0
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define RTM_EXPORT(symbol)
#define L2_RET_BSS_SECT_BEGIN(name)
#define L2_RET_BSS_SECT_END
#define L2_RET_BSS_SECT(name)
#define INIT_COMPONENT_EXPORT(fn) int host_init_##fn(void) { return fn(); }
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t);
void rt_enter_critical(void);
void rt_exit_critical(void);
rt_thread_t rt_thread_self(void);
void rt_thread_yield(void);
uint32_t rt_tick_get(void);
int rt_event_init(struct rt_event *,const char *,int);
int rt_event_detach(struct rt_event *);
int rt_event_send(struct rt_event *,uint32_t);
int rt_event_recv(struct rt_event *,uint32_t,int,int32_t,uint32_t *);
int rt_mutex_init(struct rt_mutex *, const char *, int);
int rt_mutex_detach(struct rt_mutex *);
int rt_mutex_take(struct rt_mutex *, int);
int rt_mutex_release(struct rt_mutex *);
int rt_sem_init(struct rt_semaphore *, const char *, unsigned, int);
int rt_sem_detach(struct rt_semaphore *);
int rt_sem_take(struct rt_semaphore *, int);
int rt_sem_release(struct rt_semaphore *);
rt_thread_t epd_thread_create_psram(const char *, void (*)(void *), void *, unsigned, unsigned char, unsigned);
int rt_thread_startup(rt_thread_t);
int rt_thread_delete(rt_thread_t);
typedef struct { bool held; const char *id; } sleep_guard_owner_t;
bool sleep_guard_acquire(sleep_guard_owner_t *);
void sleep_guard_release(sleep_guard_owner_t *);
void epd_power_notify(void);
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_FLASH 0
void storage_lock(void);
void storage_unlock(void);
bool storage_path_available(const char *);
bool storage_file_recover(const char *);
bool storage_file_replace(const char *, const void *, size_t);
uint32_t storage_crc32(const void *, size_t);
uint32_t storage_path_revision(const char *);
uint32_t storage_volume_revision(int);
uint32_t storage_file_writers(const char *);
bool system_reset_write_allowed(void);
bool system_reset_blocks_apps(void);
bool storage_changing(void);
bool board_power_pending(void);
#define O_RDONLY 0
#define S_ISREG(mode) ((mode) == 1)
#define open host_open
#define read host_read
#define close host_close
#define fstat host_fstat
#define stat host_stat
struct host_stat { unsigned st_mode; int64_t st_size; };
int host_open(const char *, int);
int host_read(int, void *, size_t);
int host_close(int);
int host_fstat(int, struct host_stat *);
int btpan_get_state(void);
#define BTPAN_STATE_OFF 0
int btpan_enable(bool);
int wlan_service_enable(bool);
typedef struct { char name[32]; } reading_font_info_t;
bool reading_font_get(unsigned, reading_font_info_t *);
unsigned reading_font_selected(void);
void epd_wave_set_part_times(unsigned);
unsigned epd_wave_get_part_times(void);
#define EPD_NOTIFICATION_ID_SIZE 64
typedef struct { char app_id[32], id[64], title[96], text[256], page[64]; bool ongoing; } epd_notification_t;
bool epd_notification_publish_resident(const epd_notification_t *);
bool epd_notification_dismiss(const char *, const char *);
#endif
