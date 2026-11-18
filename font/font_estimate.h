#ifndef SYSTEM_FONT_ESTIMATE_H
#define SYSTEM_FONT_ESTIMATE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define SYSTEM_FONT_COMPRESS_FLAGS (TDEFL_WRITE_ZLIB_HEADER|TDEFL_COMPUTE_ADLER32|1500)
typedef bool (*font_estimate_read_t)(void *,uint32_t,void *,uint32_t);
size_t font_estimate_workspace_bytes(void);
/* Estimated TTF bytes only. The caller adds Emoji, block index and resource
 * metadata. This is a sampling estimate, never an admission guarantee. */
bool font_estimate(void *workspace, uint32_t size, font_estimate_read_t read, void *user,
                   uint32_t *packed, uint32_t *sampled);
#endif
