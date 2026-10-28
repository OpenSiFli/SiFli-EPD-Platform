/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_SETTINGS_DRAFT_H
#define AI_SETTINGS_DRAFT_H
#include "ai_internal.h"

#define AI_DRAFT_PATH "/flash/config/ai-settings-draft.bin"
#define AI_DRAFT_INPUT_SIZE (AI_SECRET_SIZE * 4u)
/* One private settings-page draft, never used for request admission. The UI
 * owns the meaning of view/input identifiers; the worker only stores bytes. */
typedef struct {
    uint32_t revision, present;
    ai_profile_t profile;
    char selected_id[EPD_AI_ID_SIZE];
    uint32_t model, header, mode;
    int32_t input_key;
    uint32_t input_capacity, input_numeric, keep_secret, has_secret, dirty;
    char input[AI_DRAFT_INPUT_SIZE];
} ai_settings_draft_t;

bool ai_settings_draft_valid(const ai_settings_draft_t *draft);
epd_ai_error_t ai_settings_draft_load(ai_settings_draft_t *draft);
epd_ai_error_t ai_settings_draft_store(const ai_settings_draft_t *draft);
#endif
