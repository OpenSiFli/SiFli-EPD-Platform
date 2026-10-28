#ifndef EPD_PORTAL_H
#define EPD_PORTAL_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { PORTAL_OFF, PORTAL_STARTING, PORTAL_ON, PORTAL_STOPPING, PORTAL_ERROR } portal_phase_t;
typedef struct
{
    portal_phase_t phase;
    rt_err_t error;
    uint32_t revision;
    unsigned clients;
    bool concurrent;
    bool lan, lan_available, hotspot_requested;
    char ssid[33], password[17], address[48], url[96];
    char lan_address[48], lan_url[96];
} portal_snapshot_t;

/* UI-thread polling automatically serves a ready station interface. */
void portal_process(void);
/* These controls own the hotspot; station access follows the Wi-Fi link. */
rt_err_t portal_start(void);
void portal_stop(void);
void portal_snapshot(portal_snapshot_t *out);
/* UI sleep handoff: gate new LAN work, then poll until sockets and worker
 * resources are drained before suspending WLAN. Active requests finish under
 * their sleep hold; an explicitly requested hotspot remains awake. */
bool portal_prepare_sleep(void);
/* UI wake/abort, after cancelling the sleep guard and resuming WLAN. */
void portal_resume(void);
/* UI shutdown rollback. Inside board_power_request(), prepare after accepting
 * the request and before publishing pending. Idempotently remember the hotspot
 * request and credentials. Internal shutdown stops retain
 * this snapshot; an explicit portal_stop() discards the restoration intent.
 * Cancel after the board power phase returns to IDLE. It restores the request
 * and reuses saved credentials for one start attempt, without changing the
 * user's Wi-Fi switch. If credentials were not yet generated, generate new
 * ones normally. Repeated cancel calls do nothing. */
#if defined(RT_USING_WIFI) && defined(EPD_USE_LVGL)
void portal_power_prepare(void);
void portal_power_cancel(void);
#else
static inline void portal_power_prepare(void) {}
static inline void portal_power_cancel(void) {}
#endif
/* UI storage handoff: reject new file operations and wait for open handles. */
bool portal_release_storage(void);
/* HTTP worker and UI callback exchange bounded requests. Returned JSON is
 * allocated in PSRAM and released with epd_app_free(). */
char *portal_settings_read(void);
bool portal_settings_init(void);
void portal_settings_deinit(void);
rt_err_t portal_settings_submit(const char *body, uint32_t *ticket);
/* Internal upload handoff; the path must belong to the completed upload claim. */
rt_err_t portal_settings_install(const char *path, unsigned volume, uint32_t *ticket);
char *portal_settings_result(uint32_t ticket);
void portal_settings_delivered(uint32_t ticket);
void portal_settings_process(bool stopping);
#endif
