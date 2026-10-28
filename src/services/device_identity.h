/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DEVICE_IDENTITY_H
#define EPD_DEVICE_IDENTITY_H

#include <stddef.h>
#include <rtthread.h>
#include <bts2_bt.h>

/* SDK local-name limit in UTF-8 bytes, excluding the terminating NUL. */
#define DEVICE_IDENTITY_NAME_MAX MAX_FRIENDLY_NAME_LEN
#define DEVICE_IDENTITY_NAME_CAPACITY (DEVICE_IDENTITY_NAME_MAX + 1)
#define DEVICE_IDENTITY_DEFAULT_NAME "RT-EPD-Reader"

#ifdef __cplusplus
extern "C" {
#endif

/* Call after storage_init(), before starting Bluetooth. Reloads the saved name;
 * a missing record selects the product default. Read failures preserve the
 * current name and return -RT_EIO; invalid payloads return -RT_EINVAL.
 * The product default is available before device_identity_init(); storage_init
 * must precede all identity APIs because they share the storage mutex. */
rt_err_t device_identity_init(void);

/* Copies a complete NUL-terminated snapshot. No truncation: -RT_EFULL means
 * insufficient capacity and leaves a nonzero-sized output empty.
 * NULL output or zero capacity returns -RT_EINVAL. */
rt_err_t device_identity_get_name(char *out, size_t size);

/* Synchronous, ordinary-thread API. Empty/invalid UTF-8, Unicode C0/C1 controls
 * and names exceeding the SDK byte limit return -RT_EINVAL.
 * Spaces, punctuation and other Unicode characters are preserved.
 * Commits through storage_file before publishing the name. -RT_EIO preserves
 * the old name. RT_EOK confirms persistence, not Bluetooth controller state. */
rt_err_t device_identity_set_name(const char *name);

#ifdef __cplusplus
}
#endif
#endif
