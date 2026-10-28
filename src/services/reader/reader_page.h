/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_PAGE_H
#define READER_PAGE_H
#include "reader_records.h"
#include "../../../modules/common/reader/compute/include/epd_reader_layout.h"

typedef struct {
    uint32_t width, height, line_space, base_height, step_limit;
    void *user;
    uint32_t (*font_height)(void *user, uint32_t style);
    uint32_t (*advance)(void *user, uint32_t style, uint32_t letter);
} reader_page_metrics_t;
typedef struct {
    epd_reader_batch_t *batch;
    reader_page_metrics_t metrics;
    epd_read_record_t record;
    epd_read_anchor_t start, position;
    uint32_t generation, ordinal, measured;
    bool loaded, eof, in_flight;
    int error;
} reader_page_t;

/* UI-owned adapter. The existing compute client's buffer holds the batch.
 * init/load/prepare/complete/turn never read files or invoke the layout kernel.
 * Load copies one record, so its source window can be returned immediately.
 * Prepare measures at most glyph_budget letters. OK transfers the batch to
 * the existing compute client; do not access/reinitialize it before completion.
 * Drawing may borrow a completed page until turn/init; the UI must finish
 * drawing before reuse. Images' dimensions must already have been prepared. */
int reader_page_init(reader_page_t *page, epd_reader_batch_t *batch,
                     const reader_page_metrics_t *metrics, uint32_t generation);
int reader_page_load(reader_page_t *page, const epd_read_record_t *record,
                     const void *payload, uint32_t image_width, uint32_t image_height);
int reader_page_end(reader_page_t *page, const epd_read_anchor_t *end);
int reader_page_prepare(reader_page_t *page, unsigned glyph_budget);
/* Called only after the compute client returns the buffer. A generation
 * change discards this completion without updating source position. */
int reader_page_complete(reader_page_t *page, int result);
int reader_page_turn(reader_page_t *page);
#endif
