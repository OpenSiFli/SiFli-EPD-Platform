#ifndef EPD_PORTAL_HTTP_H
#define EPD_PORTAL_HTTP_H
#include "app_service.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const char *address;
    uint32_t netmask; /* Network byte order, from the selected interface. */
    epd_service_t *service;
    void (*ready)(void);
    bool (*keep_running)(void);
    /* Admission only: changing this does not cancel an accepted request. */
    bool (*accepting)(void);
    /* The caller retains its service sleep hold throughout AP operation. */
    bool keep_awake;
} portal_http_config_t;
/* Runs on the portal worker until cancelled or the AP disappears. */
int portal_http_run(const portal_http_config_t *config);
bool portal_http_storage_idle(void);
void portal_http_storage_block(bool blocked);
#endif
