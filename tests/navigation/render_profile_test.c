#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4244)
#endif
#include "lvgl.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "epd_render_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define RT_EOK 0
#define RT_TICK_PER_SECOND 1000u
#define RTM_EXPORT(fn)
#define INIT_COMPONENT_EXPORT(fn)
#define MSH_CMD_EXPORT(fn, text)
#define L2_RET_BSS_SECT_BEGIN(name)
#define L2_RET_BSS_SECT_END
#define L2_RET_BSS_SECT(name)
typedef unsigned rt_base_t;
typedef uint32_t rt_tick_t;
static unsigned irq_depth;
static uint32_t ticks;
static rt_base_t rt_hw_interrupt_disable(void) { return irq_depth++; }
static void rt_hw_interrupt_enable(rt_base_t level) { CHECK(irq_depth==level+1);irq_depth=level; }
static uint32_t rt_tick_get(void) { return ticks; }
static uint32_t rt_tick_from_millisecond(uint32_t ms) { return ms; }
static char report[8192];
static void rt_kprintf(const char *format, ...)
{
    size_t used=strlen(report); va_list args;va_start(args,format);
    int n=vsnprintf(report+used,sizeof(report)-used,format,args);va_end(args);
    CHECK(n>=0&&(size_t)n<sizeof(report)-used);
}
#include "render_profile_actual.inc"
#include "ui_perf_actual.inc"

static void event(lv_display_t *display, lv_event_code_t code, uint32_t at)
{ ticks=at;CHECK(lv_display_send_event(display,code,NULL)==LV_RESULT_OK); }
static void command(const char *name)
{ char *args[]={"ui_perf",(char *)name};report[0]=0;ui_perf(2,args); }
int main(void)
{
    _Static_assert(sizeof(epd_render_profile_t)==32,"module profile ABI");
    memset(render_profile,0xaa,sizeof(render_profile));
    memset(&render_diagnostic_base,0xaa,sizeof(render_diagnostic_base));
    CHECK(render_profile_init()==RT_EOK);
    epd_render_diagnostic_t result;
    epd_render_diagnostic_snapshot(&result);
    CHECK(!result.stage[EPD_RENDER_DSC].ticks&&!epd_render_profile_begin(EPD_RENDER_DSC));
    ticks=100;epd_render_diagnostic_window(true);
    uint32_t outer=epd_render_profile_begin(EPD_RENDER_DSC);
    ticks=110;uint32_t inner=epd_render_profile_begin(EPD_RENDER_DSC);
    ticks=115;epd_render_profile_end(EPD_RENDER_DSC,inner);
    ticks=120;epd_render_diagnostic_snapshot(&result);
    CHECK(result.stage[EPD_RENDER_DSC].calls==2&&result.stage[EPD_RENDER_DSC].ticks==20);
    epd_render_diagnostic_window(true);
    CHECK(epd_render_profile_generation==outer);
    ticks=125;epd_render_profile_end(EPD_RENDER_DSC,outer);
    epd_render_profile_window(true);
    uint32_t bitmap=epd_render_profile_begin(EPD_RENDER_BITMAP);
    ticks=132;epd_render_profile_end(EPD_RENDER_BITMAP,bitmap);
    epd_render_profile_window(false);
    CHECK(epd_render_profile_generation==outer);
    epd_render_diagnostic_window(false);
    epd_render_diagnostic_snapshot(&result);
    CHECK(result.stage[EPD_RENDER_DSC].calls==0&&result.stage[EPD_RENDER_DSC].ticks==5);
    CHECK(result.stage[EPD_RENDER_BITMAP].ticks==7&&!epd_render_profile_generation);
    ticks=500;epd_render_diagnostic_snapshot(&result);CHECK(result.stage[EPD_RENDER_BITMAP].ticks==7);

    /* Old module snapshots still write exactly the original four counters. */
    struct { epd_render_profile_t data; uint32_t guard; } legacy={0};
    legacy.guard=0x12345678;epd_render_profile_snapshot(&legacy.data);CHECK(legacy.guard==0x12345678);
    epd_render_profile_window(true);
    uint32_t token=epd_render_profile_begin(EPD_RENDER_DSC);
    ticks=510;epd_render_diagnostic_window(true);
    ticks=520;epd_render_diagnostic_window(false);
    CHECK(epd_render_profile_generation==token);
    ticks=530;epd_render_profile_end(EPD_RENDER_DSC,token);
    epd_render_profile_snapshot(&legacy.data);CHECK(legacy.data.stage[EPD_RENDER_DSC].ticks==30);
    epd_render_diagnostic_snapshot(&result);CHECK(result.stage[EPD_RENDER_DSC].ticks==10);
    epd_render_profile_window(false);
    epd_render_diagnostic_window(true);uint32_t next=epd_render_profile_begin(EPD_RENDER_DSC);
    CHECK(next!=token);epd_render_profile_end(EPD_RENDER_DSC,token);
    CHECK(render_profile[EPD_RENDER_DSC].depth==1);
    ticks=535;epd_render_profile_end(EPD_RENDER_DSC,next);epd_render_diagnostic_window(false);
    epd_render_diagnostic_snapshot(&result);CHECK(result.stage[EPD_RENDER_DSC].ticks==5);

    lv_init();lv_tick_set_cb(rt_tick_get);
    lv_display_t *display=lv_display_create(684,1216);CHECK(display);
    lv_display_add_event_cb(display,ui_render_event,LV_EVENT_ALL,NULL);
    command("reset");CHECK(strstr(report,"window started"));
    event(display,LV_EVENT_REFR_START,1000);
    event(display,LV_EVENT_RENDER_START,1007);
    event(display,LV_EVENT_FLUSH_WAIT_START,1008);
    event(display,LV_EVENT_FLUSH_WAIT_FINISH,1011);
    event(display,LV_EVENT_FLUSH_START,1012);
    ticks=1029;token=epd_render_profile_begin(EPD_RENDER_COPY);
    ticks=1031;epd_render_profile_end(EPD_RENDER_COPY,token);
    event(display,LV_EVENT_FLUSH_FINISH,1035);
    event(display,LV_EVENT_RENDER_READY,1041);
    event(display,LV_EVENT_REFR_READY,1044);
    CHECK(ui_duration_record(2,1000)==1044);
    command("report");
    CHECK(strstr(report,"ui_perf lvgl count=1 total_ms=44 max_ms=44"));
    CHECK(strstr(report,"ui_render refresh count=1 total_ms=44"));
    CHECK(strstr(report,"ui_render pre_render count=1 total_ms=7"));
    CHECK(strstr(report,"ui_render render count=1 total_ms=34"));
    CHECK(strstr(report,"ui_render flush count=1 total_ms=23"));
    CHECK(strstr(report,"ui_render flush_wait count=1 total_ms=3"));
    CHECK(strstr(report,"ui_render copy count=1 total_ms=2"));
    CHECK(!epd_render_profile_generation);
    ticks=2000;command("report");CHECK(strstr(report,"ui_render refresh count=1 total_ms=44"));
    command("reset");event(display,LV_EVENT_REFR_START,3000);event(display,LV_EVENT_REFR_READY,3004);
    command("report");CHECK(strstr(report,"ui_render pre_render count=1 total_ms=4"));
    CHECK(strstr(report,"ui_render render count=0 total_ms=0"));
    CHECK(!irq_depth);
    puts("PASS render profile: nested stages, clipped reset, legacy ABI/window isolation, stale tokens, LVGL events and ui_perf report");
    return 0;
}
