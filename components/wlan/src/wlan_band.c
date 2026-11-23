#include "wlan_control.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* AIC8800D80N SDK host-message layout. The vendor host library assembles
 * these requests from its regulatory channel table and sends them to firmware. */
enum
{
    SCAN_CHANNEL_LIMIT = 42,
    SCAN_CHANNEL_BYTES = 6,
    SCAN_CHANNEL_COUNT_OFFSET = 367,
    CONNECT_FREQUENCY_OFFSET = 40,
    CONNECT_BAND_OFFSET = 42,
};

static volatile wlan_band_t sta_band = WLAN_BAND_2_4_GHZ;

rt_err_t wlan_adapter_set_sta_band(wlan_band_t band)
{
    if (band != WLAN_BAND_2_4_GHZ && band != WLAN_BAND_DUAL && band != WLAN_BAND_5_GHZ)
        return -RT_EINVAL;
    sta_band = band;
    return RT_EOK;
}

static unsigned channel_frequency(const uint8_t *channel)
{
    return (unsigned)channel[0] | ((unsigned)channel[1] << 8);
}

static bool frequency_allowed(wlan_band_t band, unsigned frequency)
{
    return band == WLAN_BAND_2_4_GHZ ? frequency >= 2412 && frequency <= 2484 :
        frequency >= 5000 && frequency < 5900;
}

extern int __real_rwnx_send_scanu_req(void *hw, uint8_t *request);
extern int __real_rwnx_send_sm_connect_req(void *hw, const uint8_t *request, void *confirmation);

int __wrap_rwnx_send_scanu_req(void *hw, uint8_t *request)
{
    wlan_band_t band = sta_band;
    unsigned count = request[SCAN_CHANNEL_COUNT_OFFSET];
    if (!count || count > SCAN_CHANNEL_LIMIT) return -RT_EINVAL;
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        uint8_t *channel = request + i * SCAN_CHANNEL_BYTES;
        if (band != WLAN_BAND_DUAL && !frequency_allowed(band, channel_frequency(channel))) continue;
        if (kept != i) memcpy(request + kept * SCAN_CHANNEL_BYTES, channel, SCAN_CHANNEL_BYTES);
        ++kept;
    }
    if (!kept) return -RT_EINVAL;
    request[SCAN_CHANNEL_COUNT_OFFSET] = (uint8_t)kept;
#if EPD_INPUT_TEST_ENABLED
    unsigned first = channel_frequency(request);
    unsigned last = channel_frequency(request + (kept - 1) * SCAN_CHANNEL_BYTES);
    rt_kprintf("wlan scan band=%u channels=%u/%u first=%u last=%u\n", band, kept, count, first, last);
#endif
    return __real_rwnx_send_scanu_req(hw, request);
}

int __wrap_rwnx_send_sm_connect_req(void *hw, const uint8_t *request, void *confirmation)
{
    wlan_band_t band = sta_band;
    unsigned frequency = channel_frequency(request + CONNECT_FREQUENCY_OFFSET);
    if (band != WLAN_BAND_DUAL && (!frequency_allowed(band, frequency) ||
        request[CONNECT_BAND_OFFSET] != (band == WLAN_BAND_5_GHZ ? 1 : 0))) return -RT_EINVAL;
#if EPD_INPUT_TEST_ENABLED
    rt_kprintf("wlan join band=%u frequency=%u\n", band, frequency);
#endif
    return __real_rwnx_send_sm_connect_req(hw, request, confirmation);
}
