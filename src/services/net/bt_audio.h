/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_BT_AUDIO_H
#define EPD_BT_AUDIO_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool connected;
    int battery_percent; /* -1 until this headset reports a percentage. */
    uint8_t address[6]; /* Audio peer, or all zeroes when disconnected. */
} bt_audio_snapshot_t;

void bt_audio_get_snapshot(bt_audio_snapshot_t *out);
/* Streaming media or an open SCO audio channel prevents Bluetooth suspension. */
bool bt_audio_is_active(void);
/* Called by the Bluetooth service under its control mutex. */
void bt_audio_set_enabled(bool enabled);
void bt_audio_handle_event(uint16_t type, uint16_t event,
                           const uint8_t *data, uint16_t size);
#endif
