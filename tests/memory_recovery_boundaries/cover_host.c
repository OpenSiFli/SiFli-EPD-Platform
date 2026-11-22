/* UI/storage boundaries are fixtures; the complete books_process and shelf
 * provider functions below come from the selected production source files. */
typedef struct { unsigned ticks_per_second,source_bitmap_ticks,glyph_hits,glyph_misses,source_bitmap_calls,prepare_failures; } epd_app_font_cache_stats_t;
typedef struct { bool ready,indexing; unsigned error_code,revision,pages,page; char error[8]; } ui_reader_view_t;
static ui_reader_view_t test_reading={.ready=true};
static struct { bool foreground,reader_has_content,reader_confirm_pending,reader_reflow_pending; unsigned id,locale_revision,reader_revision,reader_jump,reader_reflow_page; void *popup,*reader_body,*reader_panel; } test_page,*current=&test_page;
static bool reader_page_live,reader_session,refresh_pending,refresh_busy,reader_resume_pending;
static unsigned refresh_page,refresh_started,pending_page;
static struct { bool active; unsigned last,last_step_ms,step_ms,max_step_ms; } submit_trace;
static epd_app_font_cache_stats_t refresh_glyphs;
#define UI_PAGE_READER 2
#define UI_PAGE_BOOKSHELF 1
#define UI_PAGE_HOME 1
#define UI_PAGE_COUNT 3
#define NAV_IDLE 0
#define NAV_BACK 3
#define LV_OBJ_FLAG_HIDDEN 1
#define BOOKS_NOTICE_SCAN 0
#define READER_ERROR_NONE 0
static bool lv_refreshing_done(void){return !display_busy;}
static bool epd_app_refresh_can_prepare(void){return false;}
static bool ui_reader_cache_process(void){return false;}
static const ui_reader_view_t *ui_reader_view(void){return &test_reading;}
static bool ui_reader_preview_active(void){return false;}
static void reader_render(void){}
static bool books_menu_active(void){return false;}
static bool epd_reader_note_visible(void){return false;}
static bool lv_obj_has_flag(void *p,unsigned f){(void)p;(void)f;return true;}
static bool books_menu_font_pending(void){return false;}
static void ui_reader_background_pause(bool v){(void)v;}
static void epd_app_font_cache_stats(epd_app_font_cache_stats_t *s){memset(s,0,sizeof(*s));}
static unsigned lv_tick_elaps(unsigned n){(void)n;return 0;}
static void books_scale_refresh(void){}
static unsigned epd_locale_revision(void){return 0;}
static void books_locale_refresh(void){}
static void bookshelf_update(bool reset){(void)reset;}
static void books_command_scan_result(int e){(void)e;}
static void popup_show(int a,int b,int c){(void)a;(void)b;(void)c;}
static bool document_view_settings_changed(void){return false;}
static bool document_view_waiting(void){return false;}
static bool document_view_active(void){return true;}
static unsigned launcher_current_page(void){return current->id;}
static bool reader_resume_prepare(void){return false;}
static bool ui_reader_process(void){return false;}
static bool ui_reader_waiting(void){return false;}
static bool ui_reader_settings_changed(void){return false;}
static bool ui_reader_open_pending(void){return false;}
static void books_menu_navigate(void){}
static void reader_submit_poll(bool idle){(void)idle;}
static unsigned lv_tick_get(void){return 0;}
typedef int ui_reader_work_t;
#define UI_READER_WORK_IDLE 0
static ui_reader_work_t ui_reader_step(unsigned n,bool f,bool r){(void)n;(void)f;(void)r;return 0;}
static void reader_status_render(void){}
static void reader_option_render(void){}
static void ui_reader_close(void){}
static void bookshelf_resume(void){}
static void reader_panel_close(void){}
static void reader_update(void){}
static void ui_reader_cache_idle(void *b,bool idle){(void)b;(void)idle;}
#include "books_process.inc"

int main(int argc,char **argv)
{
    assert(argc==2); manager_setup();
    if(!strcmp(argv[1],"legacy")) return legacy_cover_main();
    seed(4); ui_bookshelf_metadata_request(0,4); publish(); pump(0);
    size_t before=shelf_cover_bytes; assert(before>0);
    ui_bookshelf_metadata_cancel();
    current->foreground=true;current->id=UI_PAGE_READER;reader_session=true;
    reader_page_live=true;
    if(!strcmp(argv[1],"idle_keep")) {
        for(unsigned n=0;n<10;++n)books_process();
        assert(shelf_cover_bytes==before);
    } else {
        unsigned cycles=!strcmp(argv[1],"repeat")?3:1;
        for(unsigned n=0;n<cycles;++n) {
            /* Offer while idle, then start a draw before the selected callback. */
            shelf_resource_publish();
            assert(epd_resource_reclaim(1,before,16)>0);
            display_busy=true;
            epd_resource_process(EPD_RESOURCE_UI,32);
            assert(shelf_cover_bytes==before);
            for(unsigned i=0;i<3;++i)books_process();
            assert(shelf_cover_bytes==before);
            assert(epd_resource_reclaim(1,before,16)==0);
            display_busy=false;
            if(!strcmp(argv[1],"busy_protect")) break;
            books_process();
            size_t selected=epd_resource_reclaim(1,before,16);
            printf("idle reader selected=%zu resident=%zu\n",selected,shelf_cover_bytes);fflush(stdout);
            if(!selected) return 23;
            if(n+1<cycles){display_busy=true;epd_resource_process(EPD_RESOURCE_UI,32);display_busy=false;books_process();}
            else {epd_resource_process(EPD_RESOURCE_UI,32);assert(shelf_cover_bytes<before);}
        }
    }
    display_busy=false;ui_bookshelf_close();
    epd_resource_process(EPD_RESOURCE_UI,64);epd_resource_process(EPD_RESOURCE_WORKER,64);
    assert(!activity_tasks && activity_queued==activity_closed);
    printf("PASS shelf boundary %s\n",argv[1]);return 0;
}
