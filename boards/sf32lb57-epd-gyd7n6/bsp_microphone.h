/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BSP_EPD_MICROPHONE_H
#define BSP_EPD_MICROPHONE_H

#define BSP_MICROPHONE_DEFAULT_GAIN_HALF_DB 24

/* Thread context: enable before opening the SDK PDM recording client.
 * Disable only after audio_close() or AUDIO_CTL_STOP has completed. Both
 * microphones share this supply and one stereo PDM1 data line. */
void BSP_Microphone_PowerOn(void);
void BSP_Microphone_PowerOff(void);

/* Call after each SDK recording client open/resume, before accepting samples.
 * The SDK configures its own initial gain on open. Units are 0.5 dB, valid
 * range -30..90; the board's default gain is 24 (12 dB).
 * Returns zero on success, a negative RT-Thread error otherwise. */
int BSP_Microphone_SetGain(int gain_half_db);

#endif
