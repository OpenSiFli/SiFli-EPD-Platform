#include "wlan_radio_power.h"
#include "wifi_api.h"

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
extern int __real_aic_wifi_init(int mode, void *parameter);

int __wrap_aic_wifi_init(int mode, void *parameter)
{
    int result = __real_aic_wifi_init(mode, parameter);
    if (result < 0) return result;
    /* The polling SDIO board needs an awake module after every firmware boot,
     * including recovery. The SDK demo applies this only on its first boot.
     * System deep sleep still closes the radio through its normal lifecycle. */
    int policy_result = aic_wifi_set_lp_level(0, 0);
    return policy_result == 0 ? result : policy_result < 0 ? policy_result : -RT_EIO;
}
#endif
