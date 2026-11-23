"""Run WLAN scan and SDIO shutdown regressions against production functions."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


COMMON = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EIO 5
#define RT_EBUSY 16
#define RT_EINVAL 22
#define RT_ENOMEM 12
typedef int rt_err_t;
typedef int rt_bool_t;
typedef unsigned rt_uint32_t;
typedef uint8_t rt_uint8_t;
typedef int8_t rt_int8_t;
typedef int16_t rt_int16_t;
typedef int rt_int32_t;
typedef int rt_base_t;
static int rt_hw_interrupt_disable(void) {return 0;}
static void rt_hw_interrupt_enable(int l) {(void)l;}
'''

SCAN = r'''
#define WIFI_MAX_SSID_LEN 32
#define MAX_AP_COUNT 4
#define WLAN_ROLE_STA 1
#define WLAN_CANCEL_SCAN 1
#define EPD_APP_PSRAM 1
#define AIC_IF_ROLE_STA 0
#define RT_802_11_BAND_5GHZ 5
#define RT_802_11_BAND_2_4GHZ 2
#define RT_WLAN_DEV_EVT_SCAN_REPORT 1
#define RT_WLAN_DEV_EVT_SCAN_DONE 2
struct rt_wlan_device {int index;};
struct ssid {unsigned len; char val[33];};
struct rt_scan_info {struct ssid ssid; int channel_min,channel_max; uint8_t bssid[6];};
struct rt_wlan_info {int security,band,channel,rssi,hidden; struct ssid ssid; uint8_t bssid[6];};
struct rt_wlan_buff {void *data; unsigned len;};
typedef struct {unsigned ssid_len,channel,auth_mode; int rssi; uint8_t ssid[33],bssid[6];} wifi_ap_info_t;
typedef struct {unsigned ap_count; wifi_ap_info_t ap_info[MAX_AP_COUNT];} wifi_ap_list_t;
struct aic_netif_info {int vif_idx; void *net_if;};
struct fhost_cntrl_link {int unused;};
static struct fhost_cntrl_link control;
static unsigned enabled_roles=WLAN_ROLE_STA, cancelled_operations;
static int have_vif, scan_calls, super_calls, temp_calls, temp_super_calls, scan_result, done, reports, alloc_fail, link_fail;
static char requested_ssid[33];
static int adapter_index(struct rt_wlan_device *d) {return d->index;}
static void begin_operation(unsigned op) {(void)op;}
static void end_operation(unsigned op) {(void)op;}
static int commit_operation(unsigned op) {return !(cancelled_operations&op);}
static void *epd_app_alloc(size_t n, int where) {(void)where; return alloc_fail?NULL:malloc(n);}
static void epd_app_free(void *p) {free(p);}
static int aic_netif_get_info(int role,struct aic_netif_info *i) {CHECK(role==0);i->vif_idx=0;i->net_if=&control;return have_vif?0:-1;}
static struct fhost_cntrl_link *fhost_cntrl_cfgrwnx_link_open(void) {return link_fail?NULL:&control;}
static void fhost_cntrl_cfgrwnx_link_close(struct fhost_cntrl_link *p) {CHECK(p==&control);}
static int fhost_scan(struct fhost_cntrl_link *p,int vif,int *freqs) {CHECK(p==&control&&vif==0&&!freqs);++scan_calls;return scan_result;}
static int aic_wifi_scan(void) {++temp_calls;return scan_result;}
static int aic_wifi_super_scan(uint8_t *s) {++temp_super_calls;strcpy(requested_ssid,(char*)s);return scan_result;}
static void aic_wifi_getscan(wifi_ap_list_t *p) {p->ap_count=1;p->ap_info[0].channel=48;}
static int scan_matches(const struct rt_scan_info *f,const wifi_ap_info_t *p) {(void)f;(void)p;return 1;}
static int scan_security(unsigned a) {return (int)a;}
static void rt_wlan_dev_indicate_event_handle(struct rt_wlan_device *d,int evt,struct rt_wlan_buff *b) {(void)d;(void)b;CHECK(evt==1);++reports;}
static void indicate(struct rt_wlan_device *d,int evt) {(void)d;CHECK(evt==2);++done;}
'''

SCAN_MAIN = r'''
static void reset(void) {scan_calls=super_calls=temp_calls=temp_super_calls=done=reports=alloc_fail=link_fail=scan_result=cancelled_operations=0;}
int main(void) {
    struct rt_wlan_device dev={0}; struct rt_scan_info filter={0};
    have_vif=1; reset(); CHECK(adapter_scan(&dev,NULL)==0&&scan_calls==1&&temp_calls==0&&done==1&&reports==1);
    /* A retained, disconnected STA follows the same path as an associated STA. */
    reset(); filter.ssid.len=6;memcpy(filter.ssid.val,"hidden",6);
    CHECK(adapter_scan(&dev,&filter)==0&&super_calls==1&&temp_super_calls==0&&!strcmp(requested_ssid,"hidden"));
    reset();scan_result=-1;CHECK(adapter_scan(&dev,NULL)<0&&reports==0&&done==1);
    reset();link_fail=1;CHECK(adapter_scan(&dev,NULL)<0&&done==1&&scan_calls==0);
    reset();alloc_fail=1;CHECK(adapter_scan(&dev,NULL)==-RT_ENOMEM&&done==1);
    reset();cancelled_operations=1;CHECK(adapter_scan(&dev,NULL)==-RT_EBUSY&&reports==0&&done==1);
    reset();have_vif=0;CHECK(adapter_scan(&dev,NULL)==0&&temp_calls==1&&scan_calls==0);
    reset();CHECK(adapter_scan(&dev,&filter)==0&&temp_super_calls==1&&super_calls==0);
    puts("WLAN existing/disconnected/directed/temporary scan regressions passed");return 0;
}
'''

RADIO = r'''
typedef int IRQn_Type;
typedef struct {unsigned FIFO;} SD_TypeDef;
typedef int HAL_StatusTypeDef;
typedef int HAL_DMA_LevelCompleteTypeDef;
typedef struct {int State,ErrorCode; unsigned LeftCounts,ChannelIndex; void *Instance; uintptr_t SrcAddress,DstAddress;} DMA_HandleTypeDef;
struct rt_mmcsd_host {int bus_lock;};
struct rt_mmcsd_cmd {int retries,cmd_code,err;};
struct rt_mmcsd_data {int err;};
struct rt_mmcsd_req {struct rt_mmcsd_cmd *cmd,*stop;struct rt_mmcsd_data *data;};
#define HAL_OK 0
#define HAL_ERROR 1
#define HAL_DMA_FULL_TRANSFER 1
#define HAL_DMA_STATE_READY 0
#define HAL_DMA_ERROR_NONE 0
#define HAL_DMA_ERROR_TE 1
#define HAL_DMA_STATE_BUSY 1
#define SD_IO_RW_EXTENDED 53
#define RADIO_TRACK 1
#define RADIO_BLOCK 2
#define RADIO_SEEN_DATA 2
#define RADIO_SEEN_DMA 4
#define RADIO_IGNORE 0
#define RT_WAITING_FOREVER -1
static struct {struct rt_mmcsd_host *host;unsigned in_flight;int cycle_error;bool dma_uncertain,sealed,nvic_was_enabled,closing,closed,detached;} radio_bus;
static struct {void *owner;SD_TypeDef *controller;DMA_HandleTypeDef *dma;uintptr_t channel;unsigned index;bool dma_pending;} radio_transfer;
static DMA_HandleTypeDef dma;
static SD_TypeDef controller;
static int channel,aborts,abort_result,request_error,cmd_error,start_dma=1,poll_result;
static unsigned radio_bus_begin(struct rt_mmcsd_host *h,unsigned s) {(void)h;(void)s;++radio_bus.in_flight;return RADIO_TRACK;}
static SD_TypeDef *radio_controller(IRQn_Type *irq) {*irq=1;return &controller;}
static void *rt_thread_self(void) {return &controller;}
static int rt_mutex_take(int *m,int t) {(void)t;++*m;return 0;}
static void rt_mutex_release(int *m) {--*m;}
static int radio_command_failed(struct rt_mmcsd_cmd *c) {return c&&c->err;}
static void HAL_NVIC_EnableIRQ(int irq) {(void)irq;}
#define rt_kprintf(...) ((void)0)
static int HAL_DMA_Abort(DMA_HandleTypeDef *d) {++aborts;if(!abort_result)d->State=0;return abort_result;}
static int __real_HAL_DMA_PollForTransfer(DMA_HandleTypeDef *d,int c,unsigned t) {(void)d;(void)c;(void)t;return poll_result;}
static void __real_mmcsd_send_request(struct rt_mmcsd_host *h,struct rt_mmcsd_req *r) {
    CHECK(h->bus_lock==1);r->data->err=request_error;r->cmd->err=cmd_error;
    if(start_dma){dma.State=HAL_DMA_STATE_BUSY;dma.Instance=&channel;dma.ChannelIndex=4;
      radio_transfer.dma=&dma;radio_transfer.channel=(uintptr_t)&channel;radio_transfer.index=4;radio_transfer.dma_pending=true;}
}
'''
RADIO_MAIN = r'''
int main(void) {
    struct rt_mmcsd_host h={0};struct rt_mmcsd_cmd c={0,53,0};struct rt_mmcsd_data d={0};struct rt_mmcsd_req r={&c,NULL,&d};
    request_error=1;__wrap_mmcsd_send_request(&h,&r);
    CHECK(aborts==1&&!radio_bus.dma_uncertain&&radio_bus.cycle_error==-RT_EIO&&h.bus_lock==0&&radio_bus.in_flight==0);
    radio_bus_operation(false);CHECK(radio_bus.cycle_error==0&&!radio_bus.dma_uncertain&&radio_bus.closing);
    abort_result=1;__wrap_mmcsd_send_request(&h,&r);CHECK(radio_bus.dma_uncertain);
    radio_bus_operation(false);CHECK(radio_bus.dma_uncertain);radio_bus_operation(true);CHECK(radio_bus.dma_uncertain);
    memset(&radio_bus,0,sizeof(radio_bus));start_dma=0;__wrap_mmcsd_send_request(&h,&r);CHECK(!radio_bus.dma_uncertain);
    dma.SrcAddress=(uintptr_t)&controller.FIFO;dma.State=HAL_DMA_STATE_READY;
    radio_transfer.owner=rt_thread_self();radio_transfer.dma=&dma;radio_transfer.dma_pending=true;
    CHECK(__wrap_HAL_DMA_PollForTransfer(&dma,HAL_DMA_FULL_TRANSFER,100)==0&&!radio_transfer.dma_pending&&!radio_bus.dma_uncertain);
    /* A completed hardware transfer error stops DMA; a software timeout does not. */
    poll_result=HAL_ERROR;dma.ErrorCode=HAL_DMA_ERROR_TE;radio_transfer.dma_pending=true;
    CHECK(__wrap_HAL_DMA_PollForTransfer(&dma,HAL_DMA_FULL_TRANSFER,100)==HAL_ERROR&&!radio_transfer.dma_pending&&!radio_bus.dma_uncertain);
    dma.ErrorCode=2;radio_transfer.dma_pending=true;
    CHECK(__wrap_HAL_DMA_PollForTransfer(&dma,HAL_DMA_FULL_TRANSFER,100)==HAL_ERROR&&radio_transfer.dma_pending&&!radio_bus.dma_uncertain);
    radio_transfer.owner=NULL;CHECK(__wrap_HAL_DMA_PollForTransfer(&dma,HAL_DMA_FULL_TRANSFER,100)==HAL_ERROR&&radio_bus.dma_uncertain);
    puts("WLAN failed request / successful abort / unresolved DMA shutdown regressions passed");return 0;
}
'''

CACHE = r'''
#define RT_WLAN_SCAN_CACHE_NUM 50
#define RT_WLAN_BSSID_MAX_LENGTH 6
#define RT_WLAN_LOG_D(...) ((void)0)
#define RT_WLAN_LOG_E(...) ((void)0)
#define rt_memcmp memcmp
#define rt_malloc malloc
#define rt_free free
static int scan_result_mutex;
static int rt_tick_from_millisecond(int t) {return t;}
static int rt_mutex_take(int *m,int t) {(void)t;++*m;return 0;}
static void rt_mutex_release(int *m) {--*m;}
static int _sta_is_null(void) {return 0;}
struct rt_wlan_info {struct {int len;char val[33];} ssid;uint8_t bssid[6];};
static struct {int num;struct rt_wlan_info *info;} scan_result;
'''
CACHE_MAIN = r'''
int main(void) {
    struct rt_wlan_info ap={0};ap.ssid.len=1;
    for(int i=0;i<75;i++) {ap.bssid[0]=(uint8_t)i;CHECK(rt_wlan_scan_result_cache(&ap,0)==0);CHECK(scan_result_mutex==0);}
    CHECK(scan_result.num==50);free(scan_result.info);scan_result.info=NULL;scan_result.num=0;
    CHECK(rt_wlan_scan_result_cache(&ap,0)==0&&scan_result_mutex==0&&scan_result.num==1);
    CHECK(rt_wlan_scan_result_cache(&ap,0)==0&&scan_result_mutex==0&&scan_result.num==1);free(scan_result.info);
    puts("WLAN full scan-cache / rescan / duplicate regressions passed");return 0;
}
'''

POWERSAVE = r'''
#define RT_ENOSYS 38
#define RT_WLAN_CMD_GET_POWERSAVE 1
#define WLAN_STA_POWERSAVE_MAX_LEVEL 1
#define LOG_I(...) ((void)0)
#define LOG_W(...) ((void)0)
static struct { bool powersave; } capabilities;
static struct { bool active; } hotspot;
static bool enabled, sleep_hold, held, reset, ready, connected, ap_active;
static int sleep_powersave, sleep_powersave_error, level, set_error, get_error;
static unsigned calls;
static void take(void) {}
static void give(void) {}
static bool reset_held(void) {return held;}
static bool system_reset_blocks_apps(void) {return reset;}
static bool driver_ready(void) {return ready;}
static bool rt_wlan_is_connected(void) {return connected;}
static bool rt_wlan_ap_is_active(void) {return ap_active;}
static rt_err_t radio_control(int command, void *argument)
{
    CHECK(command == RT_WLAN_CMD_GET_POWERSAVE);
    if (get_error) return get_error;
    if (level < 0) return -RT_ENOSYS;
    *(int *)argument = level;
    return RT_EOK;
}
static rt_err_t rt_wlan_set_powersave(int value)
{
    CHECK(value == 1);
    ++calls;
    if (set_error) return set_error;
    level = value;
    return RT_EOK;
}
'''

POWERSAVE_MAIN = r'''
int main(void)
{
    unsigned attempts = 0;
    enabled = ready = capabilities.powersave = true;
    level = sleep_powersave = -1;
    enable_connected_powersave(&attempts);
    CHECK(!attempts && !calls); /* Wait for association. */
    connected = ap_active = true;
    enable_connected_powersave(&attempts);
    CHECK(!attempts && !calls);
    ap_active = false;
    hotspot.active = true;
    enable_connected_powersave(&attempts);
    CHECK(!attempts && !calls);
    hotspot.active = false;
    sleep_hold = true;
    enable_connected_powersave(&attempts);
    CHECK(!attempts && !calls);
    sleep_hold = false;
    sleep_powersave = 0;
    enable_connected_powersave(&attempts);
    CHECK(!attempts && !calls); /* Let the stored level restore first. */
    sleep_powersave = -1;
    level = 0;
    enable_connected_powersave(&attempts);
    CHECK(attempts == POWERSAVE_ATTEMPTS && !calls && level == 0);
    attempts = 0;
    level = -1;
    get_error = -RT_EBUSY;
    enable_connected_powersave(&attempts);
    CHECK(attempts == 1 && !calls);
    get_error = 0;
    set_error = -RT_EBUSY;
    enable_connected_powersave(&attempts);
    CHECK(attempts == 2 && calls == 1);
    set_error = 0;
    enable_connected_powersave(&attempts);
    enable_connected_powersave(&attempts);
    CHECK(attempts == POWERSAVE_ATTEMPTS && calls == 2 && level == 1);
    puts("WLAN connected STA power save / explicit level / AP / suspend / bounded retry passed");
    return 0;
}
'''

OPEN_RETRY = r'''
#define RT_WAITING_FOREVER -1
typedef uint32_t rt_tick_t;
static rt_tick_t now;
static rt_tick_t rt_tick_get(void) { return now; }
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
'''

OPEN_RETRY_MAIN = r'''
int main(void)
{
    open_retry_t retry = {.epoch = 7};
    open_retry_result(&retry, -RT_EIO, RT_FALSE);
    CHECK(open_retry_wait(&retry, now) == RT_WAITING_FOREVER);

    now = UINT32_MAX - 300;
    open_retry_result(&retry, -RT_EIO, RT_TRUE);
    now += 749;
    CHECK(open_retry_wait(&retry, now) == 1);
    CHECK(!open_retry_due(&retry, 7, true));
    ++now;
    CHECK(open_retry_due(&retry, 7, true));
    CHECK(!open_retry_due(&retry, 7, true));

    open_retry_result(&retry, -RT_EIO, RT_TRUE);
    now += 1500;
    CHECK(open_retry_due(&retry, 7, true));
    open_retry_result(&retry, -RT_EIO, RT_TRUE);
    CHECK(!retry.pending && retry.attempts == 2);

    open_retry_result(&retry, RT_EOK, RT_FALSE);
    CHECK(retry.attempts == 0);
    open_retry_result(&retry, -RT_EIO, RT_TRUE);
    now += 750;
    CHECK(!open_retry_due(&retry, 8, true) && !retry.pending && !retry.attempts);
    open_retry_result(&retry, -RT_EIO, RT_TRUE);
    now += 750;
    CHECK(!open_retry_due(&retry, 8, false) && !retry.pending && !retry.attempts);
    puts("WLAN OPEN rollback gate, retry delays, limit and cancellation passed");
    return 0;
}
'''


def main():
    adapter = (ROOT / 'components/wlan/src/wlan_rt_adapter.c').read_text()
    radio = (ROOT / 'components/wlan/src/wlan_radio_power.c').read_text()
    mgnt = (ROOT / 'SiFli-SDK/rtos/rtthread/components/drivers/wlan/wlan_mgnt.c').read_text()
    service = (ROOT / 'src/services/net/wlan_service.c').read_text(encoding='utf-8')
    retry_limit = next(line for line in service.splitlines() if line.startswith('#define POWERSAVE_ATTEMPTS '))
    open_retry_constants = '\n'.join(line for line in service.splitlines() if line.startswith('#define OPEN_RETRY_'))
    open_retry_end = service.index('} open_retry_t;') + len('} open_retry_t;')
    open_retry_type = service[service.rindex('typedef struct', 0, open_retry_end):open_retry_end]
    structures = adapter[adapter.index('struct adapter_scan_ssid\n'):adapter.index('extern int fhost_super_scan')]
    super_mock = r'''
static int fhost_super_scan(struct fhost_cntrl_link *link,int vif,const struct adapter_scan_parameters *p) {
 CHECK(link==&control&&vif==0&&p->ssid_count==1&&p->band==-1&&!p->frequencies&&!p->bssid&&!p->extra_ies&&!p->extra_ies_length);
 CHECK(p->ssids->length==6);memcpy(requested_ssid,p->ssids->value,6);requested_ssid[6]=0;++super_calls;return scan_result;
}
'''
    programs = {
        'scan': COMMON + SCAN + structures + super_mock + '\n'.join(function(adapter, s) for s in (
            'static rt_err_t driver_result(', 'static rt_err_t adapter_scan_existing(', 'static rt_err_t adapter_scan(')) + SCAN_MAIN,
        'radio': COMMON + RADIO + '\n'.join(function(radio, s) for s in (
            'static void radio_bus_end(', 'void __wrap_mmcsd_send_request(', 'HAL_StatusTypeDef __wrap_HAL_DMA_PollForTransfer(', 'static void radio_bus_operation(')) + RADIO_MAIN,
        'cache': COMMON + CACHE + function(mgnt, 'static rt_err_t rt_wlan_scan_result_cache(') + CACHE_MAIN,
        'powersave': COMMON + POWERSAVE + retry_limit + '\n' +
            function(service, 'static void enable_connected_powersave(') + POWERSAVE_MAIN,
        'open_retry': COMMON + OPEN_RETRY + open_retry_constants + '\n' + open_retry_type + '\n' +
            '\n'.join(function(service, signature) for signature in (
                'static rt_int32_t open_retry_wait(', 'static void open_retry_result(', 'static bool open_retry_due(')) +
            OPEN_RETRY_MAIN,
    }
    cc, env = compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='wlan_recovery_') as directory:
        build = Path(directory)
        for name, text in programs.items():
            source = build / f'{name}.c'
            source.write_text(text, encoding='utf-8')
            exe = build / f'{name}.exe'
            subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/O2', '/D_CRT_SECURE_NO_WARNINGS', str(source), f'/Fe{exe}'], cwd=build, env=env, check=True)
            subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__':
    main()
