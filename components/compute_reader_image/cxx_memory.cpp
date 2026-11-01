/* SPDX-License-Identifier: Apache-2.0 */
#include <new>
#include <cstdlib>
extern "C" void *ri_work_alloc(size_t);
extern "C" void ri_work_free(void *);
void *operator new(size_t n) { void *p = ri_work_alloc(n); if (!p) throw std::bad_alloc(); return p; }
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { ri_work_free(p); }
void operator delete[](void *p) noexcept { ri_work_free(p); }
void operator delete(void *p, size_t) noexcept { ri_work_free(p); }
void operator delete[](void *p, size_t) noexcept { ri_work_free(p); }
