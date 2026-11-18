/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_dfu_request.h"
#include "dfu_records.h"
#include "drv_flash.h"
#include "ptab.h"
#include "sleep_guard.h"
#include "storage.h"
#include "storage_internal.h"
#include "usb_card_reader.h"
#include <dfs_posix.h>
#include <rthw.h>
#include <string.h>

static sleep_guard_owner_t update_hold = {.id = "firmware-update"};

bool dfu_flash_read(uint32_t address, void *data, size_t size)
{
    return rt_flash_read(address, data, (int)size) == (int)size;
}

bool dfu_flash_write(uint32_t address, const void *data, size_t size)
{
    return rt_flash_write(address, data, (int)size) == (int)size;
}

bool dfu_flash_erase(uint32_t address, size_t size)
{
    return rt_flash_erase(address, (int)size) == RT_EOK;
}

static bool loader_present(void)
{
    uint32_t vector[2];
    return dfu_flash_read(DFU_LOADER_START_ADDR, vector, sizeof(vector)) &&
           vector[0] >= HCPU_RAM_DATA_START_ADDR && vector[0] <= HCPU_RAM_DATA_START_ADDR + HCPU_RAM_DATA_SIZE &&
           (vector[1] & 1) && vector[1] >= DFU_EXEC_START_ADDR &&
           vector[1] < DFU_EXEC_START_ADDR + DFU_LOADER_SIZE - 2 * DFU_SECTOR_SIZE;
}

static rt_err_t update_request(const char *path, uint32_t signature, const char *name)
{
    const size_t root = sizeof(STORAGE_SD_ROOT) - 1;
    if (path && (strncmp(path, STORAGE_SD_ROOT "/", root + 1) ||
                 !(name ? dfu_font_path_valid(path + root) : dfu_request_path_valid(path + root)))) return -RT_EINVAL;
    if (name && (!*name || strlen(name)>=sizeof(((dfu_request_t *)0)->font_name))) return -RT_EINVAL;
    storage_lock();
    if (!sleep_guard_acquire(&update_hold))
    {
        storage_unlock();
        return -RT_EBUSY;
    }
    rt_err_t result = -RT_EIO;
    dfu_request_t request = {0};
    struct stat info;
    if (usb_card_reader_active() || storage_changing() || storage_release_requested()) result = -RT_EBUSY;
    else if (!loader_present()) result = -RT_ENOSYS;
    else if (path && (!storage_available(STORAGE_SD) || stat(path, &info) ||
                     !S_ISREG(info.st_mode) || info.st_size <= 0 ||
                     (uint64_t)info.st_size > UINT32_MAX)) result = -RT_EINVAL;
    else
    {
        if (path)
        {
            request.zip_size = (uint32_t)info.st_size;
            strcpy(request.path, path + root);
            if (name) {
                request.kind = DFU_REQUEST_FONT;
                request.signature = signature;
                strcpy(request.font_name, name);
            }
        }
        if (dfu_start_new(path ? &request : NULL)) rt_hw_cpu_reset();
    }
    sleep_guard_release(&update_hold);
    storage_unlock();
    return result;
}

rt_err_t epd_dfu_request(const char *path) { return update_request(path,0,NULL); }
rt_err_t epd_dfu_font_request(const char *path, uint32_t signature, const char *name)
{ return path && name ? update_request(path,signature,name) : -RT_EINVAL; }

#ifdef RT_USING_FINSH
#include <finsh.h>
static int epd_firmware_update(int argc, char **argv)
{
    if (argc > 2)
    {
        rt_kprintf("epd_dfu [TF ZIP path]\n");
        return -RT_EINVAL;
    }
    int result = epd_dfu_request(argc == 2 ? argv[1] : NULL);
    rt_kprintf("DFU request failed: %d\n", result);
    return result;
}
MSH_CMD_EXPORT_ALIAS(epd_firmware_update, epd_dfu, Enter TF firmware update);
#endif
