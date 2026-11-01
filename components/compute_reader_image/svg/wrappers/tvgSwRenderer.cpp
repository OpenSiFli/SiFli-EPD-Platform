#include "../config/reader_image_lv_conf.h"
#include "../config/reader_image_cxx.h"
#if defined(__GNUC__) && !defined(_WIN32)
#include <strings.h>
#include <alloca.h>
#endif
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgSwRenderer.cpp"

extern "C" void ri_reset_tvgSwRenderer(void) { initEngineCnt = 0; rendererCnt = 0; globalMpool = nullptr; threadsCnt = 0; }
