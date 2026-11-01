#include "../config/reader_image_lv_conf.h"
#include <stdlib.h>
extern float ri_svg_strtof(const char *, char **);
#define strtof ri_svg_strtof
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/svg/lv_svg_parser.c"
#undef strtof
