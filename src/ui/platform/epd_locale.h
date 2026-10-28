#ifndef EPD_LOCALE_H
#define EPD_LOCALE_H

#include <stdbool.h>
#include <stdint.h>

/* UI-thread language selection. Returned codes have firmware lifetime. */
const char *epd_locale_get(void);
uint32_t epd_locale_revision(void);

#endif
