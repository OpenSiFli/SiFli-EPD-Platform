/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BSP_SPEECH_PROFILE_H
#define BSP_SPEECH_PROFILE_H

/* Native PDM capture: channel 0 is MIC1, channel 1 is MIC2. */
#ifndef EPD_SPEECH_USE_ANYKA
#define EPD_SPEECH_USE_ANYKA 0
#endif
#ifndef EPD_SPEECH_INPUT_CHANNEL
#define EPD_SPEECH_INPUT_CHANNEL 0
#endif

/* SF32LB57-DevKit-EPD_MB_V1.0 PCB from
 * ProPrj_SF32LB57-DevKit-EPD_2026-10-08.epro2, PCB a76deb11fb27f03e:
 * MIC1 component 8a78373262a9d79e: (1365,1970) mil;
 * MIC2 component c27d41c687a2ca31: (2190,805) mil.
 * Both are top-side footprints rotated 0 degrees. The PDM order is MIC1
 * left, MIC2 right. The origin is the midpoint of the microphones; x/y
 * retain the PCB axes and +z points out of the screen toward the speaker.
 * Coordinates are metres, with a separation of 36.259334 mm. */
#define EPD_SPEECH_EPD57_ARRAY_GEOMETRY "2, -0.0104775 0.0147955 0, 0.0104775 -0.0147955 0"

/* Portrait handheld input: mouth in front of the screen, 15-25 cm away.
 * Use the midpoint distance of 0.20 m along +z. sdfilter.h defines angles
 * in units of pi/2; parse_echo_denc_param converts all three literal values
 * to Q15 directly. Elevation=1 therefore points normal to the mic plane.
 * This is a fixed nominal target, not source tracking or a distance gate. */
#ifndef EPD_SPEECH_EPD57_TARGET_SPHERICAL
#define EPD_SPEECH_EPD57_TARGET_SPHERICAL "0 1 0.20"
#endif

#ifndef EPD_SPEECH_ANYKA_NEAR_PARAMETERS
#define EPD_SPEECH_ANYKA_NEAR_PARAMETERS \
    "--eqLoad=0 --preGain=0dB --aecEna=0 --nearDigiGain=1.0 --farDigiGain=1.0 " \
    "--nrEna=0 --agcEna=0 --volLoad=0 --dencLoad=1 " \
    "--arrayGeometry=\"" EPD_SPEECH_EPD57_ARRAY_GEOMETRY "\" " \
    "--targetSpherical=\"" EPD_SPEECH_EPD57_TARGET_SPHERICAL "\" " \
    "--interfSphericals=\"0\""
#endif
#endif
