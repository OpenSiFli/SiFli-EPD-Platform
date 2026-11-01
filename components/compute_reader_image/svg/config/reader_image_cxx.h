#ifndef READER_IMAGE_CXX_H
#define READER_IMAGE_CXX_H
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cinttypes>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
/* The LVGL SVG frontend supplies vector paints and decoded RAW images.
 * ThorVG's alternate file/JSON/SVG frontends are never used by this backend.
 * SDK config defines disabled loaders to zero, but the loader uses #ifdef. */
#include "../../../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/config.h"
#undef THORVG_SVG_LOADER_SUPPORT
#undef THORVG_LOTTIE_LOADER_SUPPORT
/* Apply only to SDK declarations after standard headers: allocation failures
 * must reach the transaction catch boundary instead of terminating. */
#define noexcept
/* glibc assert.h deliberately redeclares functions on repeated inclusion.
 * Keep its C declarations valid after the SDK-only noexcept adaptation. */
#if defined(__GLIBC__)
#undef __THROW
#define __THROW throw()
#undef __THROWNL
#define __THROWNL throw()
#endif
#endif
