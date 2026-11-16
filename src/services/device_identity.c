/* SPDX-License-Identifier: Apache-2.0 */
#include "device_identity.h"
#include "preference_store.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define IDENTITY_PATH STORAGE_FLASH_ROOT "/system/device_identity.bin"
#define IDENTITY_MAGIC 0x44494431u
#define IDENTITY_VERSION 1u

static char current_name[DEVICE_IDENTITY_NAME_CAPACITY] = DEVICE_IDENTITY_DEFAULT_NAME;
static struct rt_mutex name_lock;
static bool name_lock_ready;

static bool name_take(void)
{
    rt_enter_critical();
    if (!name_lock_ready) name_lock_ready = rt_mutex_init(&name_lock, "devname", RT_IPC_FLAG_PRIO) == RT_EOK;
    rt_exit_critical();
    return name_lock_ready && rt_mutex_take(&name_lock, RT_WAITING_FOREVER) == RT_EOK;
}

static void publish_name(const char *name)
{
    rt_enter_critical();
    strcpy(current_name, name);
    rt_exit_critical();
}

static rt_err_t validate_name(const char *name, size_t *length)
{
    if (!name || !*name) return -RT_EINVAL;
    size_t size = 0;
    while (size < DEVICE_IDENTITY_NAME_CAPACITY && name[size]) ++size;
    if (size > DEVICE_IDENTITY_NAME_MAX) return -RT_EINVAL;

    const unsigned char *bytes = (const unsigned char *)name;
    for (size_t offset = 0; offset < size;)
    {
        uint32_t value = bytes[offset++], minimum = 0;
        unsigned remaining = 0;
        if (value >= 0xc2 && value <= 0xdf)
        { value &= 0x1f; minimum = 0x80; remaining = 1; }
        else if (value >= 0xe0 && value <= 0xef)
        { value &= 0x0f; minimum = 0x800; remaining = 2; }
        else if (value >= 0xf0 && value <= 0xf4)
        { value &= 0x07; minimum = 0x10000; remaining = 3; }
        else if (value >= 0x80) return -RT_EINVAL;
        if (remaining > size - offset) return -RT_EINVAL;
        while (remaining--)
        {
            unsigned next = bytes[offset++];
            if ((next & 0xc0) != 0x80) return -RT_EINVAL;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff) ||
            value < 0x20 || (value >= 0x7f && value <= 0x9f)) return -RT_EINVAL;
    }
    *length = size;
    return RT_EOK;
}

rt_err_t device_identity_init(void)
{
    char saved[DEVICE_IDENTITY_NAME_CAPACITY];
    struct stat st;
    rt_err_t result = -RT_EIO;
    size_t length;
    if (!preference_store_begin_sync(PREFERENCE_IDENTITY)) return result;
    storage_lock();
    if (!storage_path_available(IDENTITY_PATH) || !storage_file_recover(IDENTITY_PATH)) goto done;
    if (stat(IDENTITY_PATH, &st) != 0)
    {
        if (errno == ENOENT || errno == -ENOENT)
        {
            publish_name(DEVICE_IDENTITY_DEFAULT_NAME);
            result = RT_EOK;
        }
        goto done;
    }
    if (!S_ISREG(st.st_mode)) { result = -RT_EINVAL; goto done; }
    if (!storage_record_load(IDENTITY_PATH, IDENTITY_MAGIC, IDENTITY_VERSION,
                             saved, sizeof(saved))) goto done;
    result = validate_name(saved, &length);
    if (result != RT_EOK) { result = -RT_EINVAL; goto done; }
    publish_name(saved);
done:
    storage_unlock();
    preference_store_end_sync(PREFERENCE_IDENTITY, PREFERENCE_SYNC_UNCHANGED);
    return result;
}

rt_err_t device_identity_get_name(char *out, size_t size)
{
    if (!out || !size) return -RT_EINVAL;
    rt_enter_critical();
    size_t length = strlen(current_name);
    rt_err_t result = size > length ? RT_EOK : -RT_EFULL;
    if (result == RT_EOK) memcpy(out, current_name, length + 1);
    else out[0] = '\0';
    rt_exit_critical();
    return result;
}

rt_err_t device_identity_set_name(const char *name)
{
    size_t length;
    rt_err_t result = validate_name(name, &length);
    if (result != RT_EOK) return result;
    char saved[DEVICE_IDENTITY_NAME_CAPACITY] = {0};
    memcpy(saved, name, length);
    if (!preference_store_begin_sync(PREFERENCE_IDENTITY)) return -RT_EIO;
    storage_lock();
    bool stored = storage_record_save(IDENTITY_PATH, IDENTITY_MAGIC, IDENTITY_VERSION, saved, sizeof(saved));
    if (stored)
    {
        if (name_take()) { publish_name(saved); rt_mutex_release(&name_lock); }
        else result = -RT_EIO;
    }
    else result = -RT_EIO;
    storage_unlock();
    preference_store_end_sync(PREFERENCE_IDENTITY, stored ? PREFERENCE_SYNC_SAVED : PREFERENCE_SYNC_FAILED);
    return result;
}

rt_err_t device_identity_request_name(const char *name)
{
    size_t length;
    rt_err_t result = validate_name(name, &length);
    if (result != RT_EOK) return result;
    char saved[DEVICE_IDENTITY_NAME_CAPACITY] = {0};
    memcpy(saved, name, length);
    if (!name_take()) return -RT_EIO;
    bool accepted = preference_store_submit(PREFERENCE_IDENTITY, IDENTITY_PATH,
        IDENTITY_MAGIC, IDENTITY_VERSION, saved, sizeof(saved));
    if (accepted) publish_name(saved);
    rt_mutex_release(&name_lock);
    return accepted ? RT_EOK : -RT_EIO;
}
