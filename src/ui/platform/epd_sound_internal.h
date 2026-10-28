#ifndef EPD_SOUND_INTERNAL_H
#define EPD_SOUND_INTERNAL_H

#include "epd_sound.h"

typedef enum
{
    EPD_SOUND_RELOAD_LOADED, EPD_SOUND_RELOAD_DEFAULTED, EPD_SOUND_RELOAD_IO_ERROR
} epd_sound_reload_result_t;


/* Resident lifecycle; these functions are not exported to dynamic modules. */
bool epd_sound_init(void);
epd_sound_reload_result_t epd_sound_reload(void);
/* Resident UI polling retries failed initialization/output, without auto-saving. */
void epd_sound_process(void);

#endif
