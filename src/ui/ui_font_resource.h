#ifndef UI_FONT_RESOURCE_H
#define UI_FONT_RESOURCE_H
#include "../../font/system_font_format.h"
const system_font_header_t *ui_font_resource(void);
const uint8_t *ui_font_resource_data(void);
#define ui_builtin_font_signature (ui_font_resource()->signature)
#define ui_builtin_font_size (ui_font_resource()->font_bytes)
#define ui_builtin_emoji_size (ui_font_resource()->emoji_bytes)
#define ui_builtin_font_pack_size (ui_font_resource()->packed_bytes)
#define ui_builtin_font_pack (ui_font_resource_data())
#endif
