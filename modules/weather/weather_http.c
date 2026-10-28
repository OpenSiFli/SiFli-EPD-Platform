/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_http.h"
#include "weather_error.h"
#include "../common/http/epd_http.h"
#include <string.h>
#include <cJSON.h>
#include "system_time.h"
#include <ctype.h>

#define DBG_TAG "weather.http"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static bool cancelled(void *user)
{
    (void)user;
    return weather_cancelled();
}

typedef struct { char retry_after[128], date[64]; } response_headers_t;

static bool header_is(const char *name, const char *expected)
{
    while (*name && *expected)
        if (tolower((unsigned char)*name++) != *expected++) return false;
    return !*name && !*expected;
}

static bool on_header(void *user, const char *name, const char *value)
{
    response_headers_t *headers = user;
    if (header_is(name, "retry-after"))
        rt_snprintf(headers->retry_after, sizeof(headers->retry_after), "%s", value);
    else if (header_is(name, "date"))
        rt_snprintf(headers->date, sizeof(headers->date), "%s", value);
    return !weather_cancelled();
}

static const char *request_name(const char *path)
{
    if (!path) return "unknown";
    if (!strncmp(path, "/geo/v2/city/lookup?", 20)) return "city";
    if (!strncmp(path, "/v7/weather/now?", 15)) return "now";
    if (!strncmp(path, "/v7/weather/7d?", 15)) return "daily";
    if (!strncmp(path, "/airquality/v1/current/", 22)) return "air";
    return "unknown";
}

static void report_failure(const char *request, const char *stage, int status, int native,
                           uint32_t verify, const cJSON *root, char *detail, size_t capacity)
{
    int api = weather_api_code(root), problem = weather_problem_status(root);
    const char *type = weather_problem_type(root);
    LOG_E("request=%s stage=%s HTTP=%d native=%d verify=%08x API=%d problem=%d type=%s",
          request, stage, status, native, (unsigned)verify, api, problem, type);
    if (detail && capacity)
    {
        if (root && (api || problem || strcmp(type, "unknown")))
            rt_snprintf(detail, capacity, "stage=%s HTTP=%d API=%d problem=%d type=%s",
                        stage, status, api, problem, type);
        else
            rt_snprintf(detail, capacity, "stage=%s HTTP=%d native=%d verify=%08x",
                        stage, status, native, (unsigned)verify);
    }
}

char *weather_http_get(const weather_config_t *config, const char *path, int *status,
                       char *detail, size_t detail_capacity, weather_failure_t *failure)
{
    epd_http_response_t response = {0};
    response_headers_t response_headers = {0};
    *failure = (weather_failure_t){WEATHER_ERROR_TRANSIENT, 0};
    char *body = NULL;
    if (status) *status = 0;
    if (detail && detail_capacity) detail[0] = '\0';
    const char *name = request_name(path);
    if (!config || !status || !path)
    {
        failure->error = WEATHER_ERROR_REJECTED;
        report_failure(name, "request-invalid", 0, -RT_EINVAL, 0, NULL, detail, detail_capacity);
        return NULL;
    }
    const epd_http_header_t headers[] = {
        {"X-QW-Api-Key", config->api_key},
        {"Accept", "application/json"},
        {"Accept-Encoding", "gzip"},
    };
    const epd_http_request_t request = {
        .host = config->api_host,
        .path = path,
        .method = "GET",
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .timeout_ms = 30000,
        .body_limit = 32768,
        .cancelled = cancelled,
        .on_header = on_header,
        .user = &response_headers,
    };
    bool complete = epd_http_perform(&request, &response);
    *status = response.status;
    if (response_headers.retry_after[0] || response.status == 429)
        failure->retry_after = weather_retry_after(response_headers.retry_after, response_headers.date,
                                                   system_time_get_utc());
    if (response.status && response.status != 200)
        failure->error = weather_classify_status(response.status);
    if (!complete)
    {
        if (response.verify_flags) failure->error = WEATHER_ERROR_REJECTED;
        report_failure(name, response.stage ? response.stage : "request", response.status,
                       response.detail, response.verify_flags, NULL, detail, detail_capacity);
    }
    else if (response.status != 200)
    {
        cJSON *root = response.body ? cJSON_ParseWithOpts(response.body, NULL, 1) : NULL;
        if (root && weather_classify_api(root) == WEATHER_ERROR_REJECTED)
            failure->error = WEATHER_ERROR_REJECTED;
        report_failure(name, "http-status", response.status, response.detail,
                       response.verify_flags, root, detail, detail_capacity);
        cJSON_Delete(root);
    }
    else if (!response.body || !response.size)
    {
        report_failure(name, "http-body-empty", response.status, 0, 0, NULL, detail, detail_capacity);
    }
    else if (memchr(response.body, '\0', response.size))
    {
        report_failure(name, "json-nul", response.status, 0, 0, NULL, detail, detail_capacity);
    }
    else
    {
        /* Transfer the HTTP layer's PSRAM buffer to the caller. */
        body = response.body;
        failure->error = WEATHER_ERROR_NONE;
        response.body = NULL;
        response.size = 0;
    }
    epd_http_response_clear(&response);
    if (weather_cancelled() && failure->error != WEATHER_ERROR_REJECTED &&
        failure->error != WEATHER_ERROR_RATE_LIMIT)
        failure->error = failure->retry_after ? WEATHER_ERROR_RATE_LIMIT : WEATHER_ERROR_CANCELLED;
    return body;
}
