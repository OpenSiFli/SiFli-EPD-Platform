#include "storage_internal.h"
#include "storage_file_internal.h"
#include "storage.h"
#include "storage_file.h"
#include "platform/epd_power.h"
#include "usb_card_reader.h"
#include <dfs_fs.h>
#include <dfs_posix.h>
#include <dfs_romfs.h>
#include "mem_map.h"
#include "drv_flash.h"
#include <string.h>
#include <rthw.h>
#include <rtm.h>
#ifdef PKG_USING_EPD_SDCARD
#include <rtdevice.h>
#include <sdcard.h>
#endif

static struct rt_mutex io_lock;
static bool flash_mounted;
#ifdef PKG_USING_EPD_SDCARD
static uint32_t apps_session;

static void card_mounted(void) { storage_watch_mount(STORAGE_SD); }

static void card_notify(void)
{
    usb_card_reader_notify();
    epd_power_notify();
}

static sdcard_snapshot_t card_snapshot(void)
{
    sdcard_snapshot_t card;
    sdcard_get_snapshot(&card);
    return card;
}
#endif

static const struct romfs_dirent directories[] = {
    {ROMFS_DIRENT_DIR, "dev", NULL, 0},
    {ROMFS_DIRENT_DIR, "flash", NULL, 0},
    {ROMFS_DIRENT_DIR, "sdcard", NULL, 0}
};
static const struct romfs_dirent root = {
    ROMFS_DIRENT_DIR, "/", (const rt_uint8_t *)directories, sizeof(directories) / sizeof(directories[0])
};

void storage_lock(void) { rt_mutex_take(&io_lock, RT_WAITING_FOREVER); }
void storage_unlock(void) { rt_mutex_release(&io_lock); }
RTM_EXPORT(storage_lock);
RTM_EXPORT(storage_unlock);
const char *storage_root(storage_volume_t v) { return v == STORAGE_SD ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
const char *storage_path_root(const char *path)
{
    return !strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)) ||
           !strcmp(path, STORAGE_SD_ROOT) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT;
}
const char *storage_relative_path(const char *path)
{
    const char *volume = storage_path_root(path);
    size_t n = strlen(volume);
    return !strncmp(path, volume, n) && path[n] == '/' ? path + n : path;
}
bool storage_available(storage_volume_t v)
{
    if (v == STORAGE_FLASH) return flash_mounted;
#ifdef PKG_USING_EPD_SDCARD
    if (v == STORAGE_SD) return card_snapshot().available;
#endif
    return false;
}
bool storage_path_available(const char *path)
{
    if (!path || !*path) return false;
    return storage_available(!strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH);
}
RTM_EXPORT(storage_path_available);
storage_replace_mode_t storage_replace_mode(storage_volume_t volume)
{
    storage_replace_mode_t mode = STORAGE_REPLACE_UNAVAILABLE;
    storage_lock();
    if (storage_available(volume))
    {
        const char *path = storage_root(volume);
        struct dfs_filesystem *fs = dfs_filesystem_lookup(path);
        if (fs && fs->path && !strcmp(fs->path, path) && fs->ops && fs->ops->name)
        {
            if (!strcmp(fs->ops->name, "lfs")) mode = STORAGE_REPLACE_ATOMIC;
            else if (!strcmp(fs->ops->name, "elm")) mode = STORAGE_REPLACE_BACKUP;
        }
    }
    storage_unlock();
    return mode;
}
uint32_t storage_card_session(void)
{
#ifdef PKG_USING_EPD_SDCARD
    return card_snapshot().session;
#else
    return 0;
#endif
}
bool storage_session_valid(storage_volume_t volume, uint32_t session)
{
#ifdef PKG_USING_EPD_SDCARD
    if (volume == STORAGE_SD)
    {
        sdcard_snapshot_t card = card_snapshot();
        return card.available && card.session == session;
    }
#else
    (void)session;
#endif
    return volume == STORAGE_FLASH && flash_mounted;
}
bool storage_apps_enabled(storage_volume_t volume)
{
#ifdef PKG_USING_EPD_SDCARD
    if (volume == STORAGE_SD)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        sdcard_snapshot_t card = card_snapshot();
        bool enabled = card.available && apps_session == card.session;
        rt_hw_interrupt_enable(level);
        return enabled;
    }
#endif
    return volume == STORAGE_FLASH && flash_mounted;
}
bool storage_card_enable_apps(uint32_t session)
{
#ifdef PKG_USING_EPD_SDCARD
    rt_base_t level = rt_hw_interrupt_disable();
    bool ok = storage_session_valid(STORAGE_SD, session);
    if (ok) apps_session = session;
    rt_hw_interrupt_enable(level);
    return ok;
#else
    (void)session;
    return false;
#endif
}
RTM_EXPORT(storage_card_session);
RTM_EXPORT(storage_session_valid);
bool storage_release_timed_out(void)
{
#ifdef PKG_USING_EPD_SDCARD
    sdcard_snapshot_t card = card_snapshot();
    return card.state == SDCARD_DRAINING && card.error_stage == SDCARD_ERROR_RELEASE_WAIT &&
           card.error == -RT_ETIMEOUT;
#else
    return false;
#endif
}
uint32_t storage_revision(void)
{
#ifdef PKG_USING_EPD_SDCARD
    return 1 + card_snapshot().revision;
#else
    return 1;
#endif
}
uint32_t storage_release_request_id(void)
{
#ifdef PKG_USING_EPD_SDCARD
    sdcard_snapshot_t card = card_snapshot();
    return card.state == SDCARD_DRAINING ? card.release_id : 0;
#else
    return 0;
#endif
}
bool storage_release_requested(void)
{
    return storage_release_request_id() != 0;
}
bool storage_release_for_export(void)
{
#ifdef PKG_USING_EPD_SDCARD
    sdcard_snapshot_t card = card_snapshot();
    return card.state == SDCARD_DRAINING && card.export_requested && card.available;
#else
    return false;
#endif
}
bool storage_changing(void)
{
#ifdef PKG_USING_EPD_SDCARD
    sdcard_snapshot_t card = card_snapshot();
    /* Probing blocks only SD access, not the Launcher or internal storage. */
    return (card.export_requested && card.available) || card.state == SDCARD_UNMOUNTING ||
           (card.state == SDCARD_DRAINING &&
            !(card.error_stage == SDCARD_ERROR_RELEASE_WAIT && card.error == -RT_ETIMEOUT));
#else
    return false;
#endif
}
rt_err_t storage_release_complete(uint32_t release_id)
{
#ifdef PKG_USING_EPD_SDCARD
    if (!release_id || release_id != storage_release_request_id()) return -RT_EINVAL;
    if (!storage_file_release(STORAGE_SD, release_id)) return -RT_EBUSY;
    return sdcard_release_complete(release_id);
#else
    (void)release_id;
    return -RT_ENOSYS;
#endif
}

bool storage_info(storage_volume_t v, storage_info_t *info)
{
    memset(info, 0, sizeof(*info));
    storage_lock();
    struct statfs fs;
    if (storage_available(v) && dfs_statfs(storage_root(v), &fs) == 0)
    {
        info->mounted = true;
        info->total = (uint64_t)fs.f_blocks * fs.f_bsize;
        info->used = (uint64_t)(fs.f_blocks - fs.f_bfree) * fs.f_bsize;
    }
    storage_unlock();
    return info->mounted;
}

void storage_format_size(uint64_t bytes, char *text, size_t size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    unsigned unit = 0;
    uint64_t scale = 1;
    while (unit < 3 && bytes / scale >= 1024) { scale *= 1024; ++unit; }
    rt_snprintf(text, size, "%lu.%lu %s", (unsigned long)(bytes / scale),
                (unsigned long)((bytes % scale) * 10 / scale), units[unit]);
}

void storage_init(void)
{
    rt_mutex_init(&io_lock, "storage", RT_IPC_FLAG_PRIO);
    if (dfs_mount(NULL, "/", "rom", 0, &root) != 0)
    {
        rt_kprintf("[storage] root mount failed\n");
        return;
    }
    register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE, "flash0");
    flash_mounted = dfs_mount("flash0", STORAGE_FLASH_ROOT, "lfs", 0, NULL) == 0;
    if (flash_mounted)
    {
        storage_watch_mount(STORAGE_FLASH);
        storage_prepare_directories();
    }
    else
        rt_kprintf("[storage] flash mount failed\n");
#ifdef PKG_USING_EPD_SDCARD
    const sdcard_config_t config = {
        .device_name = EPD_SDCARD_DEVICE_NAME,
        .mount_point = STORAGE_SD_ROOT,
        .detect_pin = EPD_SDCARD_DETECT_PIN,
        .present_level = PIN_LOW,
        .io_lock = &io_lock,
        .notify = card_notify,
        .mounted = card_mounted
    };
    rt_err_t result = sdcard_start(&config);
    if (result != RT_EOK)
        rt_kprintf("[storage] SD service start failed: %d\n", result);
#endif
}
