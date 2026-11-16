/* SPDX-License-Identifier: Apache-2.0 */
#include "dfu_records.h"
#include "ptab.h"
#include <string.h>

#define STATE_SECTOR (DFU_LOADER_START_ADDR + DFU_LOADER_SIZE - 2 * DFU_SECTOR_SIZE)
#define REQUEST_SECTOR (STATE_SECTOR + DFU_SECTOR_SIZE)
#define BOOT_FLAG (STATE_SECTOR + 288)
#define STATE_JOURNAL (STATE_SECTOR + 512)
#define STATE_MAGIC 0x53544600u
#define REQUEST_MAGIC 0x45504455u
#define REQUEST_VERSION 1u

typedef struct
{
    uint32_t magic, version, length, crc;
    dfu_request_t request;
} request_record_t;

typedef struct { uint32_t value, inverse; } state_record_t;

uint32_t dfu_crc32(uint32_t crc, const void *data, size_t size)
{
    static const uint32_t nibble[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *bytes = data;
    crc = ~crc;
    while (size--)
    {
        crc ^= *bytes++;
        crc = (crc >> 4) ^ nibble[crc & 15];
        crc = (crc >> 4) ^ nibble[crc & 15];
    }
    return ~crc;
}

bool dfu_request_path_valid(const char *path)
{
    if (!path || path[0] != '/') return false;
    size_t length = 0, component = 0;
    for (; length < DFU_PATH_SIZE && path[length]; ++length)
    {
        unsigned char c = path[length];
        if (c < 32 || c == '\\' || c == ':') return false;
        if (c == '/')
        {
            if (length && (!component || (component == 1 && path[length - 1] == '.') ||
                          (component == 2 && path[length - 1] == '.' && path[length - 2] == '.')))
                return false;
            component = 0;
        }
        else ++component;
    }
    return length < DFU_PATH_SIZE && component > 4 && path[length - 4] == '.' &&
           (path[length - 3] | 32) == 'z' && (path[length - 2] | 32) == 'i' &&
           (path[length - 1] | 32) == 'p';
}

static bool set_flag(uint32_t flag)
{
    uint32_t check;
    return dfu_flash_write(BOOT_FLAG, &flag, sizeof(flag)) &&
           dfu_flash_read(BOOT_FLAG, &check, sizeof(check)) && check == flag;
}

dfu_request_status_t dfu_request_load(dfu_request_t *request)
{
    request_record_t record;
    if (!dfu_flash_read(REQUEST_SECTOR, &record, sizeof(record))) return DFU_REQUEST_BAD;
    if (record.magic == UINT32_MAX) return DFU_REQUEST_EMPTY;
    if (record.magic != REQUEST_MAGIC || record.version != REQUEST_VERSION ||
        record.length != sizeof(record.request) || !record.request.zip_size ||
        record.crc != dfu_crc32(0, &record.request, sizeof(record.request)) ||
        !dfu_request_path_valid(record.request.path)) return DFU_REQUEST_BAD;
    *request = record.request;
    return DFU_REQUEST_VALID;
}

bool dfu_request_store(const dfu_request_t *request)
{
    request_record_t record = {0};
    if (request)
    {
        if (!request->zip_size || !dfu_request_path_valid(request->path)) return false;
        record.magic = REQUEST_MAGIC;
        record.version = REQUEST_VERSION;
        record.length = sizeof(record.request);
        record.request.zip_size = request->zip_size;
        strcpy(record.request.path, request->path);
        record.crc = dfu_crc32(0, &record.request, sizeof(record.request));
    }
    if (!dfu_flash_erase(REQUEST_SECTOR, DFU_SECTOR_SIZE)) return false;
    if (request && !dfu_flash_write(REQUEST_SECTOR, &record, sizeof(record))) return false;
    dfu_request_t check;
    dfu_request_status_t result = dfu_request_load(&check);
    return request ? result == DFU_REQUEST_VALID && check.zip_size == request->zip_size &&
                     strcmp(check.path, request->path) == 0 : result == DFU_REQUEST_EMPTY;
}

static bool scan_state(uint32_t *state, uint32_t *next)
{
    *next = 0;
    *state = DFU_STARTED | DFU_TARGET_MASK;
    bool valid = false;
    for (uint32_t address = STATE_JOURNAL; address < REQUEST_SECTOR; address += sizeof(state_record_t))
    {
        state_record_t record;
        if (!dfu_flash_read(address, &record, sizeof(record))) return false;
        if (record.value == UINT32_MAX && record.inverse == UINT32_MAX)
        {
            *next = address;
            break;
        }
        if ((record.value & ~(DFU_STARTED | DFU_TARGET_MASK)) == STATE_MAGIC &&
            record.value == ~record.inverse)
        {
            uint32_t candidate = record.value & (DFU_STARTED | DFU_TARGET_MASK);
            if ((candidate && !(candidate & DFU_STARTED)) ||
                (valid && (*state & DFU_STARTED) && !(candidate & DFU_STARTED))) return false;
            *state = candidate;
            valid = true;
        }
    }
    return valid;
}

bool dfu_state_load(uint32_t *state)
{
    uint32_t next;
    return scan_state(state, &next);
}

bool dfu_state_append(uint32_t state)
{
    if ((state & ~(DFU_STARTED | DFU_TARGET_MASK)) || (state && !(state & DFU_STARTED))) return false;
    uint32_t current, address;
    bool valid = scan_state(&current, &address);
    if (valid && current == state) return true;
    if (!address || (valid && (current & DFU_STARTED) && !(state & DFU_STARTED))) return false;
    unsigned pending = 0;
    for (uint32_t bits = state & DFU_TARGET_MASK; bits; bits &= bits - 1) ++pending;
    if ((REQUEST_SECTOR - address) / sizeof(state_record_t) <= pending) return false;
    const uint32_t value = STATE_MAGIC | state;
    const state_record_t record = {value, ~value};
    state_record_t check;
    return dfu_flash_write(address, &record, sizeof(record)) &&
           dfu_flash_read(address, &check, sizeof(check)) &&
           check.value == record.value && check.inverse == record.inverse;
}

bool dfu_start_new(const dfu_request_t *request)
{
    uint32_t flag;
    return dfu_flash_read(BOOT_FLAG, &flag, sizeof(flag)) && flag != 1 &&
           dfu_request_store(request) && dfu_flash_erase(STATE_SECTOR, DFU_SECTOR_SIZE) &&
           dfu_state_append(0) && set_flag(1);
}

bool dfu_leave(void)
{
    uint32_t state;
    return dfu_state_load(&state) && !(state & DFU_TARGET_MASK) &&
           dfu_request_store(NULL) && set_flag(0);
}
