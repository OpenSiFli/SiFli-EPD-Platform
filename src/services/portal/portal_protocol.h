#ifndef EPD_PORTAL_PROTOCOL_H
#define EPD_PORTAL_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTAL_HEADER_MAX 4096
#define PORTAL_TARGET_MAX 1600
#define PORTAL_PATH_MAX 512
#define PORTAL_JSON_MAX 4096
typedef struct
{
    char method[8], target[PORTAL_TARGET_MAX], cookie[512], origin[96], host[64];
    char session_token[65], csrf[65], content_type[80];
    uint64_t length;
    bool has_length, expect_continue;
} portal_request_t;
/* Parses one complete header, with CRLF CRLF at the end. Rejects ambiguous
 * framing, unsupported transfer encoding and duplicate security headers. */
int portal_request_parse(char *header, size_t length, portal_request_t *out);
bool portal_query(const char *target, const char *key, char *out, size_t capacity);
/* 0: absent, 1: decoded, -1: malformed/duplicated. */
int portal_query_value(const char *target, const char *key, char *out, size_t capacity);
bool portal_path(const char *relative, char *absolute, size_t capacity);
/* Apply to every ordinary-file mutation, including recursive parent removal.
 * The font service owns /.epd/fonts; FAT path comparisons ignore ASCII case. */
bool portal_path_mutable(const char *relative);
bool portal_equal_secret(const char *a, const char *b);

typedef struct { char *data; size_t used, capacity; bool failed; } portal_json_t;
void portal_json_init(portal_json_t *json, char *data, size_t capacity);
void portal_json_raw(portal_json_t *json, const char *value);
void portal_json_string(portal_json_t *json, const char *value);
void portal_json_number(portal_json_t *json, int64_t value);
#endif
