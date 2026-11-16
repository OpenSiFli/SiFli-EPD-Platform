/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DFU_RECORDS_H
#define EPD_DFU_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DFU_SECTOR_SIZE 4096u
#define DFU_PATH_SIZE 768u
#define DFU_TARGET_MASK 0x0fu
#define DFU_STARTED 0x10u

typedef struct
{
    uint32_t zip_size;
    char path[DFU_PATH_SIZE];
} dfu_request_t;

typedef enum { DFU_REQUEST_EMPTY, DFU_REQUEST_VALID, DFU_REQUEST_BAD } dfu_request_status_t;

bool dfu_flash_read(uint32_t address, void *data, size_t size);
bool dfu_flash_write(uint32_t address, const void *data, size_t size);
bool dfu_flash_erase(uint32_t address, size_t size);

uint32_t dfu_crc32(uint32_t crc, const void *data, size_t size);
bool dfu_request_path_valid(const char *path);
dfu_request_status_t dfu_request_load(dfu_request_t *request);
bool dfu_request_store(const dfu_request_t *request);
bool dfu_start_new(const dfu_request_t *request);
bool dfu_state_load(uint32_t *state);
bool dfu_state_append(uint32_t state);
bool dfu_leave(void);

#endif
