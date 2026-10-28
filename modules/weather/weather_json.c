/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_json.h"
#include "epd_memory.h"
#include <cJSON.h>

/* cJSON's libc defaults use size_t, while SDK allocation uses rt_size_t. */
void *weather_json_default_malloc(size_t size)
{
    return rt_malloc(size);
}

void *weather_json_default_realloc(void *memory, size_t size)
{
    return rt_realloc(memory, size);
}

static void *json_allocate(size_t size)
{
    return epd_app_alloc(size, EPD_APP_PSRAM);
}

void weather_json_init(void)
{
    cJSON_Hooks hooks = {json_allocate, epd_app_free};
    cJSON_InitHooks(&hooks);
}
