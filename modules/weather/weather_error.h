/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WEATHER_ERROR_H
#define WEATHER_ERROR_H

#include <cJSON.h>
#include <string.h>
#include "weather_retry.h"

/* Legacy Geo/weather codes are three decimal digits, encoded as strings. */
static inline int weather_api_code(const cJSON *root)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 3) return 0;
    const char *value = item->valuestring;
    if (value[0] < '1' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9') return 0;
    return (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
}

static inline int weather_problem_status(const cJSON *root)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(error, "status");
    if (!cJSON_IsNumber(status) || status->valuedouble < 400 || status->valuedouble > 599 ||
        status->valuedouble != status->valueint) return 0;
    return status->valueint;
}

/* Return only known literals; server text and URLs can echo request credentials. */
static inline const char *weather_problem_type(const cJSON *root)
{
    static const char prefix[] = "https://dev.qweather.com/docs/resource/error-code/#";
    static const char *const types[] = {
        "invalid-parameter", "invalid-parameters", "missing-parameter", "missing-parameters",
        "no-such-location", "data-not-available", "unauthorized", "no-credit", "overdue",
        "security-restriction", "invalid-host", "account-suspension", "deprecated",
        "forbidden", "not-found", "method-not-allowed", "too-many-requests",
        "over-monthly-limit", "unknown-error",
    };
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(error, "type");
    if (cJSON_IsString(type) && !strncmp(type->valuestring, prefix, sizeof(prefix) - 1))
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
            if (!strcmp(type->valuestring + sizeof(prefix) - 1, types[i])) return types[i];
    return "unknown";
}

static inline weather_error_t weather_classify_api(const cJSON *root)
{
    const char *type = weather_problem_type(root);
    if (!strcmp(type, "over-monthly-limit") || !strcmp(type, "no-credit") ||
        !strcmp(type, "overdue") || !strcmp(type, "account-suspension"))
        return WEATHER_ERROR_REJECTED;
    int status = weather_problem_status(root);
    if (!status) status = weather_api_code(root);
    return weather_classify_status(status);
}

#endif
