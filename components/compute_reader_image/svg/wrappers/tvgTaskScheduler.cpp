#include "../config/reader_image_lv_conf.h"
#include "../config/reader_image_cxx.h"
#if defined(__GNUC__) && !defined(_WIN32)
#include <strings.h>
#include <alloca.h>
#endif
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgTaskScheduler.cpp"

extern "C" void ri_reset_tvgTaskScheduler(void) { tvg::inst = nullptr; tvg::_async = true; }
