#ifndef EPD_SYSTEM_FONT_FORMAT_H
#define EPD_SYSTEM_FONT_FORMAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define SYSTEM_FONT_SLOT_BYTES 0x005e0000u
#define SYSTEM_FONT_HEADER_BYTES 256u
#define SYSTEM_FONT_MAGIC 0x31524653u
#define SYSTEM_FONT_NAME_BYTES 96u
typedef struct {
    uint32_t magic, version, packed_bytes, font_bytes, emoji_bytes, signature, packed_crc;
    char name[SYSTEM_FONT_NAME_BYTES];
    uint8_t reserved[128];
    uint32_t header_crc;
} system_font_header_t;
_Static_assert(sizeof(system_font_header_t) == SYSTEM_FONT_HEADER_BYTES, "font header layout");
static inline uint32_t system_font_crc(uint32_t crc, const void *data, size_t count)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (count--) {
        crc ^= *p++;
        for (unsigned i=0;i<8;++i) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static inline bool system_font_header_valid(const system_font_header_t *h)
{
    return h && h->magic == SYSTEM_FONT_MAGIC && h->version == 1 && h->font_bytes &&
        h->font_bytes <= 64u*1024u*1024u && h->emoji_bytes <= 2u*1024u*1024u &&
        h->packed_bytes >= 24 && h->packed_bytes <= SYSTEM_FONT_SLOT_BYTES-SYSTEM_FONT_HEADER_BYTES &&
        h->name[0] && memchr(h->name, 0, sizeof(h->name)) &&
        h->header_crc == system_font_crc(0,h,offsetof(system_font_header_t,header_crc));
}
#endif
