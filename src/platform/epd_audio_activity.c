/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#if defined(AUDIO_USING_MANAGER) && defined(RT_USING_THREAD_MEM_CONTEXT)
#include <audio_server.h>
#include "epd_memory_manager.h"

rt_err_t audio_callback_context_capture(rt_uint32_t *context)
{
    epd_memory_owner_t token = rt_thread_mem_context_get(RT_NULL);
    if (!epd_memory_owner_retain_trusted(token)) return -RT_EINVAL;
    *context = token;
    return RT_EOK;
}
void audio_callback_context_release(rt_uint32_t context)
{
    epd_memory_owner_release_trusted(context);
}
int audio_callback_context_invoke(rt_uint32_t context, audio_server_callback_func callback,
                                  audio_server_callback_cmt_t command, void *data, uint32_t value)
{
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(context);
    if (previous == EPD_MEMORY_OWNER_INVALID) return 0;
    int result = callback(command, data, value);
    epd_memory_owner_leave_trusted(previous);
    return result;
}
#endif
#if defined(AUDIO_USING_MANAGER) && defined(RT_USING_MEMHEAP_OWNER) && defined(RT_USING_THREAD_MEM_CONTEXT)
#include <audio_server.h>
#include "epd_activity_work.h"

/* Only the resident audio server touches these device records. A shared DMA
 * buffer belongs to the device activity, independent of its first client. */
typedef struct {
    void *key;
    epd_activity_work_t *work;
    epd_activity_guard_t guard;
    unsigned reservations;
} device_activity_t;
static device_activity_t devices[AUDIO_DEVICE_NUMBER];

typedef struct {
    epd_activity_work_t *work;
    epd_activity_guard_t guard;
} stream_activity_t;

void *audio_stream_memory_begin(void)
{
    epd_activity_work_t *work = epd_activity_work_open(256u * 1024u, 8192u);
    if (!work) return NULL;
    epd_activity_guard_t guard;
    if (!epd_activity_work_enter_trusted(work, &guard)) { epd_activity_work_close(work); return NULL; }
    stream_activity_t *stream = epd_app_alloc(sizeof(*stream), EPD_APP_PSRAM);
    if (!stream) {
        epd_activity_work_leave_trusted(&guard); epd_activity_work_close(work); return NULL;
    }
    stream->work = work; stream->guard = guard;
    return stream;
}
void audio_stream_memory_end(void *cookie)
{
    stream_activity_t *stream = cookie;
    epd_activity_work_leave_trusted(&stream->guard);
}
void audio_stream_memory_release(void *cookie)
{
    stream_activity_t *stream = cookie;
    epd_activity_work_t *work = stream->work;
    epd_app_free(stream);
    epd_activity_work_close(work);
}
rt_err_t audio_stream_memory_pause(void *cookie)
{
    stream_activity_t *stream = cookie;
    return stream && epd_activity_work_pause(stream->work) ? RT_EOK : -RT_EBUSY;
}
rt_err_t audio_stream_memory_resume(void *cookie)
{
    stream_activity_t *stream = cookie;
    return stream && epd_activity_work_resume(stream->work) ? RT_EOK : -RT_ENOMEM;
}

static device_activity_t *device_memory_find(void *key, bool create, size_t sram)
{
    device_activity_t *entry = NULL;
    for (unsigned i = 0; i < AUDIO_DEVICE_NUMBER; ++i) {
        if (devices[i].key == key) { entry = &devices[i]; break; }
        if (create && !devices[i].key && !entry) entry = &devices[i];
    }
    if (!entry) return NULL;
    if (!entry->work) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
        if (previous == EPD_MEMORY_OWNER_INVALID) return NULL;
        entry->work = epd_activity_work_open(32u * 1024u, sram);
        epd_memory_owner_leave_trusted(previous);
        if (!entry->work) return NULL;
        entry->key = key;
    }
    return entry;
}
void *audio_device_memory_reserve(void *key, const size_t *allocations, size_t count, bool enter)
{
    size_t sram = count ? 0u : 32u * 1024u;
    for (size_t i = 0; i < count; ++i) sram += epd_app_allocation_peak(allocations[i]);
    device_activity_t *entry = device_memory_find(key, true, sram);
    if (!entry || !epd_activity_work_replenish_to(entry->work, 32u * 1024u, sram)) return NULL;
    if (enter)
    {
        if (!epd_activity_work_enter_trusted(entry->work, &entry->guard)) return NULL;
    }
    else
    {
        /* Keep the reservation through the previous client's last close
         * if the next client will use the same device. */
        ++entry->reservations;
    }
    return entry;
}

void audio_device_memory_settle(void *key, bool active)
{
    device_activity_t *entry = device_memory_find(key, false, 0);
    if (!entry || entry->reservations) return;
    if (active) epd_activity_work_trim(entry->work);
    else
    {
        epd_activity_work_t *work = entry->work;
        entry->work = NULL;
        entry->key = NULL;
        epd_activity_work_close(work);
    }
}

void audio_device_memory_end(void *cookie, bool enter, bool active)
{
    device_activity_t *entry = cookie;
    if (!entry) return;
    if (enter) epd_activity_work_leave_trusted(&entry->guard);
    else --entry->reservations;
    audio_device_memory_settle(entry->key, active);
}
#endif
