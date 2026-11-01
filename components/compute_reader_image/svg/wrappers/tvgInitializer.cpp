#include "../config/reader_image_lv_conf.h"
#include "../config/reader_image_cxx.h"
#if defined(__GNUC__) && !defined(_WIN32)
#include <strings.h>
#include <alloca.h>
#endif
#if defined(__ARM_EABI__) && defined(__GNUC__)
/* SDK formats only three integer version fields here. Newlib's integer-only
 * formatter avoids pulling floating-point printf into the image backend. */
extern "C" int sniprintf(char *, size_t, const char *, ...);
#define snprintf sniprintf
#endif
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgInitializer.cpp"
#undef snprintf

extern "C" void ri_reset_tvgInitializer(void) { _initCnt = 0; _version = 0; }
