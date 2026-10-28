#ifndef EPD_SYSTEM_H
#define EPD_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct epd_service epd_service_t;
#define EPD_SYSTEM_APP_ID_SIZE 32
#define EPD_NOTIFICATION_ID_SIZE 64
#define EPD_NOTIFICATION_TITLE_SIZE 96
#define EPD_NOTIFICATION_TEXT_SIZE 256
#define EPD_NOTIFICATION_PAGE_SIZE 64

typedef struct
{
    char app_id[EPD_SYSTEM_APP_ID_SIZE];
    char id[EPD_NOTIFICATION_ID_SIZE];
    char title[EPD_NOTIFICATION_TITLE_SIZE];
    char text[EPD_NOTIFICATION_TEXT_SIZE];
    /* Optional SDK intent string parameter "page"; empty opens the app root. */
    char page[EPD_NOTIFICATION_PAGE_SIZE];
    bool ongoing;
} epd_notification_t;

/* Task context, copied synchronously. app_id is supplied by the service owner.
 * All strings must be terminated within their arrays. Allocation failure leaves
 * the previous snapshot unchanged. Each app has at most one visible notification.
 * Within a service lifetime, updating a dismissed ID succeeds silently; a new
 * business event uses a new ID and replaces the app's previous notification.
 * Concurrent tasks are summarized by the application before publication. */
bool epd_notification_publish(epd_service_t *service, const epd_notification_t *value);
uint32_t epd_notification_revision(void);
/* One consistent page, total count and revision. Newest events come first;
 * progress updates do not change event order. */
size_t epd_notification_read(size_t offset, epd_notification_t *out, size_t capacity,
                             size_t *total, uint32_t *revision);
bool epd_notification_find(const char *app_id, const char *id, epd_notification_t *out);
bool epd_notification_dismiss(const char *app_id, const char *id);
size_t epd_notification_app_ids(char (*ids)[EPD_SYSTEM_APP_ID_SIZE], size_t capacity);

typedef enum
{
    EPD_MEDIA_PREVIOUS = 1, EPD_MEDIA_PLAY, EPD_MEDIA_PAUSE, EPD_MEDIA_NEXT,
    EPD_MEDIA_STOP
} epd_media_command_t;
#define EPD_MEDIA_CAN_PREVIOUS (1u << EPD_MEDIA_PREVIOUS)
#define EPD_MEDIA_CAN_PLAY (1u << EPD_MEDIA_PLAY)
#define EPD_MEDIA_CAN_PAUSE (1u << EPD_MEDIA_PAUSE)
#define EPD_MEDIA_CAN_NEXT (1u << EPD_MEDIA_NEXT)
#define EPD_MEDIA_CAN_STOP (1u << EPD_MEDIA_STOP)
typedef struct
{
    char app_id[EPD_SYSTEM_APP_ID_SIZE];
    char artist[96];
    char title[160];
    /* SDK audio_type_t identifying this session's output category. */
    unsigned audio_type;
    uint32_t capabilities;
    bool playing;
} epd_media_t;

/* Latest published session is controlled. NULL withdraws this service's session.
 * Publication never implies playback: the service reports its actual state. */
bool epd_media_publish(epd_service_t *service, const epd_media_t *value);
/* item identifies this concrete playback instance, not its display title.
 * quiet is true only after pause/close completed and output stopped. */
bool epd_media_publish_state(epd_service_t *service, const epd_media_t *value,
                             uint64_t item, bool quiet);
/* Call when accepting a direct user playback/queue/seek command, including a
 * repeated pause. Commands sent by epd_media_send already record this intent. */
void epd_media_note_intent(epd_service_t *service);
/* Worker acknowledges that all accepted user commands have finished. Serialize
 * this call with direct-command admission; do not acknowledge queued work. */
void epd_media_intent_settled(epd_service_t *service);
bool epd_media_read(epd_media_t *out, uint32_t *revision);
/* Commands are copied into a FIFO and wake the service. No module code runs on
 * the caller's thread. The service consumes commands with epd_media_poll(). */
bool epd_media_send(const char *app_id, epd_media_command_t command);
/* Success removes one command with the service's idempotent sleep hold acquired.
 * The consumer keeps that hold throughout command execution, playback and
 * resource cleanup, then calls epd_service_sleep_release() from app_service.h
 * once all service work requiring the hold has ended. Empty/failed polls neither
 * acquire nor release a service hold, leaving any existing playback hold intact.
 * If service hold acquisition fails, the command remains queued. */
bool epd_media_poll(epd_service_t *service, epd_media_command_t *command);


#endif
