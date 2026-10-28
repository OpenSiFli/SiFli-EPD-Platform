/*
 * Copyright (c) 2026, SiFli Technology
 * SPDX-License-Identifier: LicenseRef-SiFli
 * See LICENSE in the component root.
 */
#ifndef __SDCARD_H__
#define __SDCARD_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SDCARD_ABSENT = 0,
    SDCARD_PROBING,
    SDCARD_READY,
    SDCARD_DRAINING,
    SDCARD_UNMOUNTING,
    SDCARD_FAULT,
    SDCARD_EXPORTED,
    SDCARD_FORMATTING,
} sdcard_state_t;

typedef enum
{
    SDCARD_ERROR_NONE = 0,
    SDCARD_ERROR_START,
    SDCARD_ERROR_PROBE_SUBMIT,
    SDCARD_ERROR_PROBE_WAIT,
    SDCARD_ERROR_PROBE_RESULT,
    SDCARD_ERROR_MOUNT,
    SDCARD_ERROR_RELEASE_WAIT,
    SDCARD_ERROR_OPEN_FILES,
    SDCARD_ERROR_UNMOUNT,
    SDCARD_ERROR_REMOVE_SUBMIT,
    SDCARD_ERROR_REMOVE_WAIT,
    SDCARD_ERROR_REMOVE_RESULT,
    SDCARD_ERROR_EXPORT_OPEN,
    SDCARD_ERROR_EXPORT_GEOMETRY,
    SDCARD_ERROR_EXPORT_CLOSE,
    SDCARD_ERROR_IDENTITY,
    SDCARD_ERROR_FORMAT,
    SDCARD_ERROR_REMOUNT,
} sdcard_error_stage_t;

#define SDCARD_IDENTITY_VERSION 1u
typedef struct
{
    uint32_t version;
    uint32_t cid[4];           /* Complete SDK resp_cid, not the decoded serial. */
    uint32_t sector_start;     /* Physical start of the registered block device. */
    uint32_t sector_count;     /* Block device capacity; bytes = count * size. */
    uint32_t sector_size;
    uint32_t volume_start;     /* FAT volume start relative to the block device. */
    uint32_t partition_sector_count; /* MBR partition capacity, or whole SFD device. */
    uint32_t volume_sector_count;
    uint32_t volume_serial;
    uint32_t fat_bits;         /* 12, 16 or 32. */
    uint8_t partition_table[72]; /* MBR bytes 438..509, including signature/entries. */
    uint8_t volume_boot[512];  /* Exact FAT boot sector, stable across reboot. */
} sdcard_identity_t;

/**
 * Both operations acquire config.io_lock (an RT-Thread recursive mutex).
 * Callers may already hold that same lock. Identity is zeroed on any error.
 * RT_EOK requires a complete CID, verified device mapping and readable, valid
 * FAT boot identity. An unmounted card is supported after a mount failure when
 * its FAT boot identity remains valid; damaged/unknown identity returns an
 * error and cannot authorize formatting. USB ownership or handoff returns
 * -RT_EBUSY. Unsupported media layouts return -RT_ENOSYS.
 */
rt_err_t sdcard_get_identity(sdcard_identity_t *out);
rt_bool_t sdcard_identity_equal(const sdcard_identity_t *a, const sdcard_identity_t *b);

/**
 * Blocking background-thread operation. The caller first saves/stops all file
 * users and retains the confirmed identity and current snapshot.session.
 * Rechecks both before any destructive operation. Open files or handoff return
 * -RT_EBUSY without waiting for a release acknowledgment. Revokes the session,
 * unmounts, calls SDK dfs_mkfs("elm", device), and remounts under io_lock.
 * Failure can leave the confirmed card unmounted or partially formatted;
 * snapshot.error_stage distinguishes format and remount failure. Never retries
 * formatting automatically. A successful remount publishes a new session.
 */
rt_err_t sdcard_format(const sdcard_identity_t *identity, uint32_t session);

typedef struct
{
    const char *device_name;
    const char *mount_point;
    rt_base_t detect_pin;
    rt_base_t present_level;
    rt_mutex_t io_lock;
    /* Optional resident event signal after snapshot changes. May run in an IRQ
     * or with interrupts disabled: do not block or access the snapshot here. */
    void (*notify)(void);
    /* Thread context under io_lock, before the mounted volume is published. */
    void (*mounted)(void);
} sdcard_config_t;

typedef struct
{
    sdcard_state_t state;
    rt_bool_t present;       /* Last debounced pin value, not readiness. */
    rt_bool_t mounted;       /* Remains true until DFS unmount succeeds. */
    rt_bool_t available;     /* Revoked immediately on a removal IRQ. */
    uint32_t session;        /* Media epoch; file I/O also requires available. Zero is reserved. */
    uint32_t release_id;     /* Current retirement request; zero when none. */
    uint32_t revision;
    sdcard_error_stage_t error_stage;
    rt_err_t error;
    rt_bool_t export_requested; /* Cleared synchronously on disable/removal. */
} sdcard_snapshot_t;

typedef struct
{
    rt_bool_t ready;
    uint32_t block_count;
    uint32_t block_size;
    uint32_t session;
} sdcard_export_info_t;

/**
 * Start the single, resident SDIO card service from thread context, after SDK
 * device initialization. The configuration is copied; strings, the mutex and
 * the SDK host must remain valid for the firmware lifetime. The mount path must
 * be an existing, canonical absolute directory, not the root directory.
 *
 * The board owns pin mux/pull configuration. This service owns only the input
 * mode, detection IRQ and card lifecycle. No other service may mount/unmount or
 * submit card changes for this host. The pin must match EPD_SDCARD_DETECT_PIN;
 * this SDK's startup detector supports active-low card detection.
 *
 * All file users share io_lock, check available/session before new I/O, and
 * retain their original session. They may close/release invalidated objects.
 * Return RT_EOK when the worker is started, not when the card is mounted.
 * Initialization failure unwinds owned resources and permits a later retry.
 */
rt_err_t sdcard_start(const sdcard_config_t *config);

/** Copy a coherent snapshot. No internal mutable object is returned. */
void sdcard_get_snapshot(sdcard_snapshot_t *snapshot);

/**
 * Acknowledge exactly one release request after all users have stopped and
 * released files, raw device accesses and asynchronous work. Call from thread
 * context without holding io_lock. Stale IDs return -RT_EINVAL. Repeating an
 * accepted acknowledgment while still DRAINING returns -RT_EBUSY. No forced
 * release occurs.
 */
rt_err_t sdcard_release_complete(uint32_t release_id);

/**
 * Request exclusive block access from thread context. Enabling is accepted only
 * in READY with a valid local session. The worker publishes DRAINING/release_id;
 * existing file users may save with that session until release acknowledgment.
 * The platform must block new business while the handoff is pending.
 *
 * Disabling immediately revokes export readiness, then wakes the worker to
 * close the device and remount locally. Canceling DRAINING before acknowledgment
 * needs no acknowledgment and preserves the local session if the card remains
 * present. Removal clears the request; replacement cards are not auto-exported.
 * This call does not take io_lock or wait for device I/O.
 */
rt_err_t sdcard_set_export(rt_bool_t enabled);

/**
 * All four export operations require the caller to hold config.io_lock.
 * They never acquire that mutex themselves. info is zeroed when not ready and
 * returns -RT_EBUSY. Other operations also reject revoked/removed media.
 * sector is a logical sector index; length is a nonzero byte count divisible
 * by block_size. A short SDK transfer is an error. Retain the info session and
 * recheck it under the same lock before I/O belonging to an earlier USB command.
 * The SDK completes native SD writes synchronously. Holding the same io_lock
 * waits for in-flight writes; sync returns a write error retained for this export
 * session, or success when every write completed. It does not call BLK_SYNC.
 */
rt_err_t sdcard_export_info(sdcard_export_info_t *info);
rt_err_t sdcard_export_read(uint32_t sector, void *buffer, uint32_t length);
rt_err_t sdcard_export_write(uint32_t sector, const void *buffer, uint32_t length);
rt_err_t sdcard_export_sync(void);

#ifdef __cplusplus
}
#endif
#endif
