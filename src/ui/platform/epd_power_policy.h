#ifndef EPD_POWER_POLICY_H
#define EPD_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t started_at, timeout, revision;
    bool timing, failed;
} epd_power_policy_t;

/* All times are monotonic milliseconds. Zero timeout means never. A changed
 * owner revision starts a complete interval, including release of the last lock. */
bool epd_power_policy_update(epd_power_policy_t *policy, uint32_t now,
                             bool eligible, uint32_t revision, uint32_t timeout);
void epd_power_policy_activity(epd_power_policy_t *policy, uint32_t now);
void epd_power_policy_fail(epd_power_policy_t *policy, uint32_t timeout);
uint32_t epd_power_policy_delay(const epd_power_policy_t *policy, uint32_t now);

#endif
