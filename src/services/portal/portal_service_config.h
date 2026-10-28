/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PORTAL_SERVICE_CONFIG_H
#define PORTAL_SERVICE_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTAL_SERVICE_CONFIG_MAX_BYTES 16384u

/* HTTP status is returned. Successful reads allocate a redacted response;
 * the caller releases it with epd_app_free(). Mutations copy their jobs into
 * the existing settings workers and return a ticket for completion polling. */
int portal_service_config_read(bool speech, char **output);
int portal_service_config_submit(bool speech, const char *text, size_t length,
                                 char *output, size_t capacity);
int portal_service_config_poll(bool speech, uint32_t ticket, char *output, size_t capacity);
void portal_service_config_wipe(void *data, size_t length);
#endif
