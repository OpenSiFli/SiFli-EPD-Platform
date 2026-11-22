#ifndef EPD_APP_BACKGROUND_EVENTS_H
#define EPD_APP_BACKGROUND_EVENTS_H
#include "app_background.h"

/* Resident publication boundary. Snapshot/publish/forget are bounded and may
 * run under the background state guard; none executes application callbacks. */
bool app_background_event_snapshot(epd_memory_owner_t owner, epd_app_background_event_t *out);
void app_background_events_publish(const epd_app_background_event_t *event);
void app_background_events_forget(epd_memory_owner_t owner, uint32_t generation);
#endif
