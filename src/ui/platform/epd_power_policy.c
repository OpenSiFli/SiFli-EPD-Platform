#include "epd_power_policy.h"

bool epd_power_policy_update(epd_power_policy_t *policy, uint32_t now,
                             bool eligible, uint32_t revision, uint32_t timeout)
{
    /* Background events must not restart a failed peripheral transition. */
    if (policy->failed && policy->timeout == timeout) return false;
    policy->failed = false;
    if (!eligible || !timeout)
    {
        policy->timing = false;
        return false;
    }
    if (!policy->timing || policy->revision != revision || policy->timeout != timeout)
    {
        policy->timing = true;
        policy->started_at = now;
        policy->timeout = timeout;
        policy->revision = revision;
    }
    return (uint32_t)(now - policy->started_at) >= timeout;
}

void epd_power_policy_activity(epd_power_policy_t *policy, uint32_t now)
{
    if (policy->failed) policy->timing = false;
    policy->failed = false;
    policy->started_at = now;
}

void epd_power_policy_fail(epd_power_policy_t *policy, uint32_t timeout)
{
    policy->timing = false;
    policy->timeout = timeout;
    policy->failed = true;
}

uint32_t epd_power_policy_delay(const epd_power_policy_t *policy, uint32_t now)
{
    if (!policy->timing) return UINT32_MAX;
    uint32_t elapsed = now - policy->started_at;
    return elapsed >= policy->timeout ? 0 : policy->timeout - elapsed;
}
