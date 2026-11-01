/* SPDX-License-Identifier: Apache-2.0 */
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include "svg_exception.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/thorvg/tvgStr.h"
static unsigned char *storage;
static bool occupied;
extern "C" void ri_exception_set_storage(void *p)
{ storage = static_cast<unsigned char *>(p); occupied = false; }

extern "C" float ri_svg_strtof(const char *s, char **end)
{
    /* The SDK's heap-free converter stores the integer part in uint64_t and
     * iterates over the exponent. Bound those two resource-sensitive inputs
     * before calling it; decimal/coordinate parsing stays in SDK code. */
    const char *p=s;
    while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') ++p;
    if (*p=='+' || *p=='-') ++p;
    while (*p=='0') ++p;
    unsigned digits=0;
    while (*p>='0' && *p<='9') { ++p; if (++digits>18) throw ri_svg_invalid(); }
    if (*p=='.') { ++p; while (*p>='0' && *p<='9') ++p; }
    if (*p=='e' || *p=='E') {
        ++p; if (*p=='+' || *p=='-') ++p;
        unsigned exponent=0;
        while (*p>='0' && *p<='9') {
            exponent=exponent*10u+(unsigned)(*p++-'0');
            if(exponent>38u) throw ri_svg_invalid();
        }
    }
    return tvg::strToFloat(s,end);
}

#if defined(__GNUC__)
/* Unhandled exceptions remain fatal. Avoid the hosted verbose handler's
 * demangler and stderr formatting; recoverable OOM is caught by the decoder. */
namespace __gnu_cxx {
[[noreturn]] void __verbose_terminate_handler() { std::abort(); }
}
/* libsupc++ initializes its exception header immediately before the returned
 * object. Both the header and payload stay inside caller-budgeted storage.
 * The 512-byte prefix exceeds the GCC 14.2.1 ARM EABI 128-byte header
 * measured from this toolchain's eh_alloc.o. */
extern "C" void *__cxa_allocate_exception(size_t n) noexcept
{
    if (!storage || occupied || n > RI_EXCEPTION_PAYLOAD) std::abort();
    occupied = true;
    std::memset(storage, 0, RI_EXCEPTION_PREFIX + RI_EXCEPTION_PAYLOAD);
    return storage + RI_EXCEPTION_PREFIX;
}
extern "C" void __cxa_free_exception(void *p) noexcept
{
    if (p != storage + RI_EXCEPTION_PREFIX) std::abort();
    occupied = false;
}
/* Nested/dependent exceptions are outside the serialized backend contract. */
extern "C" void *__cxa_allocate_dependent_exception() noexcept { std::abort(); }
extern "C" void __cxa_free_dependent_exception(void *) noexcept { std::abort(); }
namespace std {
[[noreturn]] void __throw_bad_alloc() { throw ri_svg_oom(); }
[[noreturn]] void __throw_bad_array_new_length() { throw ri_svg_oom(); }
[[noreturn]] void __throw_logic_error(const char *) { throw ri_svg_invalid(); }
[[noreturn]] void __throw_length_error(const char *) { throw ri_svg_invalid(); }
[[noreturn]] void __throw_out_of_range_fmt(const char *, ...) { throw ri_svg_invalid(); }
}
#endif
