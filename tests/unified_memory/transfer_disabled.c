/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_memory_manager.h"
#include <assert.h>

int main(void)
{
    unsigned buffer = 123;
    epd_memory_block_snapshot_t snapshot = {0};
    assert(epd_memory_block_get(&buffer, &snapshot) == EPD_MEMORY_INVALID);
    assert(!snapshot.physical);
    assert(epd_memory_block_transfer_try(&buffer, EPD_MEMORY_OWNER_SYSTEM) == EPD_MEMORY_INVALID);
    assert(epd_memory_block_transfer_try(NULL, EPD_MEMORY_OWNER_SYSTEM) == EPD_MEMORY_INVALID);
    assert(epd_memory_block_transfer(&buffer, EPD_MEMORY_OWNER_SYSTEM) == EPD_MEMORY_INVALID);
    assert(buffer == 123);
    return 0;
}
