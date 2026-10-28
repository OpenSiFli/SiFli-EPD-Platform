#include "app_memory_usage.h"
#include <rtthread.h>
#include "mem_map.h"
#include <string.h>

extern struct rt_memheap app_image_psram_memheap;
extern unsigned char __RW_PSRAM_NON_RET_end__[];

static bool contains(uintptr_t first, size_t size, uintptr_t address)
{
    return address >= first && address - first < size;
}

static bool sample_heap(struct rt_memheap *heap, app_memory_snapshot_t *out)
{
    if (!heap) return false;
    uintptr_t address = (uintptr_t)heap->start_addr;
    unsigned region;
    if (contains(HPSYS_RAM0_BASE, HPSYS_RAM_SIZE, address)) region = 0;
    else if (contains(PSRAM_BASE, PSRAM_SIZE, address)) region = 1;
    else return false;
    if (rt_sem_take(&heap->lock, RT_WAITING_FOREVER) != RT_EOK) return false;
    out->ram[region].available += heap->available_size;
    rt_sem_release(&heap->lock);
    return true;
}

bool app_memory_snapshot(app_memory_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    app_memory_snapshot_t snapshot = {0};
    snapshot.ram[0].total = HPSYS_RAM_SIZE;
    snapshot.ram[1].total = PSRAM_SIZE;
    /* The SRAM remainder includes linked firmware, stacks and SDK reservations.
     * PSRAM beyond the linked heaps is not allocatable free memory. */
    uintptr_t psram_end = (uintptr_t)__RW_PSRAM_NON_RET_end__;
    if (psram_end < PSRAM_BASE || psram_end > PSRAM_BASE + PSRAM_SIZE) return false;
    snapshot.ram[1].unassigned = PSRAM_BASE + PSRAM_SIZE - psram_end;
    struct rt_memheap *system_heap = (struct rt_memheap *)rt_object_find("heap", RT_Object_Class_MemHeap);
    struct rt_memheap *ui_heap = (struct rt_memheap *)rt_object_find("lv_psram", RT_Object_Class_MemHeap);
    /* These heaps have firmware lifetime and disjoint, static backing. */
    if (!sample_heap(system_heap, &snapshot) ||
        !sample_heap(&app_image_psram_memheap, &snapshot) ||
        !sample_heap(ui_heap, &snapshot)) return false;
    for (unsigned r = 0; r < 2; ++r)
    {
        app_ram_usage_t *ram = &snapshot.ram[r];
        if (ram->available + ram->unassigned > ram->total) return false;
        ram->used = ram->total - ram->available - ram->unassigned;
    }
    *out = snapshot;
    return true;
}
