#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
static unsigned host_ctz(uint32_t value) { unsigned long bit; _BitScanForward(&bit,value); return bit; }
#define __builtin_ctz host_ctz
#include "rtthread.h"
#include "rthw.h"
#include "mem_section.h"
#include "epd_gray_pixels.h"
#include "epd_waveform.h"
#include "reference_waveform.h"
#include "epd_waveform_bin_reader.h"
#include "epd_display_chunks.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(2);}}while(0)
#include "wave_reader.h"
typedef int LCDC_InitTypeDef;
typedef int HAL_LCDC_PixelFormat;
typedef struct LCDC {int Init;volatile uint32_t ErrorCode;struct {int data_format;} Layer[1];void(*XferCpltCallback)(struct LCDC*);void(*XferErrorCallback)(struct LCDC*);} LCDC_HandleTypeDef;
typedef struct {int Pin,Mode,Pull;} GPIO_InitTypeDef;
typedef void *rt_thread_t;
typedef void *rt_device_t;
struct rt_semaphore {unsigned count;};
struct rt_device_graphic_ops {void(*set_window)(int,int,int,int);void(*draw_rect_async)(const char*,int,int,int,int);};
#define L2_NON_RET_BSS_SECT_BEGIN(x)
#define L2_NON_RET_BSS_SECT_END
#define L2_NON_RET_BSS_SECT(x,y) y
#define ALIGN(n) __declspec(align(n))
#define DBG_ERROR 1
#define LOG_E(...) ((void)0)
#define RT_NULL NULL
#define RT_EOK 0
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_FIFO 0
#define RT_THREAD_PRIORITY_HIGH 5
#define HAL_LCDC_LOOKUP_TABLE_SIZE 1024
#define HAL_LCDC_LAYER_DEFAULT 0
#define HAL_OK 0
#define HAL_LCDC_ERROR_NONE 0x00000000U
#define HAL_LCDC_ERROR_OVERFLOW 0x00000002U
#define LCDC_PIXEL_FORMAT_RGB565 1
#define LCDC_PIXEL_FORMAT_A4 2
#define LCDC_PIXEL_FORMAT_L8 3
#define PAD_PA26 26
#define GPIO_A26 26
#define PIN_NOPULL 0
#define GPIO_MODE_INPUT 0
#define GPIO_NOPULL 0
#define LCDC1_TCON_SDOE 1
#define hwp_gpio1 0
#define EPD_APP_PSRAM 1
#define EPD_VCOM_MV 1
#define EPD_RENDER_SETTLE 1
#define EPD_RENDER_COPY 2
#define RT_USING_FINSH 1
#define MSH_CMD_EXPORT(...)
#define RT_TICK_PER_SECOND 1000
typedef uint32_t rt_tick_t;
static rt_tick_t host_tick;
static rt_tick_t rt_tick_get(void){return host_tick;}
static unsigned clean_bytes;
static const int epd_e0470a03_gyd7n6_config=1;
static unsigned sends,completions,fail_send_at,fail_complete_at,callbacks,power_ons,power_offs;
static unsigned overflow_complete_at;
static bool fail_power;
static rt_tick_t rail_start;
static int modes_seen[1024];
static void BSP_LCD_PowerUp(void){++power_ons;}
static void BSP_LCD_GMODE_Set(int x){(void)x;}
static void BSP_GPIO_Set(int x,int y,int z){(void)x;(void)y;(void)z;}
void BSP_EPD_BusIdle(void){}
static void HAL_PIN_Set(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
static void HAL_Delay_us(int n){(void)n;}
static void HAL_GPIO_Init(int x,GPIO_InitTypeDef*y){(void)x;(void)y;}
static int HAL_LCDC_Resume(LCDC_HandleTypeDef*l){(void)l;return HAL_OK;}
static void HAL_LCDC_Init(LCDC_HandleTypeDef*l){(void)l;}
static void HAL_LCDC_SetROIArea(LCDC_HandleTypeDef*l,int a,int b,int c,int d){(void)l;(void)a;(void)b;(void)c;(void)d;}
static void HAL_LCDC_LayerSetFormat(LCDC_HandleTypeDef*l,int n,int f){l->Layer[n].data_format=f;}
static void HAL_LCDC_LayerSetData(LCDC_HandleTypeDef*l,int n,void*p,int a,int b,int c,int d){(void)l;(void)n;(void)p;(void)a;(void)b;(void)c;(void)d;}
typedef uint32_t LCDC_AColorDef;
static void HAL_LCDC_LayerSetLTab(LCDC_HandleTypeDef*l,int n,LCDC_AColorDef*p){(void)l;(void)n;(void)p;}
static int HAL_LCDC_SendLayerData_IT(LCDC_HandleTypeDef*l){CHECK(sends<1024);modes_seen[sends++]=ref57_wave_get_selected_mode();if(sends==fail_send_at){if(l->XferErrorCallback)l->XferErrorCallback(l);return -1;}return HAL_OK;}
static void epd_e0470a03_timing_stop(void){}
static void epd_e0470a03_timing_release(void){}
static bool epd_e0470a03_timing_prepare(LCDC_HandleTypeDef*l){(void)l;return true;}
static bool epd_e0470a03_timing_arm(void){return true;}
static bool epd_e0470a03_timing_complete(void){return ++completions!=fail_complete_at;}
static int tps_init(int v){(void)v;return RT_EOK;}
static rt_tick_t tps_disable_vcom(void){return host_tick;}
static void tps_finish_powerdown(rt_tick_t vcom_at){
    rt_tick_t elapsed=host_tick-vcom_at;
    if(elapsed<5)host_tick+=5-elapsed;
    host_tick+=155;
    ++power_offs;
}
static void tps_enter_sleep(void){tps_finish_powerdown(tps_disable_vcom());}
static int tps_start_powerup(void){rail_start=host_tick;host_tick+=3;return RT_EOK;}
static int tps_wait_powerup(void){rt_tick_t elapsed=host_tick-rail_start;if(elapsed<17)host_tick+=17-elapsed;return fail_power?-1:RT_EOK;}
static bool tps_rails_good(void){return !fail_power;}
static int rt_sem_init(struct rt_semaphore*s,const char*n,int v,int f){(void)n;(void)f;s->count=v;return 0;}
static int rt_sem_release(struct rt_semaphore*s){++s->count;return 0;}
static int rt_sem_take(struct rt_semaphore*s,int t){(void)t;CHECK(s->count);--s->count;return 0;}
static rt_thread_t rt_thread_create(const char*n,void(*f)(void*),void*p,int stack,int prio,int tick){(void)n;(void)f;(void)p;(void)stack;(void)prio;(void)tick;return (void*)1;}
static int rt_thread_startup(rt_thread_t t){(void)t;return 0;}
static void *epd_app_alloc(size_t n,int place){(void)place;return malloc(n);}
static rt_device_t rt_device_find(const char*n){(void)n;return (void*)1;}
static void rt_device_set_tx_complete(rt_device_t d,rt_err_t(*f)(rt_device_t,void*)){(void)d;(void)f;}
static struct rt_device_graphic_ops *rt_graphix_ops(rt_device_t d){(void)d;return NULL;}
static uint32_t epd_render_profile_begin(int p){(void)p;return 0;}
static void epd_render_profile_end(int p,uint32_t t){(void)p;(void)t;}
static void mpu_dcache_clean(void*p,unsigned n){(void)p;clean_bytes+=n;host_tick+=3;}
#include "driver.inc"
static LCDC_HandleTypeDef lcd;
static void complete_callback(LCDC_HandleTypeDef*l){(void)l;++callbacks;}
static void reset(void){
 fail_power=false;fail_send_at=fail_complete_at=overflow_complete_at=0;sends=completions=callbacks=power_ons=0;
 memset(&lcd,0,sizeof(lcd));lcd.Layer[0].data_format=LCDC_PIXEL_FORMAT_A4;lcd.XferCpltCallback=complete_callback;
 LCD_Init(&lcd);power_offs=0;memset(mixed,0x77,sizeof(mixed));shadow_known=true;
 epd_wave_submit_frame(EPD_DRAW_MODE_FULL);CHECK(epd_wave_table_get_frames(28,EPD_DRAW_MODE_AUTO)==44);epd_wave_finish_frame(NULL,0,true);
}
static unsigned at(unsigned y){return (PANEL_HEIGHT-1)*PANEL_WIDTH+y;}
static void request(unsigned y,unsigned gray,EpdDrawMode mode){uint8_t pixel=gray;epd_wave_submit_frame(mode);LCD_WriteMultiplePixels(&lcd,&pixel,0,y,0,y);}
/* The real HAL records overflow, then calls the ordinary EOF callback. */
static void eof(void){host_tick+=7;if(completions+1==overflow_complete_at)lcd.ErrorCode|=HAL_LCDC_ERROR_OVERFLOW;CHECK(lcd.XferCpltCallback);lcd.XferCpltCallback(&lcd);}
static void drain(void){unsigned guard=0;while(drawing){CHECK(++guard<500);if(finishing){CHECK(finished.count);--finished.count;host_tick+=5;finish_request();}else eof();}CHECK(!finished.count);}
static void drive_to_finish(void){unsigned guard=0;while(!finishing){CHECK(++guard<100);eof();}}
#include "driver_stats_checks.inc"
#include "driver_gray_checks.inc"
int main(int argc,char**argv){
 CHECK(argc==2);FILE*f=fopen(argv[1],"rb");CHECK(f);bin_size=fread(bin,1,sizeof(bin),f);fclose(f);
 reset();request(0,0,EPD_DRAW_MODE_AUTO);CHECK(mixed[at(0)]==0x70&&!frame_succeeded);drain();CHECK(mixed[at(0)]==0&&shadow_known&&frame_succeeded&&callbacks==1&&sends==29);
 // An untouched panel keeps its old gray and retries pending targets outside the next dirty rectangle.
 reset();fail_power=true;request(0,0,EPD_DRAW_MODE_AUTO);drain();CHECK(!frame_succeeded&&shadow_known&&mixed[at(0)]==0x70&&callbacks==1&&!sends);
 fail_power=false;request(1,15,EPD_DRAW_MODE_INPUT);CHECK(mixed[at(0)]==0x70&&mixed[at(1)]==0x7f);drain();CHECK(frame_succeeded&&shadow_known&&mixed[at(0)]==0&&mixed[at(1)]==0xff&&callbacks==2&&sends==44&&modes_seen[0]==0);
 // Any attempted scan may have moved ink. INIT must finish before claiming a white source.
 reset();fail_complete_at=1;request(0,0,EPD_DRAW_MODE_AUTO);drain();CHECK(!frame_succeeded&&!shadow_known&&mixed[at(0)]==0x70&&callbacks==1);
 fail_complete_at=0;unsigned first=sends;request(1,15,EPD_DRAW_MODE_INPUT);CHECK(recovery_init&&modes_seen[first]==3&&mixed[at(0)]==0x70);
 drive_to_finish();CHECK(!shadow_known&&callbacks==1);--finished.count;finish_request();CHECK(shadow_known&&!recovery_init&&drawing&&mixed[at(0)]==0xf0&&mixed[at(1)]==0xff&&callbacks==1);
 drain();CHECK(frame_succeeded&&shadow_known&&mixed[at(0)]==0&&mixed[at(1)]==0xff&&mixed[at(2)]==0x77&&callbacks==2&&modes_seen[sends-1]==0);
 // Failure inside recovery INIT never creates a trustworthy white state.
 reset();fail_send_at=1;request(0,0,EPD_DRAW_MODE_AUTO);drain();CHECK(!shadow_known);fail_send_at=sends+1;request(1,15,EPD_DRAW_MODE_AUTO);drain();CHECK(!shadow_known&&!frame_succeeded&&mixed[at(0)]==0x70&&callbacks==2);
 fail_send_at=0;request(2,3,EPD_DRAW_MODE_AUTO);CHECK(recovery_init);drain();CHECK(frame_succeeded&&mixed[at(0)]==0&&mixed[at(1)]==0xff&&mixed[at(2)]==0x33&&callbacks==3);
 // If power-good is lost between a successful INIT and GC16, retain white
 // as the physical source and retry the desired pixels without another INIT.
 reset();fail_send_at=1;request(0,0,EPD_DRAW_MODE_AUTO);drain();fail_send_at=0;request(1,15,EPD_DRAW_MODE_AUTO);drive_to_finish();fail_power=true;--finished.count;finish_request();drain();CHECK(shadow_known&&!frame_succeeded&&mixed[at(0)]==0xf0&&mixed[at(2)]==0xf7);
 fail_power=false;first=sends;request(2,3,EPD_DRAW_MODE_AUTO);CHECK(!recovery_init&&modes_seen[first]==0);drain();CHECK(frame_succeeded&&mixed[at(0)]==0&&mixed[at(2)]==0x33);
 // Failure during the reconstruction scan returns to unknown, even after a good INIT.
 reset();fail_send_at=1;request(0,0,EPD_DRAW_MODE_AUTO);drain();fail_send_at=0;request(1,15,EPD_DRAW_MODE_AUTO);drive_to_finish();fail_send_at=sends+1;--finished.count;finish_request();drain();CHECK(!shadow_known&&!frame_succeeded);fail_send_at=0;request(2,3,EPD_DRAW_MODE_AUTO);CHECK(recovery_init);drain();CHECK(frame_succeeded&&callbacks==3);
 // Initial hardware state and explicit INIT both establish a real baseline.
 reset();shadow_known=false;request(0,0,EPD_DRAW_MODE_INPUT);CHECK(recovery_init);drain();CHECK(frame_succeeded&&shadow_known&&mixed[at(0)]==0);
 reset();epd_wave_set_mode(3);request(0,0,EPD_DRAW_MODE_AUTO);drain();CHECK(frame_succeeded&&shadow_known&&mixed[at(0)]==0xff&&mixed[at(1)]==0xff);
 // Overflow at either boundary must not commit even with valid PTM completion.
 for(unsigned end=1;end<=29;end+=28){
  reset();overflow_complete_at=end;request(0,0,EPD_DRAW_MODE_AUTO);drain();
  CHECK(!frame_succeeded&&!shadow_known&&mixed[at(0)]==0x70&&callbacks==1&&sends==end);
  CHECK(lcd.ErrorCode==HAL_LCDC_ERROR_OVERFLOW);
  overflow_complete_at=0;first=sends;request(1,15,EPD_DRAW_MODE_INPUT);
  CHECK(lcd.ErrorCode==HAL_LCDC_ERROR_NONE&&recovery_init&&modes_seen[first]==3);
  drain();CHECK(frame_succeeded&&shadow_known&&callbacks==2&&sends==first+41+44);
  CHECK(mixed[at(0)]==0&&mixed[at(1)]==0xff&&mixed[at(2)]==0x77);
 }
 // An overflow in INIT or its following GC16 leaves recovery uncommitted.
 for(unsigned end=1;end<=42;end+=41){
  reset();shadow_known=false;overflow_complete_at=end;request(0,0,EPD_DRAW_MODE_AUTO);drain();
  CHECK(!frame_succeeded&&!shadow_known&&callbacks==1&&sends==end);
  CHECK((mixed[at(0)]&15)==0&&(mixed[at(0)]>>4)==(end==1?7:15));
  overflow_complete_at=0;first=sends;request(1,15,EPD_DRAW_MODE_INPUT);
  CHECK(recovery_init&&lcd.ErrorCode==HAL_LCDC_ERROR_NONE);drain();
  CHECK(frame_succeeded&&callbacks==2&&sends==first+41+44&&mixed[at(0)]==0);
 }
 driver_stats_checks();
 driver_gray_checks();
 puts("PASS: production driver commit, pre-scan/scan/INIT/reconstruction failures, overflow at first/final EOF and both recovery stages, sticky ErrorCode reset, retained targets and single completion");return 0;
}
