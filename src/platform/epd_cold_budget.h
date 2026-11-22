/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COLD_BUDGET_H
#define EPD_COLD_BUDGET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { EPD_COLD_DERIVED, EPD_COLD_OPTIONAL, EPD_COLD_REQUIRED } epd_cold_class_t;
typedef uint64_t epd_cold_lease_t;
enum { EPD_COLD_BUDGET_OK, EPD_COLD_BUDGET_BUSY, EPD_COLD_BUDGET_FULL,
       EPD_COLD_BUDGET_IO, EPD_COLD_BUDGET_INVALID };
#define EPD_COLD_BUDGET_RECORDS 48u
#define EPD_COLD_BUDGET_PATH 128u
#ifndef EPD_COLD_BUDGET_VOLUME_BYTES
#define EPD_COLD_BUDGET_VOLUME_BYTES (8u * 1024u * 1024u)
#endif

/* Worker-only, outside storage_lock. Lease covers disk access through close and
 * publication. bytes is the complete new file, reserved in addition to the old
 * file. Zero requests a read lease. REQUIRED is never an eviction candidate.
 * New paths must belong to the caller's cache/state store; no callbacks survive
 * the call. DERIVED declares that all contents at this path are reconstructible.
 * Other unregistered files stay REQUIRED until a reader classifies them. */
int epd_cold_budget_begin(const char *path, epd_cold_class_t kind, size_t bytes,
                          uint32_t session, epd_cold_lease_t *lease);
/* End after closing all file access. Successful reads may classify a validated
 * record; successful writes publish kind. Failure preserves the old record.
 * Deferred owned-file cleanup remains charged in resident metadata. */
int epd_cold_budget_finish(epd_cold_lease_t lease, epd_cold_class_t kind, bool success);
bool epd_cold_budget_temporary(const char *path, char *out, size_t capacity);
typedef struct {
    size_t bytes, reserved, index_bytes, limit;
    unsigned records, active, deferred;
    uint32_t evicted_derived, evicted_optional;
} epd_cold_budget_snapshot_t;
/* Memory-only diagnostic; volume is the storage_volume_t value. */
void epd_cold_budget_snapshot(unsigned volume, epd_cold_budget_snapshot_t *out);
#endif
