#include "ui_nav_gesture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
#define GUI_APP_ID_MAX_LEN 32
#define rt_strncpy strncpy
enum { NAV_IDLE=-1, POPUP_CLOSE=-2 };
static struct {
    int action;
    bool power, recent, nav_visible, nav_error;
    unsigned selected_recent;
    unsigned navigation_scope;
    uint32_t navigation_popup;
    char opened_ids[2][GUI_APP_ID_MAX_LEN];
} shell;
static bool locked, shutting_down, resetting, handoff, release_requested, popup, center, voice;
static unsigned backs, homes, recents;
static unsigned scope_id;
static uint32_t popup_serial;
static bool gestures_enabled;
static int pending_page;
static bool board_power_pending(void){return shutting_down;}
static bool system_reset_blocks_apps(void){return resetting;}
static bool storage_changing(void){return handoff;}
static bool storage_release_requested(void){return release_requested;}
static bool ui_shell_locked(void){return locked;}
static bool page_popup_visible(void){return popup;}
static bool gesture_navigation(void){return gestures_enabled;}
static unsigned epd_input_scope_id(void){return scope_id;}
static uint32_t navigation_popup(void){return popup_serial;}
static bool ui_control_center_visible(void){return center;}
static bool ui_voice_input_visible(void){return voice;}
static void power_close(void){shell.power=false;}
static void navigation_error_close(void){shell.nav_error=false;}
static void power_open(void){shell.power=true;}
static void power_text(void){}
static bool board_power_request(bool restart){(void)restart;return true;}
static void recent_close(void){shell.recent=false;}
static void recent_open(void){shell.recent=true;++recents;}
static void ui_control_center_close(void){center=false;}
static void ui_voice_input_close(void){voice=false;}
static void ui_voice_input_open(void){voice=true;}
static void epd_input_back(void){++backs;}
static void epd_lock_enter(void){locked=true;}
static void ui_navigation_run(const char *name){if(!strcmp(name,"Main"))++homes;}
static bool entry_find(const char *id){(void)id;return true;}
#include "shell_actions.inc"

static void reset(void)
{
    memset(&shell,0,sizeof(shell));
    locked=shutting_down=resetting=handoff=release_requested=popup=center=voice=false;
    backs=homes=recents=0;pending_page=NAV_IDLE;
    scope_id=popup_serial=0;gestures_enabled=true;
}
static void run(ui_nav_action_t action){navigation_gesture(action);action_process();}
static void unchanged(void){CHECK(!backs&&!homes&&!recents&&pending_page==NAV_IDLE);}
int main(void)
{
    reset();run(UI_NAV_BACK);CHECK(backs==1);action_process();CHECK(backs==1);
    reset();run(UI_NAV_HOME);CHECK(homes==1&&!backs&&!recents);
    reset();run(UI_NAV_RECENT);CHECK(recents==1&&!backs&&!homes);
    for(unsigned gate=0;gate<5;++gate)for(ui_nav_action_t action=UI_NAV_BACK;action<=UI_NAV_RECENT;++action){
        reset();
        if(gate==0)locked=true;else if(gate==1)shutting_down=true;else if(gate==2)resetting=true;
        else if(gate==3)handoff=true;else release_requested=true;
        run(action);unchanged();
    }
    reset();shell.power=true;run(UI_NAV_BACK);CHECK(!shell.power);unchanged();
    reset();shell.recent=true;run(UI_NAV_BACK);CHECK(!shell.recent);unchanged();
    reset();center=true;run(UI_NAV_BACK);CHECK(!center);unchanged();
    reset();voice=true;run(UI_NAV_BACK);CHECK(!voice);unchanged();
    reset();popup=true;run(UI_NAV_BACK);CHECK(pending_page==POPUP_CLOSE&&!backs&&!homes);
    reset();popup=true;pending_page=17;run(UI_NAV_BACK);CHECK(pending_page==17&&!backs);
    reset();popup=true;run(UI_NAV_HOME);unchanged();
    reset();popup=true;run(UI_NAV_RECENT);unchanged();
    reset();shell.power=true;run(UI_NAV_HOME);CHECK(shell.power);unchanged();
    reset();shell.power=true;run(UI_NAV_RECENT);CHECK(shell.power);unchanged();
    reset();shell.action=ACTION_POWER;navigation_gesture(UI_NAV_HOME);action_process();CHECK(shell.power);unchanged();
    reset();scope_id=1;run(UI_NAV_BACK);unchanged();
    reset();popup_serial=1;run(UI_NAV_BACK);unchanged();
    reset();gestures_enabled=false;run(UI_NAV_HOME);unchanged();
    puts("PASS navigation routing: input owner, popup dismissal, lock, shutdown, reset and storage gates");
    return 0;
}
