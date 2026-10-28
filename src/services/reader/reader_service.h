/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PLATFORM_READER_SERVICE_H
#define PLATFORM_READER_SERVICE_H
#include "reader_core.h"

/* Platform UI only. The UI adapter retains the calling app and its callback
 * state through reader_owner_t. Apps do not start their own parsing worker.
 * A nonzero result leaves caller-owned request data unchanged. */
int reader_service_open(const reader_open_t *request, reader_session_id_t *id);
reader_core_t *reader_service_core(void);
void reader_service_notify(void);
bool reader_service_supports(const char *format, uint32_t version);
/* UI owner cancels source work before requesting the worker join. */
void reader_service_stop(void);
#endif
