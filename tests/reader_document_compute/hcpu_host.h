/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_DOCUMENT_HCPU_HOST_H
#define READER_DOCUMENT_HCPU_HOST_H
#include "../books/document_host.h"
#include <assert.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
static uint32_t storage_path_revision(const char *p) { (void)p; return media_generation; }
static uint32_t storage_volume_revision(storage_volume_t v) { (void)v; return media_generation; }
static unsigned storage_file_writers(const char *p) { (void)p; return 0; }
static bool storage_session_valid(storage_volume_t v, uint32_t n) { (void)v; return n == media_generation; }
static bool cache_card_available;
static bool storage_available(storage_volume_t v) { return v == STORAGE_FLASH || cache_card_available; }
static int hcpu_closedir(DIR *dir) { closedir(dir); return 0; }
#define closedir hcpu_closedir
static unsigned storage_depth;
static unsigned shutdown_delays;
static void (*shutdown_delay_probe)(void);
static void rt_thread_mdelay(int milliseconds)
{
    assert(!storage_depth && !irq_depth);
    ++shutdown_delays; host_ticks += (unsigned)milliseconds;
    if (shutdown_delay_probe) shutdown_delay_probe();
}
static void (*irq_probe)(void);
static rt_base_t hcpu_irq_disable(void)
{
    if (!irq_depth && irq_probe) { void (*probe)(void) = irq_probe; irq_probe = NULL; probe(); }
    return rt_hw_interrupt_disable();
}
#define rt_hw_interrupt_disable hcpu_irq_disable
static void hcpu_storage_lock(void) { storage_lock(); ++storage_depth; }
static void hcpu_storage_unlock(void) { --storage_depth; storage_unlock(); }
#define storage_lock hcpu_storage_lock
#define storage_unlock hcpu_storage_unlock
static void hcpu_before_free(void *memory);
static void rt_memory_info(uint32_t *total, uint32_t *used, uint32_t *peak)
{
    if (total) *total = 254088;
    if (used) *used = 238980;
    if (peak) *peak = 244748;
}
static void hcpu_app_free(void *memory) { hcpu_before_free(memory); epd_app_free(memory); }
#define epd_app_free hcpu_app_free
#endif
