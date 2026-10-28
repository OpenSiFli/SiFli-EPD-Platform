#ifndef EPD_SYSTEM_INTERNAL_H
#define EPD_SYSTEM_INTERNAL_H

#include "epd_system.h"

bool epd_notification_publish_resident(const epd_notification_t *value);

/* Resident shell snapshot. app_id remains the command target; navigation_id
 * is a copied installed-app identity for display/navigation. Read atomically
 * with the media snapshot. */
bool epd_media_read_display(epd_media_t *out, uint32_t *revision,
                            char *navigation_id, size_t capacity);

/* Resident arbitration: one shared lease, with a copied original session.
 * No module callbacks run here. begin queues PAUSE; ready requires an explicit
 * quiet publication. end restores only the same service/item/user intent.
 * A restore command is checked again when consumed, including quiet state. */
bool epd_media_lease_begin(void);
/* Identity/intent validity is independent of the output-close acknowledgement. */
bool epd_media_lease_valid(void);
bool epd_media_lease_ready(void);
void epd_media_lease_end(bool restore);
/* A resident speech service may cover the parked original media publication.
 * NULL withdraws its overlay without losing the original lease. */
bool epd_media_publish_overlay(epd_service_t *service, epd_service_t *requester,
                               const epd_media_t *value, bool quiet);

/* Resident lifecycle integration. Stopping drops media/queued commands and
 * unfinished notifications and dismissal records owned by that service.
 * Finished notifications remain copied. No module pointers survive stop. */
void epd_system_service_stopped(epd_service_t *service);
void epd_system_remove_app(const char *app_id);
/* UI catalog snapshot, including all installed apps, not only visible notices. */
void epd_system_reconcile_apps(const char (*ids)[EPD_SYSTEM_APP_ID_SIZE], size_t count);

#endif
