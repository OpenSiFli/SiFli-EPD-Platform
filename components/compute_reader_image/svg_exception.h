#ifndef READER_IMAGE_SVG_EXCEPTION_H
#define READER_IMAGE_SVG_EXCEPTION_H
struct ri_svg_oom {};
struct ri_svg_invalid {};
extern "C" void *ri_work_alloc(size_t);
extern "C" void ri_work_free(void *);
/* One serialized exception; prefix reserves ABI header space independently
 * of private libsupc++ structure definitions. */
enum { RI_EXCEPTION_PREFIX = 512, RI_EXCEPTION_PAYLOAD = 512 };
extern "C" void ri_exception_set_storage(void *);
#endif
