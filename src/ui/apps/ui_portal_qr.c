#include "ui_portal_qr.h"
#include <string.h>

static bool append(char **next, size_t *remaining, const char *text)
{
    size_t length = strlen(text);
    if (length >= *remaining) return false;
    memcpy(*next, text, length + 1);
    *next += length;
    *remaining -= length;
    return true;
}

static bool field(char **next, size_t *remaining, const char *text)
{
    for (; *text; ++text) {
        char character[2] = {*text, 0};
        if (strchr("\\;,:\"", *text) && !append(next, remaining, "\\")) return false;
        if (!append(next, remaining, character)) return false;
    }
    return true;
}

bool ui_portal_wifi_qr(char *output, size_t capacity, const char *ssid, const char *password)
{
    if (!output || !capacity) return false;
    output[0] = 0;
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password ||
        strlen(password) < 8 || strlen(password) > 63) return false;
    char *next = output;
    size_t remaining = capacity;
    bool ok = append(&next, &remaining, "WIFI:T:WPA;S:") && field(&next, &remaining, ssid) &&
              append(&next, &remaining, ";P:") && field(&next, &remaining, password) &&
              append(&next, &remaining, ";;");
    if (!ok) memset(output, 0, capacity);
    return ok;
}
