/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_FLOW_H
#define READER_FLOW_H
#include "platform/epd_reader_session.h"
#include "reader_core.h"
#include "reader_page.h"
#include "../../../modules/common/reader/compute/include/reader_compute_client.h"

typedef struct {
    reader_core_t *core;
    reader_session_id_t session;
    reader_page_metrics_t metrics;
    reader_page_t page;
    reader_compute_job_t *job;
    reader_work_token_t work;
    reader_window_lease_t input;
    size_t offset;
    epd_read_anchor_t end;
    uint32_t generation;
    bool closing, ready, presented;
    int error;
} reader_flow_t;

/* UI-owned, one flow for the platform's active view. step performs at most
 * one record transfer or bounded glyph preparation; compute runs exclusively
 * on the existing service worker. The ready page remains immutable until
 * next/seek/close. The view must copy it before returning control to drawing. */
int reader_flow_init(reader_flow_t *flow, reader_core_t *core, reader_session_id_t session,
                     const reader_page_metrics_t *metrics);
bool reader_flow_step(reader_flow_t *flow);
const epd_reader_page_t *reader_flow_page(const reader_flow_t *flow);
int reader_flow_present(reader_flow_t *flow);
int reader_flow_next(reader_flow_t *flow);
int reader_flow_seek(reader_flow_t *flow, const epd_read_anchor_t *position);
/* Unbind drawing first. false means an accepted compute request still owns
 * its buffer. Keep calling step until closed before freeing this object. */
bool reader_flow_close(reader_flow_t *flow);
bool reader_flow_closed(const reader_flow_t *flow);
#endif
