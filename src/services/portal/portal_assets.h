#ifndef EPD_PORTAL_ASSETS_H
#define EPD_PORTAL_ASSETS_H
#include <stddef.h>
typedef struct { const char *path, *type; const unsigned char *bytes; size_t size; } portal_asset_t;
extern const portal_asset_t portal_assets[];
extern const unsigned portal_asset_count;
#endif
