#include "ui_font_resource.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_reader_font.h"
#include "reading_font.h"
#include <rtm.h>
bool epd_reader_font_fingerprint(uint32_t id, uint32_t *fingerprint)
{
    reading_font_info_t info;
    if (!fingerprint || !reading_font_get(id, &info) || !info.available) return false;
    uint32_t key = (2166136261u ^ ui_builtin_font_signature) * 16777619u;
    key = (key ^ id) * 16777619u;
    *fingerprint = (key ^ (id ? info.signature : ui_builtin_font_signature)) * 16777619u;
    return true;
}
RTM_EXPORT(epd_reader_font_fingerprint);

int epd_reader_font_begin(void)
{
    if (reading_font_begin()) return 1;
    reading_font_status_t status;
    reading_font_get_status(&status);
    return status.ready || status.busy ? 0 : -1;
}
RTM_EXPORT(epd_reader_font_begin);
