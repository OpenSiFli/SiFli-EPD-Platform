#ifndef EPD_BOARD_SDCARD_H
#define EPD_BOARD_SDCARD_H

#include <rtthread.h>

/* Restore the board's card-detect mux after LCD initialization. */
void board_sdcard_prepare(void);
void board_sdcard_set_power(rt_bool_t enabled);

#endif
