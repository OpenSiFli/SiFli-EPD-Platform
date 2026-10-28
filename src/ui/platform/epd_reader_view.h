/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_VIEW_H
#define EPD_READER_VIEW_H
#include "epd_reader_session.h"
#include "lvgl.h"

typedef struct {
    bool visible, preparing, at_end;
    int error;
    epd_read_anchor_t position;
} epd_read_view_status_t;

/* UI thread. Bind the singleton body to a caller-owned container whose size
 * is already resolved. The platform owns fonts, layout and child objects.
 * The caller retains navigation ownership. Deleting the container detaches
 * the body; close the session separately when leaving the reading workflow.
 * The first attach of a session begins reading-font selection. Reattaching,
 * resuming and reflowing keep the actual selection until an explicit apply
 * or storage-loss fallback changes it.
 * BUSY means a previous view is returning its compute buffer; retry after
 * the platform service's next UI iteration. */
int epd_read_view_attach(epd_read_session_t session, lv_obj_t *container);
void epd_read_view_detach(epd_read_session_t session);
int epd_read_view_next(epd_read_session_t session);
/* Rebuild at the last submitted source position with current reading fonts,
 * spacing and resolved container size. Repeated requests replace unfinished
 * layout, not visible progress. Old objects retain their fonts until unused. */
int epd_read_view_reflow(epd_read_session_t session);
int epd_read_view_status(epd_read_session_t session, epd_read_view_status_t *status);
#endif
