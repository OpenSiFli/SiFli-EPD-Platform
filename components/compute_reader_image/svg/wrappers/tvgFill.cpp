#include "../config/reader_image_lv_conf.h"
#include "../config/reader_image_cxx.h"
#if defined(__GNUC__) && !defined(_WIN32)
#include <strings.h>
#include <alloca.h>
#endif
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgFill.h"
/* colorStops(0) must release the same allocation domain as lv_realloc. */
#define free lv_free
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgFill.cpp"
#undef free
