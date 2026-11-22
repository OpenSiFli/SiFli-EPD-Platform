/* Production retry gates with real core capacity notifications. */
#define view near_view
#define state near_state
typedef struct view_frame { struct view_frame *next; unsigned layout; } view_frame_t;
static struct {
    bool pixels_blocked,pixels_memory_blocked,pixels_retry_ready;
    unsigned session,intent,pixels_blocked_intent,pixels_blocked_priority,layout;
    uint32_t pixels_blocked_epoch;
    size_t pixels_blocked_bytes,pixels_blocked_available;
    view_frame_t *frames;
} near_view;
static size_t frame_bytes;
static const void *state=&near_view;
static unsigned frame_priority(const void *owner,unsigned number) { CHECK(owner==state);return number; }
/* This fixture exercises heap progress without reusable frame storage. */
static bool frame_spare_available(const view_frame_t *frame) { (void)frame;return false; }
#include "reader_near_functions.inc"
static void near_activity_case(void)
{
    near_view.intent=1;
    void *pixels=epd_app_alloc(65536,EPD_APP_PSRAM);CHECK(pixels);frame_bytes=65536;
    optional_pixels_block(-3,2);
    for(unsigned i=0;i<1000;++i){
        uint32_t before=epd_memory_capacity_epoch();
        epd_memory_owner_t task=epd_memory_scope_create(app,EPD_MEMORY_SCOPE_TASK);
        CHECK(task!=EPD_MEMORY_OWNER_INVALID);
        epd_memory_owner_close(task);epd_memory_owner_release(task);
        CHECK(epd_memory_capacity_epoch()!=before);
        CHECK(!optional_pixels_ready()&&"UNRELATED_OWNER_CLOSE_MUST_NOT_RETRY_NEARBY_FRAME");
    }
    CHECK(!optional_pixels_ready_for(2)&&!optional_pixels_ready_for(3));
    CHECK(optional_pixels_ready_for(1));
    epd_app_free(pixels);frame_bytes=0;CHECK(optional_pixels_ready());
    optional_pixels_block(-3,2);CHECK(!optional_pixels_ready());
    ++near_view.intent;CHECK(optional_pixels_ready());
    optional_pixels_block(-1,2);
    ++frame_bytes;--frame_bytes;CHECK(!optional_pixels_ready());
    ++near_view.intent;CHECK(optional_pixels_ready());
}
#undef view
#undef state
