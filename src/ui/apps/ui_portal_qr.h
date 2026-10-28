#ifndef UI_PORTAL_QR_H
#define UI_PORTAL_QR_H

#include <stdbool.h>
#include <stddef.h>

#define PORTAL_WIFI_QR_CAPACITY 224

bool ui_portal_wifi_qr(char *output, size_t capacity, const char *ssid, const char *password);

#endif
