/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_RUNTIME_H
#define READER_RUNTIME_H
#include <stdbool.h>
/* UI admission: retain an already loaded application or verify a built-in.
 * Does not start/load another application. drop runs after callbacks return. */
int reader_runtime_pin(const char *app_id, void **module);
void reader_runtime_drop(void *module);
bool reader_runtime_accept(void);
#endif
