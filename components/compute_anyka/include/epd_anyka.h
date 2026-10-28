/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_ANYKA_H
#define EPD_ANYKA_H
#include <stdbool.h>
#include "epd_compute_protocol.h"

#define EPD_ANYKA_CAPABILITY "epd/anyka-3a"
#define EPD_ANYKA_VERSION 1u
#define EPD_ANYKA_FRAME_BYTES 320u
#define EPD_ANYKA_MIC_COUNT 4u /* SDK Anyka MAX_MIC_NUM. */
#define EPD_ANYKA_FREQUENCIES 30u /* SDK MAX_SINGLE_FREQUENCY_NUM. */
enum { EPD_ANYKA_UPLINK = 1, EPD_ANYKA_DOWNLINK, EPD_ANYKA_SSL };

/* Pointer-free SRP-SSL parameters. Positions use the SDK's Q15 metres;
 * direction angles use its Q15 * pi/2 convention. */
typedef struct { int32_t x, y, z; } epd_anyka_position_t;
typedef struct { int32_t azimuth, elevation, radius; } epd_anyka_direction_t;
typedef struct
{
    uint16_t dimensions, frequency_mode, frame_samples, sound_speed, sources, snapshots;
    uint32_t min_frequency, max_frequency;
    uint16_t frequency_count, reserved;
    uint32_t frequencies[EPD_ANYKA_FREQUENCIES];
    epd_anyka_position_t microphones[EPD_ANYKA_MIC_COUNT];
    int32_t threshold, level;
} epd_anyka_ssl_config_t;

typedef struct
{
    uint32_t sample_rate, microphones, far_enabled, ssl_enabled;
    epd_anyka_ssl_config_t ssl;
} epd_anyka_config_t;

typedef struct
{
    epd_anyka_config_t config;
    epd_compute_ref_t near_parameters, far_parameters; /* NUL-terminated command lines. */
} epd_anyka_open_t;

typedef struct
{
    uint64_t timestamp_us;
    uint32_t operation;
    epd_compute_ref_t input, reference, output;
} epd_anyka_request_t;

typedef struct
{
    uint64_t timestamp_us;
    int32_t error; /* SDK error if negative; zero if this step succeeded. */
    uint32_t input_consumed, reference_consumed, output_bytes, sources;
    epd_anyka_direction_t directions[EPD_ANYKA_MIC_COUNT - 1];
} epd_anyka_result_t;

typedef struct epd_anyka_client *epd_anyka_t;
int epd_anyka_open(const epd_anyka_config_t *config, const char *near_parameters,
                   const char *far_parameters, epd_compute_purpose_t purpose, epd_anyka_t *client);
/* Cancellation is polled by the compute transport during open and frame waits.
 * The callback context remains valid until close returns. */
int epd_anyka_open_cancelable(const epd_anyka_config_t *config, const char *near_parameters,
    const char *far_parameters, epd_compute_purpose_t purpose,
    bool (*cancelled)(void *context), void *context, epd_anyka_t *client);
/* Uplink: input=320*microphones bytes, reference=320 bytes, output>=320 bytes.
 * Downlink: input=320 bytes, output>=320 bytes; reference unused.
 * SSL: input=frame_samples*microphones*2 bytes, output unused; directions in result.
 * Inspect consumption even when result.error is nonzero; do not replay data
 * already accepted by the algorithm. All PCM is signed 16-bit native endian. */
int epd_anyka_process(epd_anyka_t client, uint32_t operation, uint64_t timestamp_us,
                      const void *input, uint32_t input_bytes,
                      const void *reference, uint32_t reference_bytes,
                      void *output, uint32_t output_bytes, epd_anyka_result_t *result);
int epd_anyka_reset(epd_anyka_t client);
void epd_anyka_cancel(epd_anyka_t client);
int epd_anyka_close(epd_anyka_t client);
typedef char epd_anyka_open_layout[(sizeof(epd_anyka_open_t) == 240) ? 1 : -1];
typedef char epd_anyka_request_layout[(sizeof(epd_anyka_request_t) == 48) ? 1 : -1];
typedef char epd_anyka_result_layout[(sizeof(epd_anyka_result_t) == 64) ? 1 : -1];
#endif
