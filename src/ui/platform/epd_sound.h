#ifndef EPD_SOUND_H
#define EPD_SOUND_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    EPD_SOUND_MASTER, EPD_SOUND_MEDIA, EPD_SOUND_NOTIFICATION, EPD_SOUND_TOUCH,
    EPD_SOUND_CHANNEL_COUNT
} epd_sound_channel_t;

typedef enum epd_sound_tone_t
{
    EPD_TONE_NONE = 0, EPD_TONE_ARGON = 1, EPD_TONE_BERYLLIUM = 2,
    EPD_TONE_CETI = 3, EPD_TONE_TICK = 4, EPD_TONE_KEYPRESS = 5
} epd_sound_tone_t;

typedef struct
{
    uint8_t volume[EPD_SOUND_CHANNEL_COUNT]; /* User preference, 0..100 percent. */
    uint8_t effective[EPD_SOUND_CHANNEL_COUNT]; /* SDK levels, 0..max_level. */
    uint8_t max_level;
    bool initialized, available, muted, output_ready, persisted;
    uint32_t revision;
} epd_sound_snapshot_t;

/* Application API, thread context only. One owner serializes preferences,
 * storage and SDK writes. read() only copies a memory snapshot.
 * Setters save on explicit changes and restore prior intent/output on failure.
 * Setting a volume does not change the independently saved mute preference. */
bool epd_sound_read(epd_sound_snapshot_t *out);
bool epd_sound_set_volume(epd_sound_channel_t channel, unsigned percent);
bool epd_sound_set_muted(bool muted);
/* Notification: NONE/ARGON/BERYLLIUM/CETI; touch: NONE/TICK/KEYPRESS.
 * NONE disables that channel independently of its saved volume and mute. */
bool epd_sound_tone_valid(epd_sound_channel_t channel, epd_sound_tone_t tone);
epd_sound_tone_t epd_sound_get_tone(epd_sound_channel_t channel);
bool epd_sound_set_tone(epd_sound_channel_t channel, epd_sound_tone_t tone);
/* Returns request acceptance, not audible completion; never saves selection.
 * A new preview replaces the old one. NONE cancels preview successfully.
 * Touch (including preview) is suppressed while published media is playing.
 * Audio open/write/drain/close run on a resident worker, never on this caller. */
bool epd_sound_preview(epd_sound_channel_t channel, epd_sound_tone_t tone);
void epd_sound_preview_stop(void);
void epd_sound_notify(void);
void epd_sound_touch(void);

#endif
