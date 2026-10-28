/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_VIEW_UI_H
#define READER_VIEW_UI_H
#include "reader_core.h"
/* One bounded UI step; true requests another step. Worker completion wakes
 * the same service. This entry never performs file or parser operations. */
bool reader_view_ui_process(reader_core_t *core, bool stopping);
#endif
