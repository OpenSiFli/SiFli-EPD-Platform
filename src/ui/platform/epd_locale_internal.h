#ifndef EPD_LOCALE_INTERNAL_H
#define EPD_LOCALE_INTERNAL_H

#include "epd_locale.h"

bool epd_locale_init(void);
bool epd_locale_set(const char *locale);
/* UI-thread queue acceptance; failures remain reported by preference_store. */
bool epd_locale_request(const char *locale);

#endif
