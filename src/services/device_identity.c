/* SPDX-License-Identifier: Apache-2.0 */
#include "device_identity.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define IDENTITY_PATH STORAGE_FLASH_ROOT "/system/device_identity.bin"
#define IDENTITY_MAGIC 0x44494431u
#define IDENTITY_VERSION 1u

static char current_name[DEVICE_IDENTITY_NAME_CAPACITY] = DEVICE_IDENTITY_DEFAULT_NAME;

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
    storage_lock();
    if (!storage_path_available(IDENTITY_PATH) || !storage_file_recover(IDENTITY_PATH)) goto done;
    if (stat(IDENTITY_PATH, &st) != 0)
    {
        if (errno == ENOENT || errno == -ENOENT)
        {
            strcpy(current_name, DEVICE_IDENTITY_DEFAULT_NAME);
            result = RT_EOK;
        }
        goto done;
    }
    if (!S_ISREG(st.st_mode)) { result = -RT_EINVAL; goto done; }
    if (!storage_record_load(IDENTITY_PATH, IDENTITY_MAGIC, IDENTITY_VERSION,
                             saved, sizeof(saved))) goto done;
    result = validate_name(saved, &length);
    if (result != RT_EOK) { result = -RT_EINVAL; goto done; }
    memcpy(current_name, saved, length + 1);
done:
    storage_unlock();
    return result;
}

rt_err_t device_identity_get_name(char *out, size_t size)
{
    if (!out || !size) return -RT_EINVAL;
    storage_lock();
    size_t length = strlen(current_name);
    rt_err_t result = size > length ? RT_EOK : -RT_EFULL;
    if (result == RT_EOK) memcpy(out, current_name, length + 1);
    else out[0] = '\0';
    storage_unlock();
    return result;
}

rt_err_t device_identity_set_name(const char *name)
{
    size_t length;
    rt_err_t result = validate_name(name, &length);
    if (result != RT_EOK) return result;
    char saved[DEVICE_IDENTITY_NAME_CAPACITY] = {0};
    memcpy(saved, name, length);
    storage_lock();
    if (storage_record_save(IDENTITY_PATH, IDENTITY_MAGIC, IDENTITY_VERSION, saved, sizeof(saved)))
        memcpy(current_name, saved, sizeof(current_name));
    else result = -RT_EIO;
    storage_unlock();
    return result;
}
