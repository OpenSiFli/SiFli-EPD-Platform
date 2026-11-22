#ifndef PREFERENCE_HOST_H
#define PREFERENCE_HOST_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
typedef int rt_err_t;
typedef int32_t rt_int32_t;
typedef void *rt_device_t;
typedef int64_t off_t;
struct rt_mutex { CRITICAL_SECTION lock; const char *name; };
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EIO 5
#define RT_EINVAL 22
#define RT_EFULL 28
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_PRIO 0
#define RT_TICK_PER_SECOND 1000
#define RT_DEVICE_CTRL_RTC_SET_TIME 1
#define RTM_EXPORT(x)
#define MAX_FRIENDLY_NAME_LEN 248
#define STORAGE_FLASH_ROOT "/flash"
#define L2_RET_BSS_SECT_BEGIN(x)
#define L2_RET_BSS_SECT_END
#define L2_RET_BSS_SECT(x)
#define INIT_COMPONENT_EXPORT(fn) void host_boot_records(void) { (void)fn(); }
void host_boot_records(void);
void rt_enter_critical(void);
void rt_exit_critical(void);
void rt_thread_mdelay(int);
uint32_t rt_tick_get(void);
int32_t rt_tick_from_millisecond(int32_t);
int rt_mutex_init(struct rt_mutex *, const char *, int);
int rt_mutex_take(struct rt_mutex *, int);
int rt_mutex_release(struct rt_mutex *);
rt_device_t rt_device_find(const char *);
int rt_device_control(rt_device_t, int, void *);
void network_time_settings_changed(void);
void reminder_time_changed(void);
time_t host_time(time_t *);
struct tm *host_gmtime_r(const time_t *, struct tm *);
#define time host_time
#define gmtime_r host_gmtime_r
void storage_lock(void);
void storage_unlock(void);
bool storage_path_available(const char *);
bool storage_file_recover(const char *);
uint32_t storage_crc32(const void *, size_t);
uint32_t storage_file_writers(const char *);
bool storage_record_save(const char *, uint32_t, uint32_t, const void *, size_t);
bool storage_record_load(const char *, uint32_t, uint32_t, void *, size_t);
bool system_reset_write_allowed(void);
bool system_reset_blocks_apps(void);
bool storage_changing(void);
bool storage_release_requested(void);
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
int host_stat(const char *, struct host_stat *);
#include "app_service.h"
#include "preference_store.h"
#include "device_identity.h"
#include "system_time_internal.h"
#include "platform/epd_locale_internal.h"
typedef struct { char app_id[32], id[64], title[96], text[256]; } epd_notification_t;
enum { platform_settings_save_failed, platform_settings_save_failed_detail };
const char *epd_get_str(int);
bool epd_notification_publish_resident(const epd_notification_t *);
bool epd_notification_dismiss(const char *, const char *);
typedef struct { const char *locale; } lv_i18n_lang_t;
extern const lv_i18n_lang_t *const lv_i18n_lang_pack[];
#define LV_RES_OK 0
int resource_init(void);
void resource_deinit(void);
const char *lv_ext_get_locale(void);
int lv_ext_set_locale(void *, const char *);
void app_modules_set_locale(const char *);
void app_catalog_changed(void);
#endif
