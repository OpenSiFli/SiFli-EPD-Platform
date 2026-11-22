/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_JSON_H
#define EPD_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;
typedef enum {
    EPD_JSON_OK = 0,
    EPD_JSON_FORMAT_ERROR,
    EPD_JSON_NO_MEMORY
} epd_json_status_t;

/* Initialize platform cJSON once, after the heaps and before JSON clients run.
 * This does not change the private parsers embedded in application modules. */
void epd_json_init(void);

/* length excludes the terminating NUL; the input need not be NUL-terminated.
 * Reject embedded NULs and trailing non-whitespace. Set *out to NULL on error.
 * The caller owns the returned tree and releases it with epd_json_delete().
 * Preserve the calling thread's errno on every return path. */
epd_json_status_t epd_json_parse(const char *text, size_t length, cJSON **out);
void epd_json_delete(cJSON *json);

#ifdef __cplusplus
}
#endif
#endif
