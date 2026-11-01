/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_INTERNAL_H
#define COMMON_READER_INTERNAL_H
#include "reader.h"
#include "../../books/document/document.h"
void epd_reader_set_error_code(reader_error_t error);

/* UI adapter entry. Takes document ownership, including on failure. The
 * document owns callback user lifetime. Reopen returns a replacement session. */
bool epd_reader_attach(document_t *document, const char *title,
                   const reader_position_t *restore, reader_save_fn save,
                   document_t *(*reopen)(void *user, char *error, size_t size), void *user);
/* Stop and join the independent view worker from the service worker before
 * module unload. collect disposes retired font shells on the UI thread. */
void epd_reader_view_worker_shutdown(void);
void epd_reader_worker_wake(void); /* Memory-only notification to document/layout worker. */
/* Publish frame/error/final-count changes to UI; no-op outside the worker lifetime. */
void epd_reader_ui_wake(void);
#endif
