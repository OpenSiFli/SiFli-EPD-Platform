#include "ui_model.h"

ui_radio_state_t ui_radio_state(bool enabled, bool connected)
{
    return !enabled ? UI_RADIO_OFF : connected ? UI_RADIO_CONNECTED : UI_RADIO_DISCONNECTED;
}

int ui_battery_percent(int percent)
{
    if (percent < 0) return 0;
    return percent > 100 ? 100 : percent;
}

ui_battery_icon_t ui_battery_icon(int percent, bool charging)
{
    if (charging) return UI_BATTERY_CHARGING;
    percent = ui_battery_percent(percent);
    if (percent <= 20) return UI_BATTERY_EMPTY;
    if (percent < 80) return UI_BATTERY_MID;
    return UI_BATTERY_FULL;
}
