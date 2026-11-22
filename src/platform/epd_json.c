/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_json.h"
#include "epd_memory.h"
#include <cJSON.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void *json_allocate(size_t size)
{
    rt_err_t saved_errno = rt_get_errno();
    void *memory = epd_app_alloc(size, EPD_APP_PSRAM);
    rt_set_errno(memory ? saved_errno : ENOMEM);
    return memory;
}

void epd_json_init(void)
{
    static bool initialized;
    if (initialized) return;
    cJSON_Hooks hooks = {json_allocate, rt_free};
    cJSON_InitHooks(&hooks);
    initialized = true;
}

epd_json_status_t epd_json_parse(const char *text, size_t length, cJSON **out)
{
    rt_err_t saved_errno = rt_get_errno();
    epd_json_status_t result = EPD_JSON_FORMAT_ERROR;
    cJSON *json = NULL;
    if (out) *out = NULL;
    rt_set_errno(0);
    if (out && text && length && length <= PTRDIFF_MAX && !memchr(text, 0, length))
    {
        const char *end = NULL;
        json = cJSON_ParseWithLengthOpts(text, length, &end, false);
        rt_err_t parse_errno = rt_get_errno();
        if (json)
        {
            const char *limit = text + length;
            while (end < limit && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
            if (end == limit) result = EPD_JSON_OK;
            else { cJSON_Delete(json); json = NULL; }
        }
        else if (parse_errno == ENOMEM) result = EPD_JSON_NO_MEMORY;
    }
    if (out) *out = json;
    rt_set_errno(saved_errno);
    return result;
}

void epd_json_delete(cJSON *json)
{
    cJSON_Delete(json);
}
