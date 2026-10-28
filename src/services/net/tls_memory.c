/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>

#ifdef MBEDTLS_USER_CONFIG_FILE
#include "epd_memory.h"
#include <mbedtls/platform.h>

static void *_tls_calloc(size_t count, size_t size)
{
    /* Bound the product before the SDK heap rounds the allocation size. */
    if (!count || !size || count > (size_t)IMAGE_CACHE_IN_PSRAM_SIZE / size)
    {
        return RT_NULL;
    }

    size_t bytes = count * size;
    void *memory = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (memory)
    {
        rt_memset(memory, 0, bytes);
    }
    return memory;
}

static int _tls_memory_init(void)
{
    return mbedtls_platform_set_calloc_free(_tls_calloc, epd_app_free);
}

/* The SDK initializes the shared PSRAM heap at INIT_PREV, before components.
 * Install once before application threads can allocate TLS objects. */
INIT_COMPONENT_EXPORT(_tls_memory_init);

RTM_EXPORT(mbedtls_calloc);
RTM_EXPORT(mbedtls_free);
#endif
