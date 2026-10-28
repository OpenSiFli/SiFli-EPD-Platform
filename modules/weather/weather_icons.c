#include "weather_icons.h"
#include "platform/app_image.h"
#include "storage_file.h"
#include <stdio.h>

void ui_weather_icon_set(lv_obj_t *image, int code, bool large)
{
    static const unsigned short codes[] = {
        100, 101, 102, 103, 104, 150, 151, 152, 153,
        300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313,
        314, 315, 316, 317, 318, 350, 351, 399, 400, 401, 402, 403, 404, 405,
        406, 407, 408, 409, 410, 456, 457, 499, 500, 501, 502, 503, 504, 507,
        508, 509, 510, 511, 512, 513, 514, 515, 800, 801, 802, 803, 804, 805,
        806, 807, 900, 901, 999
    };
    unsigned i;
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
        if (codes[i] == code) break;
    if (i == sizeof(codes) / sizeof(codes[0])) code = 999;
    char name[48], path[96];
    snprintf(name, sizeof(name), "res/w%d_%d.ezip", code, large ? 160 : 48);
    if (!storage_app_path(path, sizeof(path), "weather", STORAGE_APP_CODE, name))
    { epd_app_image_set(image, NULL, NULL); return; }
    if (!epd_app_image_set(image, path, NULL) && code != 999)
    {
        snprintf(name, sizeof(name), "res/w999_%d.ezip", large ? 160 : 48);
        bool found = storage_app_path(path, sizeof(path), "weather", STORAGE_APP_CODE, name);
        epd_app_image_set(image, found ? path : NULL, NULL);
    }
}
