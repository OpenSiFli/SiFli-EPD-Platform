#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <crtdbg.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_nav_gesture.h"
#include "ui_shell_model.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
static uint32_t now;
static lv_indev_t *pointer;
static lv_point_t point;
static bool pressed;
static unsigned dispatched, clicks, gestures, pointer_clicks;
static ui_nav_action_t result;
enum { HEIGHT=1216, WIDTH=684, UI_SETTING_TOUCH };
typedef struct shell_page { lv_obj_t *screen; struct shell_page *next; } shell_page_t;
static struct {
    bool power, recent, nav_error, nav_visible, transient_status;
    uint32_t hide_status_at, navigation_scope, navigation_popup;
    int action;
    lv_obj_t *screen;
    shell_page_t *pages;
    ui_shell_gesture_t gesture;
} shell;
static bool touch=true, locked, center_visible, popup, power_pending, resetting, storage_busy, storage_release, sleeping;
static bool gesture_mode=true, page_immersive, voice;
static unsigned input_scope, popup_serial, ui_scale=100;
static bool ui_settings_enabled(int setting) { (void)setting; return touch; }
static bool ui_shell_locked(void) { return locked; }
static bool ui_control_center_visible(void) { return center_visible; }
static bool ui_voice_input_visible(void) { return voice; }
static void ui_voice_input_close(void) { voice=false; }
static void recent_close(void) { shell.recent=false; }
static void ui_shell_page_mode(lv_obj_t *screen, bool fullscreen) { CHECK(screen==lv_screen_active()); page_immersive=fullscreen; }
static bool page_popup_visible(void) { return popup; }
static bool board_power_pending(void) { return power_pending; }
static bool system_reset_blocks_apps(void) { return resetting; }
static bool storage_changing(void) { return storage_busy; }
static bool storage_release_requested(void) { return storage_release; }
static bool epd_power_sleeping(void) { return sleeping; }
static bool gesture_navigation(void) { return gesture_mode; }
static bool immersive(void) { return page_immersive; }
static unsigned epd_ui_scale_percent(void) { return ui_scale; }
static int epd_ui_px(int v) { return (v*(int)ui_scale+50)/100; }
static unsigned epd_input_scope_id(void) { return input_scope; }
static unsigned navigation_popup(void) { return popup_serial; }
static lv_obj_t *flat(lv_obj_t *p,int x,int y,int w,int h) {
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);return o;
}
#include "shell_edges.inc"
__declspec(align(32)) static unsigned char pixels[684 * 1216 * 4];
static uint32_t ticks(void) { return now; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{ (void)input; data->point = point; data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void dispatch(ui_nav_action_t action)
{
    navigation_gesture(action);
    if (shell.action == ACTION_NONE) return;
    result = action; ++dispatched;
}
static void pointer_clicked(lv_event_t *event) { (void)event; ++pointer_clicks; }
static void content_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) ++clicks;
    if (lv_event_get_code(event) == LV_EVENT_GESTURE) ++gestures;
}
static void sample(int x, int y, bool down, uint32_t elapsed)
{
    now += elapsed;
    point.x = x; point.y = y; pressed = down;
    lv_obj_update_layout(lv_screen_active());
    lv_obj_update_layout(lv_layer_top());
    lv_indev_read(pointer);
}
static void reset(void)
{
    sample(340, 500, false, 10);
    lv_indev_reset(pointer, NULL);
    ui_nav_gesture_cancel();
    dispatched = clicks = gestures = pointer_clicks = 0; result = UI_NAV_NONE;
    shell.gesture=UI_SHELL_NONE;
    shell.action=ACTION_NONE;
    shell.power=shell.recent=shell.nav_error=shell.nav_visible=shell.transient_status=false;
    locked=center_visible=popup=power_pending=resetting=storage_busy=storage_release=sleeping=page_immersive=voice=false;
    touch=gesture_mode=true; input_scope=popup_serial=0;ui_scale=100;
    shell.screen=lv_screen_active();
    navigation_update();
}
static void swipe(int x, int y, int end_x, int end_y)
{ sample(x,y,true,10); sample(end_x,end_y,true,90); sample(end_x,end_y,false,20); }
static void isolated(ui_nav_action_t expected)
{ CHECK(result == expected && dispatched == 1 && !clicks && !gestures); }
static lv_obj_t *new_page(void)
{
    lv_obj_t *screen=lv_obj_create(NULL), *content=lv_obj_create(screen);
    lv_obj_remove_style_all(content); lv_obj_set_size(content,WIDTH,HEIGHT);
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(content,content_event,LV_EVENT_ALL,NULL);
    lv_screen_load(screen);
    return screen;
}
static void switch_window_cases(void)
{
    const int paths[][4]={{4,500,110,500},{680,500,580,500},{340,1208,340,1090}};
    /* New page, optional new input owner, and DOWN before shell catches up.
     * Exercise both a complete buffered swipe and one spanning the next UI step. */
    for(unsigned route=0;route<3;++route)for(unsigned scope=0;scope<2;++scope)for(unsigned step=0;step<2;++step) {
        reset();
        lv_obj_t *old=lv_screen_active(), *screen=new_page();
        if(scope)++input_scope;
        uint32_t callbacks=lv_obj_get_event_count(screen);
        sample(paths[route][0],paths[route][1],true,10);
        CHECK(shell.navigation_scope==input_scope);
        CHECK(lv_obj_get_event_count(screen)==callbacks+1);
        if(step) { shell_screen_process(); navigation_update(); }
        sample(paths[route][2],paths[route][3],false,80);
        isolated(route==2?UI_NAV_HOME:UI_NAV_BACK);
        CHECK(lv_obj_get_event_count(screen)==callbacks);
        lv_obj_delete(old);
    }
    reset();
    lv_obj_t *old=lv_screen_active(), *screen=new_page(); ++input_scope;
    sample(340,1208,true,10); shell_screen_process(); navigation_update();
    sample(340,1090,true,80); sample(340,1090,true,350); isolated(UI_NAV_RECENT);
    sample(340,1090,false,10); CHECK(dispatched==1); lv_obj_delete(old);

    /* A normal tap on the new page stays with the page, including both sides. */
    for(unsigned route=0;route<3;++route) {
        reset(); old=lv_screen_active(); screen=new_page(); ++input_scope;
        uint32_t callbacks=lv_obj_get_event_count(screen);
        sample(paths[route][0],paths[route][1],true,10);
        shell_screen_process(); navigation_update();
        sample(paths[route][0],paths[route][1],false,20);
        CHECK(clicks==1 && pointer_clicks==1 && !dispatched);
        CHECK(lv_obj_get_event_count(screen)==callbacks); lv_obj_delete(old);
    }
    /* The current modal gate must be applied even before the next shell step. */
    reset(); old=lv_screen_active(); screen=new_page(); ++input_scope;
    center_visible=true; swipe(340,1208,340,1090); CHECK(!dispatched); lv_obj_delete(old);
    reset(); sample(4,500,true,10); ++input_scope;
    navigation_update(); sample(110,500,false,20); CHECK(!dispatched);
    reset(); sample(4,500,true,10); ++popup_serial;
    navigation_update(); sample(110,500,false,20); CHECK(!dispatched);

    /* Unload during unclaimed and claimed contacts, then reuse the input. */
    for(unsigned claimed=0;claimed<2;++claimed) {
        reset(); old=lv_screen_active(); sample(4,500,true,10);
        if(claimed)sample(110,500,true,20);
        screen=new_page(); ++input_scope; lv_obj_delete(old);
        ui_nav_gesture_cancel();
        sample(110,500,false,20); CHECK(!dispatched);
        swipe(4,500,110,500); isolated(UI_NAV_BACK);
    }
    /* The deletion subscription is bounded to the current contact. */
    for(unsigned i=0;i<1000;++i) {
        reset(); screen=lv_screen_active(); uint32_t callbacks=lv_obj_get_event_count(screen);
        sample(4,500,true,10); CHECK(lv_obj_get_event_count(screen)==callbacks+1);
        sample(4,500,false,20); CHECK(lv_obj_get_event_count(screen)==callbacks);
        CHECK(clicks==1 && !dispatched);
    }
    puts("PASS switch window: new DOWN, real scope routing, next shell step, page taps, deletion and 1000 bounded subscriptions");
}
int main(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    lv_init(); lv_tick_set_cb(ticks);
    lv_display_t *display = lv_display_create(684,1216);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(content); lv_obj_set_size(content,684,1216);
    lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(content,content_event,LV_EVENT_ALL,NULL);
    const int targets[][4]={{0,450,20,110},{664,450,20,110},{60,0,100,64},{300,1190,100,26}};
    for(unsigned i=0;i<sizeof(targets)/sizeof(targets[0]);++i) {
        lv_obj_t *button=lv_button_create(content);lv_obj_remove_style_all(button);
        lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_pos(button,targets[i][0],targets[i][1]);lv_obj_set_size(button,targets[i][2],targets[i][3]);
        lv_obj_add_event_cb(button,content_event,LV_EVENT_ALL,NULL);
    }
    pointer = lv_indev_create(); lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,read_pointer);
    lv_indev_add_event_cb(pointer,pointer_clicked,LV_EVENT_CLICKED,NULL);
    lv_timer_pause(lv_indev_get_read_timer(pointer));
    edge_pointer.read=read_pointer; lv_indev_set_read_cb(pointer,edge_pointer_read);
    lv_obj_t *top=edge_create(0,64), *bottom=edge_create(1184,32);
    CHECK(!lv_obj_has_flag(top,LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag(bottom,LV_OBJ_FLAG_CLICKABLE));
    ui_nav_gesture_init(dispatch);

    reset(); swipe(4,500,110,500); isolated(UI_NAV_BACK);
    reset(); swipe(680,500,580,500); isolated(UI_NAV_BACK);
    reset(); swipe(4,500,32,500); CHECK(!dispatched && !clicks && !gestures);
    reset(); sample(4,500,true,10); sample(4,500,false,20); CHECK(!dispatched && clicks == 1 && pointer_clicks == 1);
    reset(); sample(680,500,true,10); sample(680,500,false,20); CHECK(!dispatched && clicks==1 && pointer_clicks==1);
    reset(); sample(4,500,true,10); sample(8,503,true,20); sample(8,503,false,20); CHECK(clicks==1 && pointer_clicks==1 && !dispatched);
    reset(); sample(4,500,true,10); sample(110,500,false,80); isolated(UI_NAV_BACK);
    sample(200,500,true,10); sample(200,500,false,20); CHECK(clicks==1 && pointer_clicks==1);
    reset(); sample(4,500,true,10); sample(110,500,true,80); sample(8,500,false,30); CHECK(!dispatched);
    reset(); sample(4,500,true,10); sample(8,580,true,30); sample(140,580,false,40); CHECK(!dispatched);
    reset(); swipe(100,500,400,500); CHECK(!dispatched && gestures);
    reset(); sample(200,300,true,10); sample(200,300,false,20); CHECK(clicks == 1 && !dispatched);
    reset(); sample(340,1208,true,10); sample(340,1208,false,20); CHECK(clicks == 1 && pointer_clicks == 1 && !dispatched);
    reset(); sample(100,1208,true,10); sample(103,1204,true,20); sample(103,1204,false,20);
    CHECK(clicks == 1 && !dispatched);
    reset(); swipe(340,1208,340,1090); isolated(UI_NAV_HOME);
    reset(); sample(340,1208,true,10); sample(340,1090,true,80);
    sample(340,1090,true,349); CHECK(!dispatched); sample(340,1090,true,1); isolated(UI_NAV_RECENT);
    sample(340,1090,false,20); CHECK(dispatched == 1 && !clicks && !gestures);
    reset(); sample(340,1208,true,10); sample(340,1208,true,1000); sample(340,1090,false,80); isolated(UI_NAV_HOME);
    reset(); sample(340,1208,true,10);
    for(int y=1120;y>=900;y-=20)sample(340,y,true,100);
    sample(340,900,false,10); isolated(UI_NAV_HOME);
    reset(); swipe(340,1208,460,1185); CHECK(!dispatched);
    reset(); sample(340,1208,true,10); sample(340,1090,true,80); sample(340,1205,false,30); CHECK(!dispatched);
    reset(); swipe(340,1140,340,950); CHECK(!dispatched && gestures);

    reset(); sample(100,10,true,10); sample(100,10,false,20); CHECK(clicks==1 && pointer_clicks==1 && shell.gesture==UI_SHELL_NONE);
    reset(); sample(600,10,true,10); sample(603,13,true,20); sample(603,13,false,20); CHECK(clicks==1 && shell.gesture==UI_SHELL_NONE);
    reset(); swipe(100,10,100,70); CHECK(shell.gesture==UI_SHELL_NOTIFICATIONS && !clicks && !gestures && !dispatched);
    reset(); sample(600,10,true,10); sample(600,70,false,80); CHECK(shell.gesture==UI_SHELL_CONTROLS && !clicks && !pointer_clicks && !gestures);
    sample(200,500,true,10); sample(200,500,false,20); CHECK(clicks==1 && pointer_clicks==1);
    reset(); sample(100,10,true,10); sample(150,12,true,20); sample(150,100,false,20); CHECK(shell.gesture==UI_SHELL_NONE);
    reset(); page_immersive=true; swipe(100,12,100,400);
    CHECK(shell.gesture==UI_SHELL_NOTIFICATIONS && !clicks && !gestures && !dispatched);
    reset(); page_immersive=true; swipe(500,12,500,400);
    CHECK(shell.gesture==UI_SHELL_CONTROLS && !clicks && !pointer_clicks && !gestures && !dispatched);
    reset(); page_immersive=true; sample(500,12,true,10); sample(500,400,false,454);
    CHECK(shell.gesture==UI_SHELL_CONTROLS && !clicks && !pointer_clicks && !gestures);
    reset(); page_immersive=true; sample(100,12,true,10); sample(102,14,false,20);
    CHECK(clicks==1 && pointer_clicks==1 && shell.gesture==UI_SHELL_NONE);
    reset(); page_immersive=true; swipe(500,24,500,400); CHECK(shell.gesture==UI_SHELL_NONE);
    reset(); page_immersive=true;shell.transient_status=true;swipe(600,30,600,90);CHECK(shell.gesture==UI_SHELL_CONTROLS);
    reset(); ui_nav_gesture_update(false,false,100,1);gesture_mode=false;
    sample(340,1208,true,10);sample(340,1208,false,20);CHECK(clicks==1 && shell.gesture==UI_SHELL_NONE);
    swipe(340,1208,340,1150);CHECK(shell.gesture==UI_SHELL_REVEAL_NAV && clicks==1);
    for(unsigned gate=0;gate<11;++gate) {
        reset();
        if(gate==0)touch=false;else if(gate==1)locked=true;else if(gate==2)center_visible=true;
        else if(gate==3)popup=true;else if(gate==4)power_pending=true;else if(gate==5)resetting=true;
        else if(gate==6)storage_busy=true;else if(gate==7)storage_release=true;else if(gate==8)sleeping=true;
        else if(gate==9)shell.recent=true;else shell.power=true;
        swipe(100,10,100,70);CHECK(shell.gesture==UI_SHELL_NONE);
    }
    reset();sample(100,10,true,10);++input_scope;sample(100,70,false,30);CHECK(shell.gesture==UI_SHELL_NONE);
    reset();sample(600,10,true,10);++popup_serial;sample(600,70,false,30);CHECK(shell.gesture==UI_SHELL_NONE);
    reset();ui_scale=120;swipe(600,10,600,50);CHECK(shell.gesture==UI_SHELL_NONE);
    swipe(600,10,600,60);CHECK(shell.gesture==UI_SHELL_CONTROLS);

    reset();lv_display_set_rotation(display,LV_DISPLAY_ROTATION_180);ui_nav_gesture_update(true,true,100,1);
    swipe(679,715,573,715);isolated(UI_NAV_BACK);
    reset();sample(679,715,true,10);sample(679,715,false,20);CHECK(clicks==1 && !dispatched);
    reset();swipe(83,1205,83,1145);CHECK(shell.gesture==UI_SHELL_CONTROLS && !clicks);
    lv_display_set_rotation(display,LV_DISPLAY_ROTATION_0);

    reset(); gesture_mode=false; swipe(4,500,110,500); CHECK(!dispatched && gestures);
    reset(); center_visible=true; swipe(340,1208,340,1090); CHECK(!dispatched);
    reset(); center_visible=true; swipe(4,500,110,500); isolated(UI_NAV_BACK);
    reset(); sample(4,500,true,10); sample(110,500,true,80); ui_nav_gesture_update(false,false,100,1);
    sample(110,500,false,20); CHECK(!dispatched);
    reset(); sample(4,500,true,10); sample(110,500,true,80); ui_nav_gesture_update(true,true,100,2);
    sample(110,500,false,20); CHECK(!dispatched);
    reset(); sample(4,500,true,10); sample(110,500,true,80); ui_nav_gesture_update(true,true,120,1);
    sample(110,500,false,20); CHECK(!dispatched);
    reset(); ui_scale=120; swipe(4,500,70,500); CHECK(!dispatched);
    swipe(4,500,100,500); isolated(UI_NAV_BACK);
    reset(); sample(4,500,true,10); sample(110,500,true,80);
    lv_obj_t *replacement=lv_obj_create(NULL); lv_screen_load(replacement);
    ui_nav_gesture_update(true,true,100,1); sample(110,500,false,20); CHECK(!dispatched);
    reset();page_immersive=true;sample(500,12,true,10);
    lv_screen_load(lv_obj_create(NULL));sample(500,400,false,20);CHECK(shell.gesture==UI_SHELL_NONE);
    swipe(500,12,500,400);CHECK(shell.gesture==UI_SHELL_CONTROLS);
    switch_window_cases();
    CHECK(ui_shell_edge_gesture(100,1200,0,-60,false,true,false,false)==UI_SHELL_REVEAL_NAV);
    CHECK(ui_shell_edge_gesture(100,0,0,60,false,true,false,false)==UI_SHELL_NOTIFICATIONS);
    CHECK(ui_shell_edge_gesture(600,0,0,60,false,true,false,false)==UI_SHELL_CONTROLS);
    CHECK(ui_shell_edge_gesture(100,1200,0,-60,false,true,false,true)==UI_SHELL_NONE);
    CHECK(ui_shell_edge_gesture(100,600,0,-60,false,true,true,false)==UI_SHELL_CLOSE_CENTER);
    puts("PASS navigation: passive edges, page taps, pull-downs, release-only swipes, cancellation, hold, gates, scale and rotation");
    return 0;
}
