#include "../src/ui/ui_font_resource.h"
#include "ptab.h"

const system_font_header_t *ui_font_resource(void)
{
    return (const system_font_header_t *)BUILTIN_RESOURCE_START_ADDR;
}
const uint8_t *ui_font_resource_data(void)
{
    return (const uint8_t *)(BUILTIN_RESOURCE_START_ADDR+SYSTEM_FONT_HEADER_BYTES);
}
