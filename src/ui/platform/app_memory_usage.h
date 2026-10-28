#ifndef APP_MEMORY_USAGE_H
#define APP_MEMORY_USAGE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    size_t total, used, available, unassigned;
} app_ram_usage_t;

typedef struct
{
    app_ram_usage_t ram[2]; /* RAM0: HCPU SRAM; RAM1: PSRAM. */
} app_memory_snapshot_t;

/* Sample each firmware-lifetime heap under its allocator lock. No allocation,
 * block traversal or UI work is performed while a heap is locked. */
bool app_memory_snapshot(app_memory_snapshot_t *snapshot);

#endif
