/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_TEXT_H
#define SPEECH_TEXT_H
#include <stddef.h>
#include <stdbool.h>
typedef enum { SPEECH_TEXT_WAIT, SPEECH_TEXT_READY, SPEECH_TEXT_INVALID, SPEECH_TEXT_LONG,
               SPEECH_TEXT_SKIP } speech_text_result_t;
/* Validates the full input and returns the largest complete UTF-8 prefix that
 * fits capacity. Incomplete trailing code points are retained by the caller. */
bool speech_text_prefix(const char *text, size_t length, size_t capacity, size_t *accepted);
/* Split only at natural punctuation. The returned prefix includes closing
 * quotes/brackets; an unfinished final sentence stays in the queue.
 * SKIP consumes a leading whitespace prefix without creating a synthesis task,
 * including while more text is still being appended. */
speech_text_result_t speech_text_unit(const char *text, size_t length, bool finished, size_t *bytes);
#endif
