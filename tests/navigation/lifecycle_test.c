#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
enum { STATUS_HEADSET_TEXT_HEIGHT=20 };
static struct {
    lv_obj_t *power, *power_icon, *power_title, *power_off, *power_restart, *power_cancel;
    lv_obj_t *clock, *clock_prefix, *headset_percent;
    lv_font_t *power_font, *power_fallback, *clock_font, *clock_fallback;
    lv_font_t *clock_prefix_font, *clock_prefix_fallback, *status_font, *status_fallback;
    lv_indev_t *power_indev;
    unsigned power_percent, clock_percent;
    bool clock_chinese_prefix;
    int32_t clock_height, clock_top, clock_placeholder_height, clock_placeholder_top, clock_prefix_height, clock_prefix_top;
} shell;
static struct { const char *clock_text; } status={"12:00"};
static bool idle=true;
static unsigned scale=100, allocated, destroyed;
static unsigned epd_ui_scale_percent(void){return scale;}
static int epd_ui_px(int n){return (n*(int)scale+50)/100;}
static bool epd_refresh_idle(void){return idle;}
static const lv_font_t *ui_font_title(void){return LV_FONT_DEFAULT;}
static const lv_font_t *ui_font_body(void){return LV_FONT_DEFAULT;}
static lv_font_t *ui_font_reader_create(int a,int b,int c,uint32_t *id,lv_font_t **fallback){
    (void)a;(void)b;(void)c;*id=++allocated;*fallback=NULL;
    lv_font_t *font=malloc(sizeof(*font));CHECK(font);*font=*LV_FONT_DEFAULT;return font;
}
static void ui_font_reader_destroy(lv_font_t *f){if(f){CHECK(idle);++destroyed;free(f);}}
static lv_font_t *clock_font_create(bool prefix,lv_font_t **fallback){uint32_t id;return ui_font_reader_create(0,prefix,0,&id,fallback);}
static lv_font_t *status_font_create(const char *text,int size,int x,lv_font_t **fallback){
    (void)text;uint32_t id;return ui_font_reader_create(0,size,x,&id,fallback);
}
static const char *clock_sample(bool prefix){return prefix?"AM":"12:00";}
static int32_t text_visible_height(const lv_font_t *f,const char *text,int32_t *top){(void)text;*top=0;return f->line_height;}
#include "shell_lifecycle.inc"
static void open_power(void){
    shell.power=lv_obj_create(lv_layer_top());shell_font_select(true);
    shell.power_off=lv_label_create(shell.power);shell.power_restart=lv_label_create(shell.power);shell.power_cancel=lv_label_create(shell.power);
    lv_obj_set_style_text_font(shell.power_off,shell.power_font,0);
    lv_obj_set_style_text_font(shell.power_restart,shell.power_font,0);
    lv_obj_set_style_text_font(shell.power_cancel,shell.power_font,0);
}
int main(void){
    lv_init();lv_display_create(684,1216);
    shell.clock=lv_label_create(lv_layer_top());shell.clock_prefix=lv_label_create(lv_layer_top());shell.headset_percent=lv_label_create(lv_layer_top());
    shell_font_select(false);CHECK(allocated==2 && !destroyed);
    open_power();CHECK(allocated==3);lv_font_t *cached=shell.power_font;
    idle=false;
    for(unsigned i=0;i<1000;++i){power_close();CHECK(!shell.power && shell.power_font==cached);open_power();}
    CHECK(allocated==3 && !destroyed);
    scale=120;shell_font_select(true);shell_font_select(false);CHECK(allocated==3 && !destroyed);
    idle=true;shell_font_select(true);shell_font_select(false);CHECK(allocated==6 && destroyed==3);
    idle=false;status.clock_text="上午12:00";shell_font_select(false);CHECK(allocated==6);
    idle=true;shell_font_select(false);CHECK(shell.clock_chinese_prefix && allocated==9 && destroyed==5);
    idle=false;power_close();CHECK(!shell.power && allocated-destroyed==4);
    puts("PASS shell lifecycle: 1000 power reopenings, bounded font cache, deferred scale/locale retirement");
    return 0;
}
