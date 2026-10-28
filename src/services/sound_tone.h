/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SOUND_TONE_H
#define SOUND_TONE_H

#include "../ui/platform/epd_sound.h"
#include "../../assets/sounds/epd_tone_assets.h"

/* Initialization is serialized by the sound preference owner. */
void sound_tone_init(void);
bool sound_tone_play(epd_sound_channel_t channel, epd_sound_tone_t tone, bool preview);
void sound_tone_stop_preview(void);
void sound_tone_cancel(void);
/* Resident capture owner: enabling invalidates pending/active tones before
 * returning. Wait for quiet before opening the microphone. Disabling never
 * replays discarded requests. */
void sound_tone_suppress(bool suppress);
bool sound_tone_quiet(void);

#endif
