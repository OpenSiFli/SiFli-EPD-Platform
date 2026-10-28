#ifndef EPD_POWER_H
#define EPD_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include <rtthread.h>

/* Initialize the resident event before starting workers. UI owns process(). */
rt_err_t epd_power_init(void);
void epd_power_process(void);
/* Wake the UI for input or maintenance; callable from interrupt context. */
void epd_power_notify(void);
/* Physical key-line wake, independent of the ADC key identity. True consumes
 * this entire press during wallpaper handoff/sleep; false preserves normal input. */
bool epd_power_key_wakeup(void);
/* UI thread: accepted user input restarts the locked sleep countdown. */
void epd_power_activity(void);
/* Maintenance mode retains the wallpaper, original pages and module contexts. */
bool epd_power_sleeping(void);
bool epd_power_transitioning(void);
/* UI thread: present the selected wallpaper for shutdown. Repeated calls return
 * -RT_EBUSY until its full refresh and panel power sequence have completed.
 * RT_EOK keeps the wallpaper on screen and suppresses further frames. Any
 * other error requires cancel before normal UI operation can resume. */
rt_err_t epd_power_shutdown_prepare(void);
/* UI thread: retry until pending peripheral resume requests are accepted. */
bool epd_power_shutdown_cancel(void);
/* UI thread: combine the next power deadline with the caller's wait, then wait
 * for an event. UINT32_MAX means no deadline, not a periodic poll. */
uint32_t epd_power_next_delay(uint32_t default_ms);
void epd_power_wait(uint32_t delay_ms);

#endif
