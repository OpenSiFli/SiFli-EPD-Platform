/* SPDX-License-Identifier: Apache-2.0 */
#include "bt_audio.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "bts2_app_inc.h"
#include "bf0_ble_common.h"
#ifdef CFG_HFP_AG
#include "battery.h"
#include "hfp_ag_api.h"
#endif
#ifdef AUDIO_USING_MANAGER
#include <audio_server.h>
#endif

static bt_audio_snapshot_t headset = {.battery_percent = -1};
static bool media_active, sco_active;
static uint16_t sco_channel;
static uint8_t sco_type;

bool bt_audio_is_active(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool active = media_active || sco_active;
    rt_hw_interrupt_enable(level);
#ifdef AUDIO_USING_MANAGER
    active = active || is_a2dp_working();
#endif
    return active;
}

#ifdef CFG_AV_SRC
static bool enabled;
static uint8_t audio_peer[6];

#ifdef CFG_HFP_AG
static uint16_t hfp_channel = UINT16_MAX;
static uint8_t hfp_peer[6];
static int hfp_battery = -1;

static void clear_hfp_peer(void)
{
    if (sco_type == BT_NOTIFY_HFP_AG && sco_channel == hfp_channel) sco_active = false;
    hfp_channel = UINT16_MAX;
    hfp_battery = -1;
    memset(hfp_peer, 0, sizeof(hfp_peer));
    rt_base_t level = rt_hw_interrupt_disable();
    headset.battery_percent = -1;
    rt_hw_interrupt_enable(level);
}

static int hfp_raw_event(uint16_t type, uint16_t event, uint8_t *data, uint32_t context)
{
    (void)context;
    if (type != BTS2M_HFP_AG || event != BTS2MU_AG_AT_CMD_EVENT || !data) return 0;
    const BTS2S_AG_AT_CMD_INFO *at = (const void *)data;
    if (at->mux_id != hfp_channel) return 0;
    if (at->command_id == HFP_AG_BATT_EVT)
    {
        /* IPHONEACCEV key 1 reports levels 0..9. Match Android's ten-percent display. */
        const uint8_t *value = (const uint8_t *)at->str;
        if (value[0] != 1 || value[1] > 9) return 0;
        hfp_battery = ((int)value[1] + 1) * 10;
        rt_base_t level = rt_hw_interrupt_disable();
        if (headset.connected && !memcmp(audio_peer, hfp_peer, 6))
            headset.battery_percent = hfp_battery;
        rt_hw_interrupt_enable(level);
    }
    else if (at->command_id == HFP_AG_AT_UNAT_EVT)
    {
        if (!strncmp(at->str, "AT+XAPL=", 8) || !strncmp(at->str, "+XAPL=", 6))
        {
            char response[] = "+XAPL=EPD,2";
            hfp_ag_send_unknow_at_cmd(at->mux_id, response, sizeof(response) - 1);
            hfp_ag_at_cmd_result(at->mux_id, BTS2_SUCC);
        }
        else
            hfp_ag_at_cmd_result(at->mux_id, BTS2_FAILED);
    }
    return 0;
}
BT_EVENT_REGISTER(hfp_raw_event, NULL);

/* Use the SDK's indicator transport without its simulated phone calls. */
uint8_t bt_hfp_ag_set_auto_answer_call_default(void)
{
    return 0;
}

static void handle_hfp_request(uint16_t event, const uint8_t *data, uint16_t size)
{
    if (!data || !size) return;
    if ((event == BT_NOTIFY_AG_PROFILE_CONNECTED || event == BT_NOTIFY_AG_PROFILE_DISCONNECTED) &&
        size >= sizeof(bt_notify_profile_state_info_t))
    {
        const bt_notify_profile_state_info_t *info = (const void *)data;
        if (event == BT_NOTIFY_AG_PROFILE_CONNECTED && info->res == BTS2_SUCC)
        {
            if (!enabled || (headset.connected && memcmp(audio_peer, info->mac.addr, 6)))
            {
                bt_interface_disc_ext((unsigned char *)info->mac.addr, BT_PROFILE_HFP);
                return;
            }
            if (hfp_channel == info->profile_channel && !memcmp(hfp_peer, info->mac.addr, 6))
                return;
            clear_hfp_peer();
            hfp_channel = info->profile_channel;
            memcpy(hfp_peer, info->mac.addr, 6);
        }
        else if (event == BT_NOTIFY_AG_PROFILE_DISCONNECTED &&
                 info->profile_channel == hfp_channel && !memcmp(info->mac.addr, hfp_peer, 6))
            clear_hfp_peer();
    }
    else if (event == BT_NOTIFY_AG_GET_INDICATOR_STATUS_REQ)
    {
        hfp_cind_status_t indicators = {0};
        int percent = battery_get_percentage();
        if (percent >= 0 && percent <= 100) indicators.batt_level = (uint8_t)(percent / 20);
        bt_interface_get_all_indicator_info_res(data[0], &indicators);
    }
    else if (event == BT_NOTIFY_AG_GET_ALL_REMT_CALLS_INFO_REQ ||
             event == BT_NOTIFY_AG_GET_LOCAL_PHONE_INFO_REQ)
    {
        /* This device has no calls or telephone number. */
        hfp_ag_at_cmd_result(data[0], BTS2_SUCC);
    }
    else if (event == BT_NOTIFY_AG_ANSWER_CALL_REQ || event == BT_NOTIFY_AG_HANGUP_CALL_REQ)
        hfp_ag_at_cmd_result(data[0], BTS2_FAILED);
}
#endif

static void select_output(bool connected)
{
#ifdef AUDIO_USING_MANAGER
    if (audio_server_select_public_audio_device(connected ?
        AUDIO_DEVICE_A2DP_SINK : AUDIO_DEVICE_SPEAKER) != 0)
        rt_kprintf("[bt.audio] output switch failed: headset=%d\n", connected);
#else
    (void)connected;
#endif
}

static void clear_audio_peer(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool connected = headset.connected;
    media_active = false;
    headset.connected = false;
    headset.battery_percent = -1;
    memset(audio_peer, 0, sizeof(audio_peer));
    rt_hw_interrupt_enable(level);
    if (connected) select_output(false);
}
#endif

void bt_audio_get_snapshot(bt_audio_snapshot_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = headset;
#ifdef CFG_AV_SRC
    if (headset.connected) memcpy(out->address, audio_peer, sizeof(out->address));
#endif
    rt_hw_interrupt_enable(level);
}

void bt_audio_set_enabled(bool value)
{
#ifdef CFG_AV_SRC
    enabled = value;
#else
    (void)value;
#endif
}

void bt_audio_handle_event(uint16_t type, uint16_t event,
                           const uint8_t *data, uint16_t size)
{
    rt_base_t activity_level = rt_hw_interrupt_disable();
    if (type == BT_NOTIFY_A2DP)
    {
        if (event == BT_NOTIFY_A2DP_START_IND || event == BT_NOTIFY_A2DP_START_CFM)
            media_active = true;
        else if (event == BT_NOTIFY_A2DP_SUSPEND_IND || event == BT_NOTIFY_A2DP_SUSPEND_CFM)
            media_active = false;
#ifndef CFG_AV_SRC
        else if (event == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED) media_active = false;
#endif
    }
    else if (type == BT_NOTIFY_COMMON)
    {
        if (event == BT_NOTIFY_COMMON_SCO_CONNECTED && data &&
            size >= sizeof(bt_notify_device_sco_info_t) &&
            ((const bt_notify_device_sco_info_t *)data)->sco_res == BTS2_SUCC)
        {
            const bt_notify_device_sco_info_t *info = (const void *)data;
            sco_type = info->sco_type;
            sco_channel = info->profile_channel;
            sco_active = true;
        }
        else if (event == BT_NOTIFY_COMMON_SCO_DISCONNECTED && data &&
                 size >= sizeof(bt_notify_device_sco_info_t))
        {
            const bt_notify_device_sco_info_t *info = (const void *)data;
            if (info->sco_type == sco_type && info->profile_channel == sco_channel)
                sco_active = false;
        }
        else if (event == BT_NOTIFY_COMMON_CLOSE_COMPLETE)
            media_active = sco_active = false;
    }
    rt_hw_interrupt_enable(activity_level);
#ifdef CFG_AV_SRC
    if (type == BT_NOTIFY_A2DP && data && size >= sizeof(bt_notify_profile_state_info_t))
    {
        const bt_notify_profile_state_info_t *info = (const void *)data;
        if (event == BT_NOTIFY_A2DP_PROFILE_CONNECTED && info->res == BTS2_SUCC)
        {
            if (info->profile_role != AV_AUDIO_SRC) return;
            if (!enabled || (headset.connected && memcmp(audio_peer, info->mac.addr, 6)))
            {
                bt_interface_disc_ext((unsigned char *)info->mac.addr, BT_PROFILE_A2DP);
                return;
            }
            if (headset.connected) return;
            rt_base_t level = rt_hw_interrupt_disable();
            memcpy(audio_peer, info->mac.addr, 6);
            headset.connected = true;
            headset.battery_percent = -1;
#ifdef CFG_HFP_AG
            if (hfp_channel != UINT16_MAX && !memcmp(hfp_peer, audio_peer, 6))
                headset.battery_percent = hfp_battery;
#endif
            rt_hw_interrupt_enable(level);
            select_output(true);
#ifdef CFG_HFP_AG
            /* Select A2DP for media and connect HFP for headset indicators. */
            if (hfp_channel == UINT16_MAX || memcmp(hfp_peer, audio_peer, 6))
                bt_interface_conn_to_source_ext((unsigned char *)info->mac.addr, BT_PROFILE_HFP);
#endif
        }
        else if (event == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED && headset.connected &&
                 !memcmp(audio_peer, info->mac.addr, 6))
            clear_audio_peer();
    }
    else if (type == BT_NOTIFY_AVRCP && event == BT_NOTIFY_AVRCP_PROFILE_CONNECTED &&
             data && size >= sizeof(bt_notify_profile_state_info_t))
    {
        const bt_notify_profile_state_info_t *info = (const void *)data;
        if (info->res == BTS2_SUCC && headset.connected && !memcmp(audio_peer, info->mac.addr, 6))
            bt_interface_set_avrcp_role_ext((bt_notify_device_mac_t *)&info->mac, AVRCP_TG);
    }
#ifdef CFG_HFP_AG
    else if (type == BT_NOTIFY_HFP_AG)
        handle_hfp_request(event, data, size);
#endif
    else if (type == BT_NOTIFY_COMMON)
    {
        if (event == BT_NOTIFY_COMMON_CLOSE_COMPLETE)
        {
            clear_audio_peer();
#ifdef CFG_HFP_AG
            clear_hfp_peer();
#endif
        }
        else if (event == BT_NOTIFY_COMMON_ACL_DISCONNECTED && data &&
                 size >= sizeof(bt_notify_device_base_info_t))
        {
            const bt_notify_device_base_info_t *info = (const void *)data;
            if (headset.connected && !memcmp(audio_peer, info->mac.addr, 6)) clear_audio_peer();
#ifdef CFG_HFP_AG
            if (hfp_channel != UINT16_MAX && !memcmp(hfp_peer, info->mac.addr, 6)) clear_hfp_peer();
#endif
        }
    }
#else
    (void)type; (void)event; (void)data; (void)size;
#endif
}
