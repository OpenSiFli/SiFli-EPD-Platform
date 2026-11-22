#include "epd_activity_work.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    epd_activity_work_t *work = epd_activity_work_open(4096, 8192);
    epd_activity_guard_t guard;
    assert(work && epd_activity_work_enter(work, &guard));
    assert(epd_activity_work_reserve(work, EPD_APP_PSRAM, 16384));
    epd_activity_work_leave(&guard);
    assert(epd_activity_work_pause(work) && epd_activity_work_resume(work));
    epd_activity_work_close(work);
    puts("PASS activity disabled: no runtime/core symbols or allocation required"); return 0;
}
