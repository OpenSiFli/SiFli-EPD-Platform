#include "host.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
static CRITICAL_SECTION disk_lock, critical;
static DWORD ui_thread;
static HANDLE entered, proceed;
static HANDLE start_entered, submit_attempt, submit_blocked, acquired, release_acquire, idle_check, release_idle, snapshot_read;
static volatile LONG gate_start, gate_acquire, gate_idle_check;
static DWORD concurrent_producer;
static volatile LONG gate_write, fail_write, held, writes, reads, worker_count, fail_start, sleeping, blocked;
static bool allow_ui_io, available = true;
static unsigned ntp_changes, reminders, rtc_writes, notices, dismissals, locale_changes;
static volatile LONG clock_notifications;
static time_t rtc = (time_t)1767225600 + SYSTEM_TIME_RTC_OFFSET;
static struct { uint8_t data[272]; size_t size, cursor; } disk[PREFERENCE_COUNT];
struct epd_service { HANDLE thread, event; epd_background_t definition; bool held; volatile LONG cancelled; DWORD worker_id; };
static epd_service_t *service;
static void wait_event(HANDLE e) { CHECK(WaitForSingleObject(e, 5000) == WAIT_OBJECT_0); }
static void ui_only(void) { CHECK(GetCurrentThreadId() == ui_thread); }
static void io_only(void) { CHECK(allow_ui_io || GetCurrentThreadId() != ui_thread); }
void rt_enter_critical(void) { EnterCriticalSection(&critical); }
void app_service_ui_signal(uint32_t events) { (void)events; }
void rt_exit_critical(void) { LeaveCriticalSection(&critical); }
void rt_thread_mdelay(int ms) { Sleep((DWORD)ms); }
uint32_t rt_tick_get(void) { return (uint32_t)GetTickCount64(); }
int32_t rt_tick_from_millisecond(int32_t ms) { return ms; }
int rt_mutex_init(struct rt_mutex *m, const char *n, int f) { (void)f; m->name=n; InitializeCriticalSection(&m->lock); return 0; }
int rt_mutex_take(struct rt_mutex *m, int t) {
    if(!strcmp(m->name,"prefsave") && GetCurrentThreadId()==concurrent_producer) {
        SetEvent(submit_attempt);
        if(TryEnterCriticalSection(&m->lock))return 0;
        SetEvent(submit_blocked);
    }
    if(t==RT_WAITING_FOREVER) { EnterCriticalSection(&m->lock); return 0; }
    uint32_t started=rt_tick_get();
    do { if(TryEnterCriticalSection(&m->lock))return 0; Sleep(1); } while(rt_tick_get()-started<(uint32_t)t);
    return -1;
}
int rt_mutex_release(struct rt_mutex *m) { LeaveCriticalSection(&m->lock); return 0; }
static DWORD WINAPI worker(void *p) { epd_service_t *s = p; s->worker_id=GetCurrentThreadId(); s->definition.run(s); InterlockedExchange(&s->cancelled,1); return 0; }
epd_service_t *app_service_start(const char *id, const epd_background_t *d, void *m) {
    ui_only(); CHECK(!strcmp(id, "preferences") && !m && d->flush && d->ui_process && d->stack_size == 3072);
    if (fail_start || service) return NULL;
    LONG startup=InterlockedExchange(&gate_start,0);
    if(startup) { SetEvent(start_entered); wait_event(startup==1?submit_attempt:snapshot_read); }
    epd_service_t *s = calloc(1, sizeof(*s)); CHECK(s); s->definition = *d;
    s->event = CreateEvent(NULL, FALSE, FALSE, NULL); s->thread = CreateThread(NULL, 0, worker, s, 0, NULL); CHECK(s->thread);
    service = s; InterlockedIncrement(&worker_count); return s;
}
uint32_t epd_service_wait(epd_service_t *s, rt_int32_t t) { (void)t; if(epd_service_cancelled(s))return EPD_SERVICE_STOP; CHECK(WaitForSingleObject(s->event, INFINITE) == WAIT_OBJECT_0); return EPD_SERVICE_WAKE; }
bool epd_service_cancelled(epd_service_t *s) {
    CHECK(s);
    if(GetCurrentThreadId()==s->worker_id && InterlockedCompareExchange(&gate_idle_check,0,1)==1) {
        SetEvent(idle_check);wait_event(release_idle);
    }
    return InterlockedCompareExchange(&s->cancelled,0,0)!=0;
}
void epd_service_wake(epd_service_t *s) { SetEvent(s->event); }
bool epd_service_sleep_acquire(epd_service_t *s) {
    CHECK(s);rt_enter_critical();bool accepted=!sleeping&&!s->cancelled;
    if(accepted&&!s->held) { s->held=true;InterlockedIncrement(&held); }
    rt_exit_critical();
    if(accepted && GetCurrentThreadId()==concurrent_producer && InterlockedCompareExchange(&gate_acquire,0,1)==1) {
        SetEvent(acquired);wait_event(release_acquire);
    }
    return accepted;
}
void epd_service_sleep_release(epd_service_t *s) {
    rt_enter_critical();if(!s->cancelled&&s->held) { s->held=false;InterlockedDecrement(&held); }rt_exit_critical();
}
void epd_service_ui_request(epd_service_t *s, uint32_t d) { (void)s; CHECK(d == 0); }
void storage_lock(void) { io_only(); EnterCriticalSection(&disk_lock); }
void storage_unlock(void) { LeaveCriticalSection(&disk_lock); }
bool storage_path_available(const char *p) { (void)p; io_only(); return available; }
bool storage_file_recover(const char *p) { return storage_path_available(p); }
uint32_t storage_file_writers(const char *p) { (void)p; io_only(); return 0; }
bool system_reset_write_allowed(void) { return blocked != 1; }
bool system_reset_blocks_apps(void) { return blocked == 1; }
bool storage_changing(void) { return blocked == 2; }
bool storage_release_requested(void) { return blocked == 3; }
bool board_power_pending(void) { return blocked == 4; }
uint32_t storage_crc32(const void *p, size_t n) { const uint8_t *v = p; uint32_t c = 0; while (n--) c = c * 31u + *v++; return c; }
static unsigned slot(const char *p) { if (strstr(p, "identity")) return PREFERENCE_IDENTITY; if (strstr(p, "time.bin")) return PREFERENCE_TIME; CHECK(strstr(p, "language")); return PREFERENCE_LANGUAGE; }
bool storage_record_save(const char *p, uint32_t magic, uint32_t version, const void *v, size_t n) {
    io_only(); InterlockedIncrement(&writes);
    if (InterlockedCompareExchange(&gate_write, 0, 1)) { SetEvent(entered); wait_event(proceed); }
    if (InterlockedCompareExchange(&fail_write, 0, 1) || !available) return false;
    unsigned i = slot(p); CHECK(n + 16 <= sizeof(disk[i].data));
    uint32_t h[4] = {magic, version, (uint32_t)n, storage_crc32(v, n)};
    memcpy(disk[i].data, h, 16); memcpy(disk[i].data + 16, v, n); disk[i].size = n + 16; return true;
}
bool storage_record_load(const char *p, uint32_t m, uint32_t v, void *out, size_t n) {
    (void)m; (void)v; io_only(); unsigned i = slot(p); ++reads;
    if (!available || disk[i].size != n + 16) return false; memcpy(out, disk[i].data + 16, n); return true;
}
int host_open(const char *p, int f) { (void)f; io_only(); ++reads; unsigned i = slot(p); disk[i].cursor = 0; if (!disk[i].size) { errno = ENOENT; return -1; } return (int)i; }
int host_read(int fd, void *v, size_t n) { io_only(); if (n > disk[fd].size - disk[fd].cursor) n = disk[fd].size - disk[fd].cursor; memcpy(v, disk[fd].data + disk[fd].cursor, n); disk[fd].cursor += n; return (int)n; }
int host_close(int fd) { (void)fd; io_only(); return 0; }
int host_fstat(int fd, struct host_stat *s) { io_only(); s->st_mode = 1; s->st_size = (int64_t)disk[fd].size; return 0; }
int host_stat(const char *p, struct host_stat *s) { int fd = host_open(p, 0); return fd < 0 ? -1 : host_fstat(fd, s); }
rt_device_t rt_device_find(const char *n) { CHECK(!strcmp(n, "rtc")); return (void *)1; }
int rt_device_control(rt_device_t d, int c, void *v) { CHECK(d && c == RT_DEVICE_CTRL_RTC_SET_TIME); rtc = *(time_t *)v; ++rtc_writes; return 0; }
time_t host_time(time_t *v) { if (v) *v = rtc; return rtc; }
struct tm *host_gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) == 0 ? out : NULL; }
void network_time_settings_changed(void) { ++ntp_changes; }
void reminder_time_changed(void) { ++reminders; }
void ui_app_status_changed(void) { InterlockedIncrement(&clock_notifications); }
static const lv_i18n_lang_t chinese = {"zh_cn"}, english = {"en_us"};
const lv_i18n_lang_t *const lv_i18n_lang_pack[] = {&chinese, &english, NULL};
static const char *locale = "zh_cn";
int resource_init(void) { return 0; }
void resource_deinit(void) {}
const char *lv_ext_get_locale(void) { return locale; }
int lv_ext_set_locale(void *m, const char *l) { (void)m; ui_only(); locale = !strcmp(l, "en_us") ? "en_us" : "zh_cn"; return 0; }
void app_modules_set_locale(const char *l) { ui_only(); CHECK(!strcmp(locale, l)); }
void app_catalog_changed(void) { ui_only(); ++locale_changes; }
const char *epd_get_str(int i) { (void)i; ui_only(); return "Save failed"; }
bool epd_notification_publish_resident(const epd_notification_t *n) { ui_only(); CHECK(!strcmp(n->app_id, "settings")); ++notices; return true; }
bool epd_notification_dismiss(const char *a, const char *i) { (void)i; ui_only(); CHECK(!strcmp(a, "settings")); ++dismissals; return true; }
static void poll(void) { if (service) service->definition.ui_process(service, false); }
static void settle(void) { for (unsigned i = 0; i < 5000; ++i) { if (!service || service->definition.flush()) { poll(); return; } poll(); Sleep(1); } CHECK(false); }
static void retire(void) {
    CHECK(service);InterlockedExchange(&service->cancelled,1);epd_service_wake(service);wait_event(service->thread);
    CHECK(service->definition.ui_process(service,true));
    rt_enter_critical();if(service->held) { service->held=false;InterlockedDecrement(&held); }rt_exit_critical();
    CloseHandle(service->thread);CloseHandle(service->event);free(service);service=NULL;
}
static void name_is(const char *v) { char n[DEVICE_IDENTITY_NAME_CAPACITY]; CHECK(device_identity_get_name(n, sizeof(n)) == 0 && !strcmp(n, v)); }
static system_time_settings_t clock_settings(void) { system_time_settings_t s; system_time_get_settings(&s); return s; }
static DWORD WINAPI sync_clock(void *p) { (void)p; CHECK(system_time_set_timezone(-60)); return 0; }
static DWORD WINAPI queued_submit(void *p) {
    bool *accepted=p;concurrent_producer=GetCurrentThreadId();
    wait_event(start_entered);
    static const uint8_t value[4]={99,0,1,1};
    *accepted=preference_store_submit(PREFERENCE_TIME,SYSTEM_TIME_SETTINGS_PATH,0x54494d45u,1,value,sizeof(value));
    return 0;
}
static DWORD WINAPI snapshot_observer(void *p) {
    (void)p;wait_event(start_entered);
    CHECK(clock_settings().timezone_minutes==480);CHECK(system_time_get_utc()!=0);
    SetEvent(snapshot_read);return 0;
}
static DWORD WINAPI concurrent_clock(void *p) {
    for(unsigned i=0;i<300;++i) {
        if(p) CHECK(system_time_request_timezone((int)i));
        else CHECK(system_time_request_hour24((i&1)==0));
    }
    return 0;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); ui_thread = GetCurrentThreadId(); InitializeCriticalSection(&disk_lock); InitializeCriticalSection(&critical);
    entered = CreateEvent(NULL, FALSE, FALSE, NULL); proceed = CreateEvent(NULL, FALSE, FALSE, NULL); host_boot_records();
    start_entered=CreateEvent(NULL,FALSE,FALSE,NULL);submit_attempt=CreateEvent(NULL,FALSE,FALSE,NULL);
    submit_blocked=CreateEvent(NULL,FALSE,FALSE,NULL);acquired=CreateEvent(NULL,FALSE,FALSE,NULL);
    release_acquire=CreateEvent(NULL,FALSE,FALSE,NULL);idle_check=CreateEvent(NULL,FALSE,FALSE,NULL);
    release_idle=CreateEvent(NULL,FALSE,FALSE,NULL);snapshot_read=CreateEvent(NULL,FALSE,FALSE,NULL);
    const char *test = argv[1];
    if(!strcmp(test,"init_failure")) {
        allow_ui_io=true;available=false;
        CHECK(device_identity_init()!=0 && system_time_init()!=0);
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && !preference_store_status(PREFERENCE_TIME).failed);
        available=true;
    }
    allow_ui_io = true; CHECK(device_identity_init() == 0 && system_time_init() == 0 && epd_locale_init()); allow_ui_io = false;
    if(!strcmp(test,"init_failure")) {
        CHECK(system_time_request_hour24(false));settle();CHECK(!notices);
    } else if (!strcmp(test, "merge") || !strcmp(test, "superseded")) {
        gate_write = 1; CHECK(device_identity_request_name("first") == 0); wait_event(entered);
        fail_write = !strcmp(test, "superseded"); LONG before = reads;
        for (unsigned i = 0; i < 1000; ++i) { CHECK(device_identity_request_name(i == 999 ? "latest" : "intermediate") == 0); name_is(i == 999 ? "latest" : "intermediate"); }
        CHECK(system_time_request_timezone(-90) && system_time_request_hour24(false) && epd_locale_request("en_us"));
        CHECK(reads == before && writes == 1 && held == 1 && worker_count == 1 && !service->definition.flush());
        CHECK(clock_settings().timezone_minutes == -90 && !clock_settings().hour24 && !strcmp(epd_locale_get(), "en_us"));
        SetEvent(proceed); settle(); CHECK(writes == 4 && !strcmp((char *)disk[0].data + 16, "latest") && disk[1].data[16] == 166 && disk[1].data[19] == 0);
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && !notices && !held);
    } else if (!strcmp(test, "failure")) {
        fail_write = 1; CHECK(device_identity_request_name("unsaved") == 0); settle(); name_is("unsaved"); CHECK(preference_store_status(PREFERENCE_IDENTITY).failed && notices == 1 && !disk[0].size);
        CHECK(device_identity_request_name("unsaved") == 0); settle(); CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && dismissals == 1);
        fail_write = 1; CHECK(epd_locale_request("en_us")); settle(); CHECK(!strcmp(epd_locale_get(), "en_us") && preference_store_status(PREFERENCE_LANGUAGE).failed);
        uint32_t revision = epd_locale_revision(); CHECK(epd_locale_request("en_us")); settle(); CHECK(epd_locale_revision() == revision && !preference_store_status(PREFERENCE_LANGUAGE).failed);
    } else if (!strcmp(test, "time")) {
        uint32_t token = system_time_ntp_token(); time_t original = rtc; unsigned changes = ntp_changes;
        LONG status_before = clock_notifications;
        gate_write = 1; CHECK(system_time_request_automatic(false)); wait_event(entered); CHECK(!system_time_ntp_current(token) && !system_time_apply_ntp(token, 1767225700));
        CHECK(system_time_request_timezone(330) && system_time_request_hour24(false) && system_time_request_automatic(true));
        CHECK(rtc == original && ntp_changes == changes + 2 && reminders == 1 && !rtc_writes);
        CHECK(clock_notifications == status_before + 2);
        uint32_t fresh = system_time_ntp_token(); CHECK(fresh && fresh != token && !system_time_ntp_current(token));
        SetEvent(proceed); settle(); CHECK(disk[1].data[18] == 1 && disk[1].data[19] == 0);
        fail_write = 1; CHECK(system_time_request_hour24(true)); settle(); CHECK(clock_settings().hour24 && preference_store_status(PREFERENCE_TIME).failed && system_time_ntp_current(fresh));
        CHECK(system_time_request_hour24(true)); settle(); CHECK(!preference_store_status(PREFERENCE_TIME).failed);
        CHECK(system_time_apply_ntp(fresh, 1767225700) && rtc_writes == 1);
        CHECK(clock_notifications == status_before + 5);
    } else if (!strcmp(test, "legacy")) {
        allow_ui_io = true; fail_write = 1; CHECK(device_identity_set_name("failed") != 0); name_is(DEVICE_IDENTITY_DEFAULT_NAME);
        uint32_t token = system_time_ntp_token(); unsigned changes = ntp_changes; fail_write = 1;
        CHECK(!system_time_set_automatic(false) && system_time_ntp_current(token) && ntp_changes == changes);
        fail_write = 1; CHECK(!epd_locale_set("en_us") && !strcmp(epd_locale_get(), "zh_cn"));
        CHECK(device_identity_set_name("durable") == 0 && system_time_set_timezone(-60) && epd_locale_set("en_us"));
        CHECK(!strcmp((char *)disk[0].data + 16, "durable") && clock_settings().timezone_minutes == -60 && !strcmp((char *)disk[2].data + 16, "en_us")); allow_ui_io = false;
    } else if (!strcmp(test, "exclusive")) {
        CHECK(system_time_request_hour24(false)); settle(); gate_write = 1; HANDLE t = CreateThread(NULL, 0, sync_clock, NULL, 0, NULL); wait_event(entered);
        CHECK(clock_settings().timezone_minutes == 480 && !system_time_request_timezone(90) && !service->definition.flush());
        SetEvent(proceed); wait_event(t); CloseHandle(t); CHECK(clock_settings().timezone_minutes == -60); settle();
    } else if (!strcmp(test, "lifecycle")) {
        gate_write = 1; CHECK(device_identity_request_name("final") == 0); wait_event(entered); CHECK(system_time_request_timezone(60));
        CHECK(held == 1 && !service->definition.flush()); service->cancelled = 1; epd_service_wake(service);
        CHECK(device_identity_request_name("rejected") != 0); name_is("final"); SetEvent(proceed); wait_event(service->thread);
        CHECK(held==1 && service->definition.flush() && !strcmp((char *)disk[0].data + 16, "final") && disk[1].data[16] == 60);
        retire();CHECK(!held);
    } else if (!strcmp(test, "reject")) {
        CHECK(device_identity_request_name("") != 0 && device_identity_request_name("bad\n") != 0 && !system_time_request_timezone(841) && !epd_locale_request("bad"));
        fail_start = 1; CHECK(device_identity_request_name("rejected") != 0); name_is(DEVICE_IDENTITY_DEFAULT_NAME); fail_start = 0;
        for (blocked = 1; blocked <= 4; ++blocked) CHECK(!system_time_request_hour24(false)); blocked = 0;
        sleeping = 1; CHECK(device_identity_request_name("sleep") != 0); name_is(DEVICE_IDENTITY_DEFAULT_NAME); CHECK(!writes && !held); sleeping = 0;
        CHECK(device_identity_request_name("awake") == 0); settle();
    } else if (!strcmp(test,"load_status")) {
        allow_ui_io=true;available=false;
        CHECK(device_identity_init()!=0 && system_time_reload()==SYSTEM_TIME_RELOAD_IO_ERROR);
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && !preference_store_status(PREFERENCE_TIME).failed);
        available=true;allow_ui_io=false;
        CHECK(device_identity_request_name("saved") == 0 && system_time_request_timezone(120));settle();
        fail_write=1;CHECK(device_identity_request_name("unsaved")==0);settle();
        fail_write=1;CHECK(system_time_request_timezone(180));settle();CHECK(notices==2);
        allow_ui_io=true;
        CHECK(device_identity_init()==0 && system_time_reload()==SYSTEM_TIME_RELOAD_LOADED);
        CHECK(system_time_init()==0);
        allow_ui_io=false;poll();
        CHECK(preference_store_status(PREFERENCE_IDENTITY).failed && preference_store_status(PREFERENCE_TIME).failed && !dismissals);
        gate_write=1;CHECK(device_identity_request_name("retry")==0);wait_event(entered);
        CHECK(preference_store_status(PREFERENCE_IDENTITY).failed && preference_store_status(PREFERENCE_IDENTITY).pending);
        poll();CHECK(notices==2 && !dismissals);SetEvent(proceed);settle();
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && dismissals==1);
    } else if (!strcmp(test,"notifications")) {
        fail_write=1;CHECK(epd_locale_request("en_us"));settle();CHECK(notices==1);
        CHECK(epd_notification_dismiss("settings","language-save"));
        poll();CHECK(notices==1);
        gate_write=fail_write=1;CHECK(epd_locale_request("en_us"));wait_event(entered);poll();CHECK(notices==1);
        CHECK(preference_store_status(PREFERENCE_LANGUAGE).failed);
        SetEvent(proceed);settle();CHECK(notices==2);
        CHECK(epd_locale_request("en_us"));settle();CHECK(!preference_store_status(PREFERENCE_LANGUAGE).failed && dismissals==2);
    } else if (!strcmp(test,"timeout")) {
        gate_write=1;CHECK(device_identity_request_name("busy")==0);wait_event(entered);
        uint32_t started=rt_tick_get();CHECK(!preference_store_begin_sync(PREFERENCE_IDENTITY));
        CHECK(rt_tick_get()-started>=1900 && rt_tick_get()-started<3500);
        CHECK(preference_store_status(PREFERENCE_IDENTITY).pending && !service->definition.flush());
        preference_store_end_sync((preference_id_t)-1,PREFERENCE_SYNC_SAVED);
        preference_store_end_sync(PREFERENCE_COUNT,PREFERENCE_SYNC_FAILED);
        preference_store_end_sync(PREFERENCE_IDENTITY,PREFERENCE_SYNC_FAILED);
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed);
        SetEvent(proceed);settle();CHECK(preference_store_begin_sync(PREFERENCE_IDENTITY));
        CHECK(!preference_store_begin_sync(PREFERENCE_IDENTITY));
        preference_store_end_sync(PREFERENCE_IDENTITY,PREFERENCE_SYNC_UNCHANGED);CHECK(service->definition.flush());
    } else if (!strcmp(test,"startup")) {
        bool accepted=false;gate_start=1;HANDLE t=CreateThread(NULL,0,queued_submit,&accepted,0,NULL);
        CHECK(device_identity_request_name("ui-start")==0);wait_event(t);CloseHandle(t);
        CHECK(accepted && worker_count==1);settle();
        CHECK(system_time_request_hour24(false));settle();CHECK(worker_count==1);
    } else if (!strcmp(test,"snapshot_start")) {
        gate_start=2;HANDLE t=CreateThread(NULL,0,snapshot_observer,NULL,0,NULL);
        CHECK(system_time_request_timezone(90));wait_event(t);CloseHandle(t);settle();
    } else if (!strcmp(test,"concurrent_fields")) {
        gate_write=1;CHECK(system_time_request_automatic(false));wait_event(entered);
        HANDLE t[2]={CreateThread(NULL,0,concurrent_clock,(void *)1,0,NULL),CreateThread(NULL,0,concurrent_clock,NULL,0,NULL)};
        wait_event(t[0]);wait_event(t[1]);CloseHandle(t[0]);CloseHandle(t[1]);
        system_time_settings_t current=clock_settings();CHECK(current.timezone_minutes==299 && !current.hour24 && !current.automatic);
        SetEvent(proceed);settle();CHECK(disk[1].data[16]==43 && disk[1].data[17]==1 && disk[1].data[18]==0 && disk[1].data[19]==0);
        CHECK(worker_count==1);
    } else if (!strcmp(test,"exit_race")) {
        gate_write=1;CHECK(device_identity_request_name("before-stop")==0);wait_event(entered);
        gate_idle_check=gate_acquire=1;SetEvent(proceed);wait_event(idle_check);
        bool accepted=false;HANDLE t=CreateThread(NULL,0,queued_submit,&accepted,0,NULL);SetEvent(start_entered);
        HANDLE admission[]={acquired,submit_blocked};CHECK(WaitForMultipleObjects(2,admission,FALSE,5000)<WAIT_OBJECT_0+2);
        InterlockedExchange(&service->cancelled,1);SetEvent(release_idle);wait_event(service->thread);
        SetEvent(release_acquire);wait_event(t);CloseHandle(t);
        CHECK(service->definition.flush());
        if(accepted)CHECK(disk[1].size && disk[1].data[16]==99);
        retire();CHECK(!held);
        CHECK(device_identity_request_name("restarted")==0);settle();CHECK(worker_count==2);
    } else if (!strcmp(test,"failed_exit")) {
        fail_write=1;CHECK(device_identity_request_name("unsaved")==0);settle();
        CHECK(preference_store_status(PREFERENCE_IDENTITY).failed && service->definition.flush());
        retire();CHECK(preference_store_status(PREFERENCE_IDENTITY).failed && !held);
        CHECK(device_identity_request_name("recovered")==0);settle();
        CHECK(!preference_store_status(PREFERENCE_IDENTITY).failed && dismissals==1);
    } else CHECK(false);
    if(service)retire();
    printf("PASS preference %s\n", test); return 0;
}
