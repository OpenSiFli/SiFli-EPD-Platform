/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DFU_PACKAGE_H
#define EPD_DFU_PACKAGE_H

#include <stdint.h>

#define DFU_PACKAGE_MAX_IMAGES 4u
#define DFU_PACKAGE_ERASE_SIZE 4096u
#define DFU_PACKAGE_BOARD "sf32lb57-epd-gyd7n6"
#define DFU_PACKAGE_LAYOUT "epd57-tf-dfu-v1"
#define DFU_PACKAGE_STAGING_DIR "0:/EPDDFU"

typedef enum
{
    DFU_TARGET_MAIN,
    DFU_TARGET_RESOURCES,
    DFU_TARGET_ACPU,
    DFU_TARGET_WAVEFORM,
} dfu_target_t;

typedef enum
{
    DFU_PACKAGE_OK = 0,
    DFU_PACKAGE_INVALID = -1,
    DFU_PACKAGE_IO = -2,
    DFU_PACKAGE_ZIP = -3,
    DFU_PACKAGE_MANIFEST = -4,
    DFU_PACKAGE_LAYOUT_MISMATCH = -5,
    DFU_PACKAGE_RANGE = -6,
    DFU_PACKAGE_CRC = -7,
    DFU_PACKAGE_NO_SPACE = -8,
} dfu_package_result_t;

typedef struct
{
    dfu_target_t target;
    uint32_t address;
    uint32_t size;
    uint32_t crc32;
    const char *staging_path;
} dfu_package_image_t;

typedef struct
{
    uint32_t count;
    uint32_t total_size;
    uint32_t zip_size;
    dfu_package_image_t images[DFU_PACKAGE_MAX_IMAGES];
} dfu_package_t;

typedef void (*dfu_package_progress_t)(dfu_target_t target, uint32_t done,
                                       uint32_t total);

/*
 * Mount the TF card as FatFs drive 0 before calling. Pass an absolute UTF-8 path.
 * The staging directory is reserved. Call the functions serially, and
 * keep the progress callback within the display layer.
 *
 * open reads ZIP metadata and the manifest. prepare reads them again and
 * stages all images. Both populate package only on success. Progress is
 * cumulative uncompressed bytes. A target's final update follows f_sync
 * and f_close. Stage paths are valid for Flash input after prepare succeeds.
 */
dfu_package_result_t dfu_package_open(const char *path, dfu_package_t *package);
dfu_package_result_t dfu_package_prepare(const char *path, dfu_package_t *package,
                                         dfu_package_progress_t progress);
dfu_package_result_t dfu_package_cleanup(void);

#endif
