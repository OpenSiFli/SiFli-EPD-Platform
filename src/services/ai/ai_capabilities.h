/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAPABILITIES_H
#define AI_CAPABILITIES_H
#include "epd_ai.h"
#include <string.h>

typedef enum {
    AI_SEARCH_SUPPORTED,
    AI_SEARCH_PROTOCOL_REQUIRED,
    AI_SEARCH_DEEPSEEK_RESPONSES_REQUIRED,
    AI_SEARCH_DEEPSEEK_PRO_REQUIRED
} ai_search_support_t;

static inline ai_search_support_t ai_model_search_support(const epd_ai_profile_t *profile,
                                                         const epd_ai_model_t *model)
{
    if (profile->features & EPD_AI_FEATURE_DEEPSEEK) {
        if (profile->protocol != EPD_AI_RESPONSES) return AI_SEARCH_DEEPSEEK_RESPONSES_REQUIRED;
        if (strcmp(model->id, "deepseek-v4-pro")) return AI_SEARCH_DEEPSEEK_PRO_REQUIRED;
    }
    if (profile->protocol != EPD_AI_RESPONSES && profile->protocol != EPD_AI_MESSAGES)
        return AI_SEARCH_PROTOCOL_REQUIRED;
    return AI_SEARCH_SUPPORTED;
}

static inline uint32_t ai_capabilities_with_dependencies(uint32_t capabilities)
{
    return capabilities & EPD_AI_CAP_WEB_SEARCH ? capabilities | EPD_AI_CAP_NATIVE : capabilities;
}
#endif
