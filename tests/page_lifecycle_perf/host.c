#include "host.h"
#include "types.inc"
#include "prototypes.inc"

/* Geometry and peripheral services are doubles; lifecycle, row construction,
 * retry decisions, QR invalidation and application scans below are production C. */
static void settings_layout(settings_page_t *v) {(void)v;}
static lv_obj_t *device_name_button(lv_obj_t *p,const char *s,int key) {return button_create(p,0,0,1,1,s,key);}
static void reset_rows(settings_page_t *v) {ui_settings_list_row_t r={.key=1};settings_rows(v,&r,1);}
static void information_rows(settings_page_t *v) {ui_settings_list_row_t r={.key=1};settings_rows(v,&r,1);}
static ui_settings_list_row_t setting_item(int id,char *s,size_t n) {snprintf(s,n,"setting");return (ui_settings_list_row_t){.key=id,.title=s};}
static void files_layout(files_view_t *v) {v->page_rows=FILE_ROWS;}
static void files_cursor_close(files_view_t *v) {v->cursor=NULL;}
static void files_track(files_view_t *v) {(void)v;}
static void files_render_directory(files_view_t *v) {(void)v;CHECK(false);}
static void app_volume_layout(app_list_t *v) {(void)v;}
static void app_progress_update(app_list_t *v) {(void)v;}
#include "pages.inc"

typedef struct {int id;void (*create)(lv_obj_t *);void (*resume)(void);} registration_t;
static const registration_t registrations[]={
#include "registrations.inc"
};
static void fixture(int id)
{
    memset(&context,0,sizeof(context));memset(objects,0,sizeof(objects));object_count=0;
    static lv_group_t group;memset(&group,0,sizeof(group));current=&context;
    current->id=id;current->screen=new_object();current->group=&group;
    fills=snapshots=scans=qr_updates=show_calls=0;
    rows_fail=show_fail=unavailable=changing=releasing=snapshot_fail=qr_fail=false;
    alloc_fail=-1;option_index=35;timezone=487;generation=1;
    portal_state=(portal_snapshot_t){.phase=PORTAL_ON,.revision=1,.lan=true};
    strcpy(portal_state.url,"http://192.0.2.1/");
    accounts.visible=false;
}
static void stop_page(void)
{
    if (current->id==UI_PAGE_FILES||current->id==UI_PAGE_STORAGE||current->id==UI_PAGE_FILE_PICKER) {
        files_view_t *v=current->feature;if(v){ui_settings_list_destroy(v->volumes);free(v);}current->feature=NULL;
    } else if (current->id==UI_PAGE_MEMORY) memory_page_stop();
    else if (current->id==UI_PAGE_PORTAL||current->id==UI_PAGE_PORTAL_WEB) portal_page_stop();
    else if (current->id==UI_PAGE_APP_INSTALL||current->id==UI_PAGE_APP_UNINSTALL||current->id==UI_PAGE_APP_SETTINGS) app_list_stop();
    else settings_stop();
}
static ui_settings_list_t *page_list(void)
{
    if (current->id==UI_PAGE_FILES||current->id==UI_PAGE_STORAGE||current->id==UI_PAGE_FILE_PICKER) return ((files_view_t *)current->feature)->volumes;
    if(current->id==UI_PAGE_MEMORY)return ((memory_view_t *)current->feature)->list;
    if(current->id==UI_PAGE_APP_INSTALL||current->id==UI_PAGE_APP_UNINSTALL||current->id==UI_PAGE_APP_SETTINGS)return ((app_list_t *)current->feature)->rows;
    return ((settings_page_t *)current->feature)->list;
}
static void lifecycle(void)
{
    for(size_t i=0;i<sizeof(registrations)/sizeof(registrations[0]);++i){
        const registration_t *r=&registrations[i];fixture(r->id);
        r->create(current->screen);
        CHECK(fills==0&&snapshots==0&&scans==0&&qr_updates==0);
        current->foreground=true;r->resume();
        bool portal=r->id==UI_PAGE_PORTAL||r->id==UI_PAGE_PORTAL_WEB;
        if(portal)CHECK(snapshots==1&&qr_updates==1);
        else {CHECK(fills==1);CHECK(page_list()->count>0);CHECK(page_list()->group==current->group);}
        current->foreground=false;
        ++generation;++portal_state.revision;strcpy(portal_state.url,"http://192.0.2.2/");
        fills=snapshots=scans=qr_updates=0;current->foreground=true;r->resume();
        if(portal){CHECK(snapshots==1&&qr_updates==1);CHECK(!strcmp(((portal_view_t *)current->feature)->qr_value,portal_state.url));}
        else CHECK(fills==1);
        if(r->id==UI_PAGE_MEMORY)CHECK(strstr(page_list()->values[0],"2/1000"));
        if(r->id==UI_PAGE_FILES||r->id==UI_PAGE_STORAGE||r->id==UI_PAGE_FILE_PICKER)CHECK(!strcmp(page_list()->values[0],"2/1000"));
        if(r->id==UI_PAGE_APP_SETTINGS||r->id==UI_PAGE_APP_UNINSTALL)CHECK(!strcmp(page_list()->titles[0],"app 2"));
        stop_page();
    }
    puts("PASS 25 registered pages: create=0 fills, first resume=1, return resume=1; service changes visible");
}
static void initial_selection(void)
{
    const int ids[]={UI_PAGE_SETTING_OPTIONS,UI_PAGE_TIMEZONE_SETTINGS};
    for(unsigned i=0;i<2;++i){
        fixture(ids[i]);settings_list_create(current->screen);settings_page_t *v=current->feature;
        CHECK(v->initial_position_pending&&show_calls==0);
        rows_fail=true;settings_resume();CHECK(current->locale_retry&&v->initial_position_pending&&show_calls==0);
        rows_fail=false;show_fail=true;current->locale_retry=false;settings_resume();
        CHECK(current->locale_retry&&v->initial_position_pending);
        show_fail=false;current->locale_retry=false;settings_resume();
        CHECK(!v->initial_position_pending);
        CHECK(v->list->shown==(i?TIMEZONE_BASE+timezone+720:OPTION_BASE+(int)option_index));
        unsigned calls=show_calls;int shown=v->list->shown;
        option_index=4;timezone=-345;settings_resume();
        CHECK(show_calls==calls&&v->list->shown==shown);
        bool checked=false;int key=i?TIMEZONE_BASE+timezone+720:OPTION_BASE+4;
        for(size_t n=0;n<v->list->count;++n)if(v->list->rows[n].key==key)checked=v->list->rows[n].checked;
        CHECK(checked);settings_stop();
        fixture(ids[i]);settings_list_create(current->screen);v=current->feature;
        alloc_fail=0;settings_resume();CHECK(v->initial_position_pending&&current->locale_retry);
        alloc_fail=-1;settings_resume();CHECK(!v->initial_position_pending);settings_stop();
    }
    puts("PASS option/timezone initial selection, non-quarter-hour timezone, OOM/set_rows/show_key retry and no return jump");
}
static void portal_cases(void)
{
    fixture(UI_PAGE_PORTAL);portal_state.lan=false;portal_state.hotspot_requested=true;
    strcpy(portal_state.ssid,"Reader");strcpy(portal_state.password,"secret");
    portal_page_create(current->screen);portal_page_resume();portal_view_t *v=current->feature;
    CHECK(qr_updates==1&&strstr(v->qr_value,"WIFI:")&&strstr(v->qr_value,"Reader"));
    portal_page_resume();CHECK(qr_updates==1);
    ++portal_state.revision;strcpy(portal_state.password,"changed");portal_page_resume();CHECK(qr_updates==2&&strstr(v->qr_value,"changed"));
    qr_fail=true;strcpy(portal_state.password,"fail");portal_page_resume();CHECK(v->qr->hidden&&!v->qr_value[0]);
    qr_fail=false;portal_page_resume();CHECK(!v->qr->hidden&&strstr(v->qr_value,"fail"));
    portal_state.phase=PORTAL_OFF;portal_page_resume();CHECK(v->qr->hidden&&!v->qr_value[0]);
    accounts.visible=true;unsigned before=snapshots;portal_page_resume();CHECK(snapshots==before&&v->body->hidden);
    accounts.visible=false;portal_page_resume();CHECK(!v->body->hidden);portal_page_stop();
    fixture(UI_PAGE_PORTAL_WEB);portal_state.lan=false;portal_state.hotspot_requested=true;
    portal_page_create(current->screen);portal_page_resume();v=current->feature;CHECK(!strcmp(v->qr_value,portal_state.url));portal_page_stop();
    puts("PASS portal LAN/AP/web QR, unchanged payload reuse, QR failure retry, off state and account panel");
}
static void failures(void)
{
    for(size_t i=0;i<sizeof(registrations)/sizeof(registrations[0]);++i){
        fixture(registrations[i].id);alloc_fail=0;registrations[i].create(current->screen);
        CHECK(!current->feature);registrations[i].resume();CHECK(!fills&&!snapshots&&!scans);stop_page();
    }
    fixture(UI_PAGE_MEMORY);memory_page_create(current->screen);rows_fail=true;memory_page_resume();
    memory_view_t *m=current->feature;CHECK(m->dirty);rows_fail=false;memory_page_process();CHECK(!m->dirty);
    snapshot_fail=true;memory_page_resume();CHECK(!strcmp(m->list->values[0],"platform_read_failed"));memory_page_stop();
    fixture(UI_PAGE_FILES);files_create(current->screen);unavailable=true;files_resume();
    CHECK(!strcmp(page_list()->values[1],"platform_card_unmounted"));
    rows_fail=true;files_resume();CHECK(current->locale_retry);rows_fail=false;unavailable=false;files_resume();CHECK(!strcmp(page_list()->values[0],"1/1000"));stop_page();
    fixture(UI_PAGE_APP_INSTALL);app_list_create(current->screen);unavailable=true;app_list_resume();
    CHECK(!strcmp(page_list()->titles[1],"platform_insert_card"));unavailable=false;app_list_resume();
    CHECK(!strcmp(page_list()->titles[1],"demo.sap"));app_list_stop();
    fixture(UI_PAGE_APP_UNINSTALL);app_list_create(current->screen);changing=true;app_list_resume();
    app_list_t *a=current->feature;CHECK(a->dirty&&!fills&&!scans);changing=false;app_list_process();CHECK(!a->dirty&&fills==1);
    ++generation;alloc_fail=0;app_list_resume();CHECK(a->dirty&&a->count==1);alloc_fail=-1;app_list_process();CHECK(!a->dirty&&!strcmp(a->rows->titles[0],"app 2"));app_list_stop();
    puts("PASS create OOM, memory sample/row failure, unavailable storage/card, storage gate and app-list allocation recovery");
}
int main(void)
{
    lifecycle();initial_selection();portal_cases();failures();
    puts("All page lifecycle source-extraction checks passed.");return 0;
}
