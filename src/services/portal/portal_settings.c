#include "system_time_internal.h"
#include "platform/epd_locale_internal.h"
#include "ui_settings_internal.h"
#include "portal.h"
#include "portal_protocol.h"
#include "portal_package.h"
#include "epd_memory.h"
#include "ui_settings.h"
#include "platform/epd_locale.h"
#include "platform/epd_sound.h"
#include "platform/epd_lock.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "frontlight.h"
#include "system_time.h"
#include "device_identity.h"
#include "wallpaper.h"
#include "usb_card_reader.h"
#include "system_reset.h"
#include "wlan_service.h"
#include "bt_pan.h"
#include "network.h"
#include "sleep_guard.h"
#include <lwip/inet.h>
#include <cJSON.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define SETTINGS_CAPACITY 49152
typedef enum { WAIT_NONE, WAIT_WLAN, WAIT_BT, WAIT_USB, WAIT_WALLPAPER, WAIT_INSTALL } wait_kind_t;
typedef struct
{
    struct rt_mutex lock;
    char *snapshot;
    char request[PORTAL_JSON_MAX];
    uint32_t ticket;
    bool pending, success;
    bool applied, delivered, reset_queued;
    bool package_request;
    wait_kind_t waiting;
    uint32_t install_ticket;
    rt_tick_t accepted;
    rt_tick_t published;
    system_reset_target_t reset_target;
    unsigned reset_token;
    rt_tick_t reset_expiry;
    bool reset_prepared;
    char result[384];
} settings_bridge_t;
static settings_bridge_t *bridge;
static unsigned serial;
static sleep_guard_owner_t settings_hold = {.id = "portal-settings"};

bool portal_settings_init(void)
{
    if (!portal_package_init()) return false;
    if (bridge) return true;
    bridge = epd_app_alloc(sizeof(*bridge), EPD_APP_PSRAM);
    if (!bridge) return false;
    memset(bridge, 0, sizeof(*bridge));
    bridge->delivered=true;
    if (rt_mutex_init(&bridge->lock, "webcfg", RT_IPC_FLAG_PRIO) != RT_EOK)
    { epd_app_free(bridge); bridge = NULL; return false; }
    return true;
}
void portal_settings_deinit(void)
{
    if (!bridge) return;
    if (bridge->package_request && bridge->pending && !bridge->applied)
        portal_package_abort("安装请求未执行：网页服务已停止。");
    rt_mutex_detach(&bridge->lock); epd_app_free(bridge->snapshot); epd_app_free(bridge); bridge = NULL;
    sleep_guard_release(&settings_hold);
}
static char *clone(const char *text)
{
    if (!text) return NULL;
    size_t n = strlen(text) + 1;
    char *out = epd_app_alloc(n, EPD_APP_PSRAM);
    if (out) memcpy(out, text, n);
    return out;
}
char *portal_settings_read(void)
{
    if (!bridge) return NULL;
    rt_mutex_take(&bridge->lock, RT_WAITING_FOREVER);
    char *out = clone(bridge->snapshot);
    rt_mutex_release(&bridge->lock); return out;
}
static rt_err_t submit(const char *body, uint32_t *ticket, const char *path, unsigned volume)
{
    if (!bridge || !body || !ticket || strlen(body) >= PORTAL_JSON_MAX) return -RT_EINVAL;
    rt_mutex_take(&bridge->lock, RT_WAITING_FOREVER);
    rt_err_t result = -RT_EBUSY;
    if (!bridge->pending && !bridge->reset_queued && (bridge->delivered ||
        (rt_tick_t)(rt_tick_get()-bridge->accepted)>rt_tick_from_millisecond(20000)))
    {
        if (!sleep_guard_acquire(&settings_hold))
        { rt_mutex_release(&bridge->lock); return -RT_EBUSY; }
        if (path && !portal_package_queue(path,(storage_volume_t)volume))
        { sleep_guard_release(&settings_hold); rt_mutex_release(&bridge->lock); return -RT_EINVAL; }
        strcpy(bridge->request, body);
        bridge->package_request = path != NULL;
        bridge->pending = true; bridge->success = false; bridge->result[0] = 0;
        bridge->applied=false; bridge->delivered=false; bridge->waiting=WAIT_NONE; bridge->accepted=rt_tick_get();
        bridge->ticket = ++serial; if (!serial) bridge->ticket = ++serial;
        *ticket = bridge->ticket; result = RT_EOK;
    }
    rt_mutex_release(&bridge->lock); return result;
}
rt_err_t portal_settings_submit(const char *body, uint32_t *ticket)
{ return submit(body,ticket,NULL,0); }
rt_err_t portal_settings_install(const char *path, unsigned volume, uint32_t *ticket)
{
    if (!path || strlen(path)>=PORTAL_PACKAGE_PATH_MAX || volume>=STORAGE_COUNT) return -RT_EINVAL;
    char request[512]; portal_json_t j; portal_json_init(&j,request,sizeof(request));
    portal_json_raw(&j,"{\"id\":\"app_install\",\"value\":{\"path\":"); portal_json_string(&j,path);
    portal_json_raw(&j,",\"volume\":"); portal_json_number(&j,volume); portal_json_raw(&j,"}}");
    return j.failed ? -RT_EINVAL : submit(request,ticket,path,volume);
}
char *portal_settings_result(uint32_t ticket)
{
    if (!bridge) return NULL;
    rt_mutex_take(&bridge->lock, RT_WAITING_FOREVER);
    char *out = ticket == bridge->ticket ? clone(bridge->result[0] ? bridge->result : "{\"state\":\"pending\"}") : NULL;
    rt_mutex_release(&bridge->lock); return out;
}
void portal_settings_delivered(uint32_t ticket)
{
    if (!bridge) return;
    rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
    if (ticket==bridge->ticket && !bridge->pending) bridge->delivered=true;
    rt_mutex_release(&bridge->lock);
}
static void field(portal_json_t *j, const char *name, const char *value)
{ portal_json_string(j, name); portal_json_raw(j, ":"); portal_json_string(j, value); }
static void begin(portal_json_t *j, const char *id, const char *label, const char *type)
{
    if (j->used && j->data[j->used - 1] == '}') portal_json_raw(j, ",");
    portal_json_raw(j, "{"); field(j, "id", id); portal_json_raw(j, ","); field(j, "label", label);
    portal_json_raw(j, ","); field(j, "type", type);
}
static void group(portal_json_t *j, const char *id, const char *label, bool first)
{
    portal_json_raw(j, first ? "{\"groups\":[{" : "]},{"); field(j, "id", id);
    portal_json_raw(j, ","); field(j, "label", label); portal_json_raw(j, ",\"items\":[");
}
static void boolean(portal_json_t *j, const char *id, const char *label, bool value, bool disabled)
{
    begin(j,id,label,"toggle"); portal_json_raw(j, value ? ",\"value\":true" : ",\"value\":false");
    if (disabled) portal_json_raw(j, ",\"disabled\":true");
    portal_json_raw(j, "}");
}
static void text(portal_json_t *j, const char *id, const char *label, const char *value, const char *type)
{
    begin(j,id,label,type); portal_json_raw(j, ","); field(j, "value", value); portal_json_raw(j, "}");
}
static void range(portal_json_t *j, const char *id, const char *label, unsigned value, bool disabled)
{
    begin(j,id,label,"range"); portal_json_raw(j, ",\"value\":"); portal_json_number(j,value);
    portal_json_raw(j, ",\"unit\":\"%\""); if (disabled) portal_json_raw(j, ",\"disabled\":true"); portal_json_raw(j,"}");
}
static void choice(portal_json_t *j, const char *id, const char *label, int value,
                   const char *const *labels, unsigned count, bool disabled)
{
    begin(j,id,label,"select"); portal_json_raw(j, ",\"value\":"); portal_json_number(j,value);
    if (disabled) portal_json_raw(j, ",\"disabled\":true");
    portal_json_raw(j, ",\"options\":[");
    for (unsigned i = 0; i < count; ++i)
    {
        if (i) portal_json_raw(j,","); portal_json_raw(j,"{\"value\":"); portal_json_number(j,i);
        portal_json_raw(j,","); field(j,"label",labels[i]); portal_json_raw(j,"}");
    }
    portal_json_raw(j,"]}");
}
static void frontlight_controls(portal_json_t *j, const frontlight_snapshot_t *light)
{
    bool disabled = !light->initialized || !light->available || light->suspended;
    range(j,"frontlight","前光亮度",light->committed_percent,disabled);
    const char *const modes[] = {"混合","冷光","暖光"};
    choice(j,"frontlight_mode","前光模式",light->mode,modes,3,
           disabled || !light->preference_valid || !light->mode_available);
}
static void generic(portal_json_t *j, const char *id, ui_setting_id_t key)
{
    const ui_setting_t *setting = ui_settings_item(key);
    if (setting->is_switch) { boolean(j,id,ui_settings_label(key),ui_settings_index(key)!=0,false); return; }
    begin(j,id,ui_settings_label(key),"select"); portal_json_raw(j,",\"value\":"); portal_json_number(j,ui_settings_index(key));
    portal_json_raw(j,",\"options\":[");
    for (unsigned i=0;i<setting->option_count;++i)
    {
        if (i) portal_json_raw(j,","); portal_json_raw(j,"{\"value\":"); portal_json_number(j,i);
        portal_json_raw(j,","); field(j,"label",ui_settings_option(key,i)); portal_json_raw(j,"}");
    }
    portal_json_raw(j,"]}");
}
static void action(portal_json_t *j, const char *id, const char *label, const char *verb)
{ begin(j,id,label,"action"); portal_json_raw(j,","); field(j,"action",verb); portal_json_raw(j,"}"); }
static void wireless(portal_json_t *j)
{
    bool exclusive=false;
    wlan_snapshot_t wlan={0};
    if (wlan_service_snapshot(&wlan)==RT_EOK)
    {
        boolean(j,"wlan_auto","自动连接已保存网络",wlan.automatic,wlan.busy);
        exclusive=wlan.hotspot.active && !wlan.hotspot.concurrent;
        begin(j,"wlan","WLAN","networks");
        if (exclusive || wlan.busy) portal_json_raw(j,",\"disabled\":true");
        portal_json_raw(j,",\"networks\":[");
        for (unsigned i=0;i<wlan.count;++i)
        {
            const wlan_network_t *network=&wlan.networks[i];
            if (i) portal_json_raw(j,","); portal_json_raw(j,"{");
            field(j,"name",network->id.ssid);
            portal_json_raw(j,network->connected?",\"connected\":true":",\"connected\":false");
            portal_json_raw(j,network->saved?",\"saved\":true":",\"saved\":false");
            portal_json_raw(j,network->secure?",\"secure\":true":",\"secure\":false");
            portal_json_raw(j,"}");
        }
        portal_json_raw(j,"]}"); wlan_service_snapshot_free(&wlan);
    }
    wlan_ipv4_state_t ipv4;
    if (wlan_service_get_ipv4(&ipv4)==RT_EOK)
    {
        begin(j,"ipv4","IP 设置","ipv4");
        if (exclusive) portal_json_raw(j,",\"disabled\":true");
        portal_json_raw(j,",\"value\":{");
        portal_json_raw(j,ipv4.config.dhcp?"\"dhcp\":true":"\"dhcp\":false");
        const uint32_t addresses[]={ipv4.config.address,ipv4.config.netmask,ipv4.config.gateway,ipv4.config.dns[0],ipv4.config.dns[1]};
        const char *const ids[]={"address","netmask","gateway","dns1","dns2"};
        for (unsigned i=0;i<5;++i) { char address[16]; struct in_addr ip={.s_addr=addresses[i]}; inet_ntoa_r(ip,address,sizeof(address)); portal_json_raw(j,","); field(j,ids[i],address); }
        portal_json_raw(j,"}}");
    }
    btpan_snapshot_t bt={0};
    if (btpan_snapshot(&bt)==RT_EOK)
    {
        begin(j,"bt","蓝牙设备","devices"); portal_json_raw(j,",\"devices\":[");
        for (unsigned i=0;i<bt.count;++i)
        {
            const btpan_device_t *device=&bt.devices[i]; char address[18];
            snprintf(address,sizeof(address),"%02x:%02x:%02x:%02x:%02x:%02x",device->address[0],device->address[1],device->address[2],device->address[3],device->address[4],device->address[5]);
            if (i) portal_json_raw(j,","); portal_json_raw(j,"{"); field(j,"name",device->name&&*device->name?device->name:address);
            portal_json_raw(j,","); field(j,"address",address);
            portal_json_raw(j,device->connected?",\"connected\":true":" ,\"connected\":false");
            portal_json_raw(j,device->paired?",\"paired\":true}":",\"paired\":false}");
        }
        portal_json_raw(j,"]}"); btpan_snapshot_free(&bt);
    }
}
static char *make_snapshot(void)
{
    char *out = epd_app_alloc(SETTINGS_CAPACITY, EPD_APP_PSRAM); if (!out) return NULL;
    portal_json_t j; portal_json_init(&j,out,SETTINGS_CAPACITY);
    frontlight_snapshot_t light; frontlight_get(&light);
    group(&j,"wireless","无线与连接",true); generic(&j,"bluetooth",UI_SETTING_BLUETOOTH);
    network_snapshot_t route; network_snapshot(&route);
    text(&j,"active_network","当前网络", !route.ready ? "未连接" : route.source == NETWORK_WIFI ? "WLAN" : "Bluetooth PAN", "info");
    wireless(&j);
    usb_card_reader_status_t usb; usb_card_reader_status(&usb);
    boolean(&j,"usb_reader","TF 卡读卡器",usb.enabled,!usb.supported || usb.busy);
    group(&j,"display","显示与亮度",false);
    frontlight_controls(&j,&light);
    generic(&j,"scale",UI_SETTING_UI_SCALE); generic(&j,"refresh",UI_SETTING_FULL_REFRESH);
    group(&j,"lock","锁屏与墙纸",false); generic(&j,"lock_timeout",UI_SETTING_TIMEOUT); generic(&j,"sleep_timeout",UI_SETTING_SYSTEM_SLEEP);
    begin(&j,"lockscreen","锁屏","select"); portal_json_raw(&j,","); field(&j,"value",epd_lock_selected());
    portal_json_raw(&j,",\"options\":[{\"value\":\"\",\"label\":\"默认锁屏\"}");
    for (unsigned i=0;i<app_catalog_count();++i)
    {
        const epd_app_entry_t *app=app_catalog_get(i); if (!app || !app->lockscreen) continue;
        portal_json_raw(&j,",{"); field(&j,"value",app->id); portal_json_raw(&j,","); field(&j,"label",app->name); portal_json_raw(&j,"}");
    }
    portal_json_raw(&j,"]}");
    wallpaper_status_t wallpaper; wallpaper_get_status(&wallpaper);
    begin(&j,"wallpaper","墙纸文件","text");
    portal_json_raw(&j,",\"value\":\"\"");
    portal_json_raw(&j,wallpaper.custom?",\"custom\":true":",\"custom\":false");
    portal_json_raw(&j,wallpaper.busy?",\"busy\":true":",\"busy\":false");
    if (!wallpaper.ready || wallpaper.busy) portal_json_raw(&j,",\"disabled\":true");
    portal_json_raw(&j,"}"); action(&j,"wallpaper_default","默认墙纸","恢复");
    group(&j,"sound","声音",false); epd_sound_snapshot_t sound; memset(&sound,0,sizeof(sound)); epd_sound_read(&sound);
    const char *const volume_ids[]={"volume_master","volume_media","volume_notification","volume_touch"};
    const char *const volume_labels[]={"主音量","媒体音量","通知音量","触摸音量"};
    for (unsigned i=0;i<4;++i) range(&j,volume_ids[i],volume_labels[i],sound.volume[i],!sound.initialized);
    boolean(&j,"mute","静音",sound.muted,!sound.initialized);
    const char *const notifications[]={"无","Argon","Beryllium","Ceti"};
    choice(&j,"notification_tone","通知音",epd_sound_get_tone(EPD_SOUND_NOTIFICATION),notifications,4,false);
    const char *const touches[]={"无","Tick","Keypress"};
    unsigned tone=epd_sound_get_tone(EPD_SOUND_TOUCH);
    choice(&j,"touch_tone","触摸音",tone==EPD_TONE_TICK?1:tone==EPD_TONE_KEYPRESS?2:0,touches,3,false);
    group(&j,"applications","应用程序",false);
    begin(&j,"applications","应用程序","applications"); portal_json_raw(&j,",\"applications\":[");
    unsigned app_count=0;
    for (unsigned i=0;i<app_catalog_count();++i)
    {
        const epd_app_entry_t *app=app_catalog_get(i);
        if (!app || !app->version) continue;
        if (app_count++) portal_json_raw(&j,","); portal_json_raw(&j,"{"); field(&j,"id",app->id);
        portal_json_raw(&j,","); field(&j,"name",app->name);
        portal_json_raw(&j,",\"volume\":"); portal_json_number(&j,app->volume); portal_json_raw(&j,"}");
    }
    portal_json_raw(&j,"]}");
    group(&j,"time","日期与时间",false); system_time_settings_t clock; system_time_get_settings(&clock);
    boolean(&j,"time_auto","自动设置时间",clock.automatic,false); boolean(&j,"hour24","24 小时制",clock.hour24,false);
    begin(&j,"timezone","时区","select"); portal_json_raw(&j,",\"value\":"); portal_json_number(&j,clock.timezone_minutes);
    portal_json_raw(&j,",\"options\":[");
    for (int minutes=-720;minutes<=840;minutes+=15)
    {
        char label[24]; int magnitude=minutes<0?-minutes:minutes;
        snprintf(label,sizeof(label),"UTC%c%02d:%02d",minutes<0?'-':'+',magnitude/60,magnitude%60);
        if (minutes!=-720) portal_json_raw(&j,","); portal_json_raw(&j,"{\"value\":"); portal_json_number(&j,minutes);
        portal_json_raw(&j,","); field(&j,"label",label); portal_json_raw(&j,"}");
    }
    portal_json_raw(&j,"]}");
    if (!clock.automatic)
    {
        struct tm now; char time[32]=""; if (system_time_get_local(&now)) strftime(time,sizeof(time),"%Y-%m-%dT%H:%M",&now);
        text(&j,"datetime","日期与时间",time,"datetime");
    }
    group(&j,"reset","重置",false); action(&j,"prepare_factory","恢复出厂数据","重置"); action(&j,"prepare_network","重置 WLAN 和蓝牙","重置"); action(&j,"prepare_card","格式化 TF 卡","格式化");
    group(&j,"device","设备",false); char name[DEVICE_IDENTITY_NAME_CAPACITY]; name[0]=0; device_identity_get_name(name,sizeof(name));
    text(&j,"name","设备名称",name,"text"); const char *const languages[]={"简体中文","English"};
    choice(&j,"language","语言",!strcmp(epd_locale_get(),"en_us"),languages,2,false); generic(&j,"touch",UI_SETTING_TOUCH);
    portal_json_raw(&j,"]}]}");
    if (j.failed) { epd_app_free(out); return NULL; } return out;
}
static bool integer(const cJSON *item, int *out)
{
    if (cJSON_IsBool(item)) { *out=cJSON_IsTrue(item); return true; }
    if (cJSON_IsNumber(item) && item->valuedouble>=INT_MIN && item->valuedouble<=INT_MAX && item->valuedouble==item->valueint)
    { *out=item->valueint; return true; }
    if (cJSON_IsString(item) && item->valuestring[0])
    {
        char *end; errno=0; long n=strtol(item->valuestring,&end,10);
        if (errno==ERANGE || *end || n<INT_MIN || n>INT_MAX) return false; *out=(int)n; return true;
    }
    return false;
}
static bool apply(const char *id, const cJSON *value, char *extra, size_t capacity)
{
    if (system_reset_busy() || storage_changing()) return false;
    int n=0; bool numeric=integer(value,&n); const char *string=cJSON_IsString(value)?value->valuestring:NULL;
    if (!strcmp(id,"wlan_scan")) { bridge->waiting=WAIT_WLAN; return wlan_service_scan()==RT_EOK; }
    if (!strcmp(id,"wlan_connect"))
    {
        const cJSON *ssid=cJSON_GetObjectItemCaseSensitive(value,"ssid"),*password=cJSON_GetObjectItemCaseSensitive(value,"password");
        if (!cJSON_IsString(ssid) || (password && !cJSON_IsString(password))) return false;
        bridge->waiting=WAIT_WLAN;
        if (!password)
        {
            wlan_snapshot_t state={0}; bool accepted=false;
            if (wlan_service_snapshot(&state)==RT_EOK)
            {
                for (unsigned i=0;i<state.count;++i)
                    if (state.networks[i].saved && !strcmp(state.networks[i].id.ssid,ssid->valuestring))
                    { accepted=wlan_service_connect(&state.networks[i].id,NULL)==RT_EOK; break; }
                wlan_service_snapshot_free(&state);
            }
            return accepted;
        }
        return wlan_service_connect_ssid(ssid->valuestring,password?password->valuestring:NULL)==RT_EOK;
    }
    if (!strcmp(id,"wlan_disconnect")) { bridge->waiting=WAIT_WLAN; return wlan_service_disconnect()==RT_EOK; }
    if (!strcmp(id,"wlan_forget") && string)
    {
        wlan_snapshot_t wlan={0}; bool ok=false;
        if (wlan_service_snapshot(&wlan)==RT_EOK)
        {
            for (unsigned i=0;i<wlan.count;++i) if (!strcmp(wlan.networks[i].id.ssid,string) && wlan.networks[i].saved)
            { ok=wlan_service_forget(&wlan.networks[i].id)==RT_EOK; break; }
            wlan_service_snapshot_free(&wlan);
        }
        bridge->waiting=WAIT_WLAN; return ok;
    }
    if (!strcmp(id,"ipv4") && cJSON_IsObject(value))
    {
        wlan_ipv4_config_t config={0}; cJSON *dhcp=cJSON_GetObjectItemCaseSensitive(value,"dhcp");
        if (!cJSON_IsBool(dhcp)) return false; config.dhcp=cJSON_IsTrue(dhcp);
        if (!config.dhcp)
        {
            uint32_t *addresses[]={&config.address,&config.netmask,&config.gateway,&config.dns[0],&config.dns[1]};
            const char *const ids[]={"address","netmask","gateway","dns1","dns2"};
            for (unsigned i=0;i<5;++i)
            {
                cJSON *address=cJSON_GetObjectItemCaseSensitive(value,ids[i]); struct in_addr parsed;
                if (!cJSON_IsString(address) || !inet_aton(address->valuestring,&parsed)) return false;
                *addresses[i]=parsed.s_addr;
            }
        }
        bridge->waiting=WAIT_WLAN; return wlan_service_set_ipv4(&config)==RT_EOK;
    }
    if (!strcmp(id,"bt_scan")) { bridge->waiting=WAIT_BT; return btpan_scan()==RT_EOK; }
    if (!strcmp(id,"app_install"))
    {
        cJSON *path=cJSON_GetObjectItemCaseSensitive(value,"path"),*volume=cJSON_GetObjectItemCaseSensitive(value,"volume");
        int target;
        if (!bridge->package_request || !cJSON_IsString(path) || !integer(volume,&target) || target<0 || target>=STORAGE_COUNT) return false;
        bridge->waiting=WAIT_INSTALL;
        return portal_package_start(path->valuestring,(storage_volume_t)target,&bridge->install_ticket);
    }
    if (!strcmp(id,"app_uninstall"))
    {
        cJSON *app=cJSON_GetObjectItemCaseSensitive(value,"id"),*volume=cJSON_GetObjectItemCaseSensitive(value,"volume");
        int target;
        if (!cJSON_IsString(app) || !integer(volume,&target) || target<0 || target>=STORAGE_COUNT) return false;
        bridge->waiting=WAIT_INSTALL;
        return app_installer_start_uninstall(app->valuestring,(storage_volume_t)target,&bridge->install_ticket);
    }
    if (!strcmp(id,"bt_connect") || !strcmp(id,"bt_disconnect") || !strcmp(id,"bt_forget"))
    {
        if (!string || strlen(string)!=17) return false;
        unsigned bytes[6]; int consumed=0;
        if (sscanf(string,"%2x:%2x:%2x:%2x:%2x:%2x%n",bytes,bytes+1,bytes+2,bytes+3,bytes+4,bytes+5,&consumed)!=6 || consumed!=17) return false;
        uint8_t address[6]; for (unsigned i=0;i<6;++i) address[i]=(uint8_t)bytes[i];
        bridge->waiting=WAIT_BT;
        return (!strcmp(id,"bt_connect")?btpan_connect(address):!strcmp(id,"bt_disconnect")?btpan_disconnect(address):btpan_unpair(address))==RT_EOK;
    }
    static const struct { const char *id; ui_setting_id_t key; } generic_keys[]={
        {"scale",UI_SETTING_UI_SCALE},{"refresh",UI_SETTING_FULL_REFRESH},{"lock_timeout",UI_SETTING_TIMEOUT},
        {"sleep_timeout",UI_SETTING_SYSTEM_SLEEP},{"bluetooth",UI_SETTING_BLUETOOTH},{"touch",UI_SETTING_TOUCH}};
    for (unsigned i=0;i<sizeof(generic_keys)/sizeof(*generic_keys);++i)
        if (!strcmp(id,generic_keys[i].id)) return numeric && n>=0 && ui_settings_set(generic_keys[i].key,(unsigned)n);
    if (!strcmp(id,"frontlight")) return numeric && n>=0 && n<=100 && frontlight_commit((uint8_t)n);
    if (!strcmp(id,"frontlight_mode")) return numeric && n>=0 && n<3 && frontlight_set_mode((frontlight_mode_t)n);
    if (!strcmp(id,"language")) return numeric && (n==0 || n==1) && epd_locale_set(n?"en_us":"zh_cn");
    if (!strcmp(id,"name")) return string && device_identity_set_name(string)==RT_EOK;
    if (!strcmp(id,"lockscreen")) return string && epd_lock_select(string);
    if (!strcmp(id,"wallpaper")) { char path[PORTAL_PATH_MAX]; bridge->waiting=WAIT_WALLPAPER; return string && portal_path(string,path,sizeof(path)) && wallpaper_import(path)==RT_EOK; }
    if (!strcmp(id,"wallpaper_default")) { bridge->waiting=WAIT_WALLPAPER; return wallpaper_use_default()==RT_EOK; }
    const char *const volume_ids[]={"volume_master","volume_media","volume_notification","volume_touch"};
    for (unsigned i=0;i<4;++i) if (!strcmp(id,volume_ids[i])) return numeric && n>=0 && n<=100 && epd_sound_set_volume((epd_sound_channel_t)i,(unsigned)n);
    if (!strcmp(id,"mute")) return cJSON_IsBool(value) && epd_sound_set_muted(cJSON_IsTrue(value));
    if (!strcmp(id,"notification_tone") || !strcmp(id,"touch_tone"))
    {
        bool touch=!strcmp(id,"touch_tone");
        if (!numeric || n<0 || n>=(touch?3:4)) return false;
        epd_sound_channel_t channel=touch?EPD_SOUND_TOUCH:EPD_SOUND_NOTIFICATION;
        epd_sound_tone_t tone=touch?(n==1?EPD_TONE_TICK:n==2?EPD_TONE_KEYPRESS:EPD_TONE_NONE):(epd_sound_tone_t)n;
        if (!epd_sound_set_tone(channel,tone)) return false;
        (void)epd_sound_preview(channel,tone);
        return true;
    }
    if (!strcmp(id,"wlan_auto")) { bridge->waiting=WAIT_WLAN; return cJSON_IsBool(value) && wlan_service_set_autoconnect(cJSON_IsTrue(value))==RT_EOK; }
    if (!strcmp(id,"usb_reader")) { bridge->waiting=WAIT_USB; return cJSON_IsBool(value) && usb_card_reader_set_enabled(cJSON_IsTrue(value)); }
    if (!strcmp(id,"time_auto")) return cJSON_IsBool(value) && system_time_set_automatic(cJSON_IsTrue(value));
    if (!strcmp(id,"hour24")) return cJSON_IsBool(value) && system_time_set_hour24(cJSON_IsTrue(value));
    if (!strcmp(id,"timezone")) return numeric && system_time_set_timezone(n);
    if (!strcmp(id,"datetime"))
    {
        struct tm calendar={0}; int year,month,day,hour,minute,read=0;
        if (!string || strlen(string)!=16 || sscanf(string,"%4d-%2d-%2dT%2d:%2d%n",&year,&month,&day,&hour,&minute,&read)!=5 || string[read] || year<1970 || year>2099) return false;
        calendar.tm_year=year-1900; calendar.tm_mon=month-1; calendar.tm_mday=day; calendar.tm_hour=hour; calendar.tm_min=minute;
        return system_time_set_local(&calendar);
    }
    if (!strcmp(id,"prepare_factory") || !strcmp(id,"prepare_network") || !strcmp(id,"prepare_card"))
    {
        system_reset_kind_t kind=!strcmp(id,"prepare_factory")?SYSTEM_RESET_FACTORY:!strcmp(id,"prepare_network")?SYSTEM_RESET_NETWORK:SYSTEM_RESET_CARD;
        if (system_reset_prepare(kind,&bridge->reset_target)!=RT_EOK) return false;
        bridge->reset_prepared=true; bridge->reset_token=++serial; bridge->reset_expiry=rt_tick_get()+rt_tick_from_millisecond(60000);
        snprintf(extra,capacity,",\"confirmation\":%u,\"message\":\"%s\"",bridge->reset_token,
            kind==SYSTEM_RESET_FACTORY?"将清除系统设置、全部已安装应用及应用数据，包括当前 TF 卡中的应用数据。":kind==SYSTEM_RESET_CARD?"将删除当前 TF 卡中的全部文件。":"将清除已保存的 WLAN 和蓝牙连接。当前连接将断开。");
        return true;
    }
    if (!strcmp(id,"reset_confirm"))
    {
        bool valid=numeric && bridge->reset_prepared && (unsigned)n==bridge->reset_token && (rt_int32_t)(bridge->reset_expiry-rt_tick_get())>0;
        bridge->reset_prepared=false;
        bridge->reset_queued=valid;
        if (valid) snprintf(extra,capacity,",\"disconnect\":true");
        return valid;
    }
    return false;
}
void portal_settings_process(bool stopping)
{
    if (!bridge) return;
    rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
    bool pending=bridge->pending;
    bool reset=bridge->reset_queued && bridge->delivered;
    rt_mutex_release(&bridge->lock);
    if (reset && !stopping)
    {
        rt_err_t error=system_reset_start(&bridge->reset_target);
        rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
        bridge->reset_queued=false;
        if (error!=RT_EOK)
        {
            strcpy(bridge->result,"{\"state\":\"failed\",\"message\":\"重置未开始\"}");
        }
        rt_mutex_release(&bridge->lock);
    }
    if (pending && (!bridge->applied || (stopping && bridge->waiting!=WAIT_INSTALL)))
    {
        const char *end=NULL;
        cJSON *request=cJSON_ParseWithOpts(bridge->request,&end,1);
        cJSON *id=request?cJSON_GetObjectItemCaseSensitive(request,"id"):NULL;
        cJSON *value=request?cJSON_GetObjectItemCaseSensitive(request,"value"):NULL;
        char extra[320]="";
        bool success=!stopping && cJSON_IsString(id) && value && apply(id->valuestring,value,extra,sizeof(extra));
        if (!success && bridge->package_request) portal_package_abort("安装请求未执行：设备忙碌或网页服务已停止。");
        cJSON_Delete(request);
        rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
        bridge->applied=true;
        snprintf(bridge->result,sizeof(bridge->result),success?"{\"state\":\"done\"%s}":"{\"state\":\"failed\",\"message\":\"设置未保存\"}",extra);
        bridge->success=success; bridge->pending=success && bridge->waiting!=WAIT_NONE; bridge->published=0;
        if (!bridge->pending && bridge->package_request) bridge->delivered=true;
        if (bridge->pending) strcpy(bridge->result,"{\"state\":\"pending\"}");
        memset(bridge->request,0,sizeof(bridge->request));
        rt_mutex_release(&bridge->lock);
    }
    if (bridge->pending && bridge->applied)
    {
        bool done=false,success=false;
        if (bridge->waiting==WAIT_WLAN)
        {
            wlan_snapshot_t state={0};
            if (wlan_service_snapshot(&state)==RT_EOK) { done=!state.busy; success=state.error==RT_EOK; wlan_service_snapshot_free(&state); }
        }
        else if (bridge->waiting==WAIT_BT)
        {
            btpan_snapshot_t state={0};
            if (btpan_snapshot(&state)==RT_EOK) { done=!state.busy; success=state.error==RT_EOK; btpan_snapshot_free(&state); }
        }
        else if (bridge->waiting==WAIT_USB)
        { usb_card_reader_status_t state; usb_card_reader_status(&state); done=!state.busy; success=state.error==RT_EOK; }
        else if (bridge->waiting==WAIT_WALLPAPER)
        { wallpaper_status_t state; wallpaper_get_status(&state); done=!state.busy; success=state.error==RT_EOK; }
        else if (bridge->waiting==WAIT_INSTALL)
        {
            app_job_result_t state; app_installer_result(&state);
            done=state.ticket!=bridge->install_ticket || state.state!=APP_JOB_RUNNING;
            success=state.ticket==bridge->install_ticket && state.state==APP_JOB_DONE;
            rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
            portal_json_t j; portal_json_init(&j,bridge->result,sizeof(bridge->result));
            portal_json_raw(&j,"{\"state\":"); portal_json_string(&j,done?(success?"done":"failed"):"pending");
            portal_json_raw(&j,",\"phase\":"); portal_json_string(&j,app_installer_phase_name(state.phase));
            portal_json_raw(&j,",\"completed_bytes\":"); portal_json_number(&j,(int64_t)state.completed_bytes);
            portal_json_raw(&j,",\"total_bytes\":"); portal_json_number(&j,(int64_t)state.total_bytes);
            portal_json_raw(&j,",\"message\":"); portal_json_string(&j,done?app_installer_message(state.message):""); portal_json_raw(&j,"}");
            rt_mutex_release(&bridge->lock);
        }
        if (done)
        {
            rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
            if (bridge->waiting!=WAIT_INSTALL)
                strcpy(bridge->result,success?"{\"state\":\"done\"}":"{\"state\":\"failed\",\"message\":\"操作未完成\"}");
            bridge->pending=false; bridge->success=success; bridge->published=0;
            if (bridge->package_request) bridge->delivered=true;
            rt_mutex_release(&bridge->lock);
        }
    }
    if (!stopping && (!bridge->published || (rt_tick_t)(rt_tick_get()-bridge->published)>=rt_tick_from_millisecond(1000)))
    {
        char *snapshot=make_snapshot();
        if (snapshot)
        {
            rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER); char *old=bridge->snapshot; bridge->snapshot=snapshot; rt_mutex_release(&bridge->lock);
            epd_app_free(old); bridge->published=rt_tick_get();
        }
    }
    /* HTTP may already be idle; retain queued work through its UI completion. */
    rt_mutex_take(&bridge->lock,RT_WAITING_FOREVER);
    if (!bridge->pending) sleep_guard_release(&settings_hold);
    rt_mutex_release(&bridge->lock);
}
