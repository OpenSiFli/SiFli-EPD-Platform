/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_AI_JSON_H
#define EPD_AI_JSON_H
#include "ai_internal.h"
#include "private/ai_cjson.h"
/* Lock around ALL ai_cjson operations, including deleting/printing DOMs.
 * No I/O or application callback may run while this lock is held. */
bool ai_json_begin(void);
void ai_json_end(void);
/* Called with lock held. Strict length/trailing-data/duplicate-key validation,
 * depth <=32 and nodes <=4096 enforced before node allocation. */
ai_cjson *ai_json_parse(const char *text, size_t size, epd_ai_error_t *error);
/* A returned string uses ai_alloc and outlives the JSON lock/DOM. */
char *ai_json_print(const ai_cjson *json, epd_ai_error_t *error);
#endif
