/* SPDX-License-Identifier: Apache-2.0 */
#include "system_time_internal.h"
#include "network_time.h"
#include "epd_memory.h"
#include "network.h"
#include "sleep_guard.h"
#include "system_time.h"
#include <lwip/api.h>
#include <lwip/sockets.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define DBG_TAG "net.time"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define NTP_PORT 123
#define NTP_TIMEOUT_MS 5000
#define SYNC_RETRY_MS 30000
#define SYNC_ATTEMPTS 3u
#define NTP_EPOCH_DELTA 2208988800ULL

static struct rt_event wakeup;
static rt_thread_t worker;
static bool initializing, event_ready, suspended;
static uint32_t session;
static rt_tick_t suspend_started;
static rt_err_t suspend_status = -RT_ERROR;
static sleep_guard_owner_t time_hold = {.id = "network_time"};

/* These short scheduler-locked sections serialize thread-context entry points
 * with the UI's sleep preparation. No DNS, socket or RTC operation runs here. */
static void wake_locked(void)
{
    if (event_ready) rt_event_send(&wakeup, 1);
}

static void suspend_deadline_locked(void)
{
    if (suspend_status == -RT_EBUSY &&
        (rt_tick_t)(rt_tick_get() - suspend_started) >=
        rt_tick_from_millisecond(NETWORK_TIME_SUSPEND_TIMEOUT_MS))
        suspend_status = -RT_ETIMEOUT;
}

static void parked_locked(void)
{
    if (!suspended || initializing) return;
    suspend_deadline_locked();
    if (suspend_status == -RT_EBUSY) suspend_status = RT_EOK;
}

static bool begin_work(uint32_t *current_session)
{
    sleep_guard_snapshot_t guard;
    rt_enter_critical();
    sleep_guard_snapshot(&guard);
    /* Acquiring during PREPARING would cancel the whole sleep attempt. Leave
     * that state untouched; suspend/resume will deliver the next worker event. */
    bool accepted = !initializing && !suspended && guard.state == SLEEP_GUARD_AWAKE &&
                    sleep_guard_acquire(&time_hold);
    *current_session = session;
    if (!accepted) parked_locked();
    rt_exit_critical();
    return accepted;
}

static bool session_current(uint32_t expected)
{
    rt_enter_critical();
    bool current = !suspended && session == expected;
    rt_exit_critical();
    return current;
}

static rt_int32_t finish_work(uint32_t expected, rt_int32_t wait)
{
    rt_enter_critical();
    sleep_guard_release(&time_hold);
    if (suspended)
    {
        parked_locked();
        wait = RT_WAITING_FOREVER;
    }
    else if (session != expected) wait = 0;
    rt_exit_critical();
    return wait;
}

static bool link_snapshot(uint32_t *generation)
{
    network_snapshot_t state;
    network_snapshot(&state);
    *generation = state.generation;
    return state.ready;
}

static bool same_link(uint32_t generation)
{
    uint32_t current;
    return link_snapshot(&current) && current == generation;
}

static bool transaction_current(uint32_t generation, uint32_t token, uint32_t expected)
{
    return session_current(expected) && same_link(generation) &&
           system_time_ntp_current(token) && session_current(expected);
}

/* A private SNTP transaction avoids the SDK client's shared packet/netif state. */
static rt_err_t request_time(uint32_t generation, uint32_t token, uint32_t expected,
                             const ip4_addr_t *local_address, int64_t *utc)
{
    ip_addr_t server_ip;
    struct sockaddr_in address;
    uint32_t request[12] = {0}, response[12];
    rt_err_t result = -RT_ERROR;
    const char *stage = "dns";
    int fd = -1, detail = 0, nonblocking = 1;
    rt_tick_t start;

    if (!transaction_current(generation, token, expected)) goto done;
    /* lwIP retains this call's stack-backed completion state until DNS returns.
     * Keep time_hold throughout; a stop deadline reports failure, not cleanup. */
    detail = netconn_gethostbyname(NETUTILS_NTP_HOSTNAME, &server_ip);
    if (detail != ERR_OK) goto done;
    if (!transaction_current(generation, token, expected)) goto done;

    stage = "socket";
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { detail = errno; goto done; }
    if (!transaction_current(generation, token, expected)) goto done;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) { detail = errno; goto done; }
    if (!transaction_current(generation, token, expected)) goto done;

    /* Pin the request to the selected address, without retaining a netif pointer. */
    stage = "bind";
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ip4_addr_get_u32(local_address);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    { detail = errno; goto done; }
    if (!transaction_current(generation, token, expected)) goto done;

    stage = "connect";
    address.sin_port = htons(NTP_PORT);
    address.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&server_ip));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    { detail = errno; goto done; }
    if (!transaction_current(generation, token, expected)) goto done;

    /* VN=4, mode=client. The server must echo this transaction's transmit stamp. */
    request[0] = htonl(0x23000000u);
    request[10] = htonl((uint32_t)(NTP_EPOCH_DELTA + SYSTEM_TIME_MIN_UTC + generation));
    request[11] = htonl(rt_tick_get());
    stage = "send";
    if (send(fd, request, sizeof(request), 0) != (int)sizeof(request))
    { detail = errno; goto done; }

    stage = "receive";
    result = -RT_ETIMEOUT;
    start = rt_tick_get();
    while (transaction_current(generation, token, expected) &&
           (rt_tick_t)(rt_tick_get() - start) < rt_tick_from_millisecond(NTP_TIMEOUT_MS))
    {
        fd_set readfds;
        struct timeval timeout = {0, 200000};
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        int count = select(fd + 1, &readfds, RT_NULL, RT_NULL, &timeout);
        if (!transaction_current(generation, token, expected)) break;
        if (count < 0)
        {
            if (errno == EINTR) continue;
            detail = errno;
            result = -RT_ERROR;
            break;
        }
        if (!count) continue;
        count = recv(fd, response, sizeof(response), 0);
        if (!transaction_current(generation, token, expected)) break;
        if (count < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            detail = errno;
            result = -RT_ERROR;
            break;
        }
        if (count < (int)sizeof(response)) continue;
        uint32_t header = ntohl(response[0]);
        unsigned version = (header >> 27) & 7;
        unsigned stratum = (header >> 16) & 255;
        if ((version != 3 && version != 4) || ((header >> 24) & 7) != 4 ||
            response[6] != request[10] || response[7] != request[11]) continue;
        /* Stratum zero is a server denial/rate-limit response; stop this session. */
        if (!stratum)
        {
            stage = "server-denied";
            result = -RT_EBUSY;
            break;
        }
        if ((header >> 30) == 3 || stratum > 15 || (!response[10] && !response[11])) continue;

        uint64_t seconds = ntohl(response[10]);
        if (seconds < NTP_EPOCH_DELTA) seconds += 1ULL << 32; /* NTP era rollover in 2036. */
        seconds -= NTP_EPOCH_DELTA;
        if (!system_time_utc_valid((int64_t)seconds)) continue;
        *utc = (int64_t)seconds;
        result = RT_EOK;
        break;
    }

done:
    if (fd >= 0) closesocket(fd);
    if (result != RT_EOK && transaction_current(generation, token, expected))
        LOG_W("NTP failed: stage=%s result=%d detail=%d", stage, result, detail);
    return result;
}

static void time_worker(void *parameter)
{
    uint32_t generation = 0, sync_token = 0, sync_session = 0;
    unsigned attempts = 0;
    bool synced = false;
    rt_tick_t last_attempt = 0;
    (void)parameter;

    while (1)
    {
        uint32_t events, current_session;
        if (!begin_work(&current_session))
        {
            rt_event_recv(&wakeup, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &events);
            continue;
        }
        uint32_t current;
        bool connected = link_snapshot(&current);
        uint32_t token = system_time_ntp_token();
        if (current != generation || token != sync_token || current_session != sync_session)
        {
            generation = current;
            sync_token = token;
            sync_session = current_session;
            attempts = 0;
            synced = false;
        }
        bool active = token && connected && !synced && attempts < SYNC_ATTEMPTS;
        network_snapshot_t route;
        network_snapshot(&route);
        ip4_addr_t local_address = {route.address};
        if (active && (!attempts || (rt_tick_t)(rt_tick_get() - last_attempt) >=
                                   rt_tick_from_millisecond(SYNC_RETRY_MS)) && route.ready &&
            transaction_current(generation, token, current_session))
        {
            int64_t utc;
            attempts++;
            rt_err_t result = request_time(generation, token, current_session, &local_address, &utc);
            if (transaction_current(generation, token, current_session))
            {
                if (result == RT_EOK) synced = system_time_apply_ntp(token, utc);
                if (result == -RT_EBUSY) attempts = SYNC_ATTEMPTS;
                if (!synced && attempts == SYNC_ATTEMPTS)
                    LOG_W("Time sync stopped; retry on next network connection");
            }
            last_attempt = rt_tick_get();
        }
        rt_int32_t wait = RT_WAITING_FOREVER;
        if (token && connected && !synced && attempts < SYNC_ATTEMPTS)
        {
            rt_tick_t elapsed = rt_tick_get() - last_attempt;
            rt_tick_t interval = rt_tick_from_millisecond(SYNC_RETRY_MS);
            wait = elapsed < interval ? interval - elapsed : 0;
        }
        wait = finish_work(current_session, wait);
        rt_event_recv(&wakeup, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, wait, &events);
    }
}

rt_err_t network_time_init(void)
{
    rt_enter_critical();
    if (initializing || (!worker && suspended))
    {
        rt_exit_critical();
        return -RT_EBUSY;
    }
    if (worker)
    {
        rt_exit_critical();
        return RT_EOK;
    }
    initializing = true;
    rt_exit_critical();
    rt_err_t loaded = system_time_init();
    rt_err_t result = rt_event_init(&wakeup, "time_w", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK) goto done;
    rt_enter_critical();
    event_ready = true;
    rt_exit_critical();
    uint32_t subscription = network_subscribe(&wakeup, 1);
    worker = subscription ? epd_thread_create_psram("net_time", time_worker, RT_NULL, 4096, 23, 10) : NULL;
    if (worker)
    {
        result = rt_thread_startup(worker);
        if (result == RT_EOK)
        {
            result = loaded;
            goto done;
        }
        rt_thread_delete(worker);
        worker = RT_NULL;
    }
    else result = -RT_ENOMEM;
    network_unsubscribe(subscription);
    rt_enter_critical();
    event_ready = false;
    rt_exit_critical();
    rt_event_detach(&wakeup);
done:
    rt_enter_critical();
    initializing = false;
    if (!worker) parked_locked();
    wake_locked();
    rt_exit_critical();
    return result;
}

void network_time_settings_changed(void)
{
    rt_enter_critical();
    if (!suspended) wake_locked();
    rt_exit_critical();
}

rt_err_t network_time_suspend(void)
{
    rt_enter_critical();
    if (!suspended)
    {
        suspended = true;
        ++session;
        suspend_started = rt_tick_get();
        suspend_status = -RT_EBUSY;
        if (!initializing && !worker) parked_locked();
        wake_locked();
    }
    rt_exit_critical();
    return RT_EOK;
}

rt_err_t network_time_suspend_result(void)
{
    rt_enter_critical();
    if (suspended) suspend_deadline_locked();
    rt_err_t result = suspend_status;
    rt_exit_critical();
    return result;
}

void network_time_resume(void)
{
    rt_enter_critical();
    suspended = false;
    suspend_status = -RT_ERROR;
    /* PREPARING can park the worker before suspend() is requested. Always wake
     * it on cancellation, preserving the session of any work not suspended. */
    wake_locked();
    rt_exit_critical();
}
