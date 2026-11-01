#ifndef CRF_FONT_FTZ_H
#define CRF_FONT_FTZ_H
#include "compute_reader_font.h"

typedef struct crf_ftz crf_ftz;
typedef crf_status (*crf_ftz_read_fn)(void *, uint32_t, void *, size_t);
typedef int (*crf_ftz_cancel_fn)(void *);
typedef struct {
    uint64_t block_hits, block_misses, blocks, errors;
    uint64_t input_calls, input_bytes, inflate_calls, inflate_ticks;
    uint32_t inflate_max_ticks;
} crf_ftz_stats;
void crf_ftz_get_stats(const crf_ftz *, crf_ftz_stats *);
size_t crf_ftz_bytes(void);
crf_status crf_ftz_open(crf_ftz *, uint32_t packed_bytes, uint32_t font_bytes,
                      crf_ftz_read_fn, crf_ftz_cancel_fn, void *);
crf_status crf_ftz_read(crf_ftz *, uint32_t offset, void *, size_t);
#endif
