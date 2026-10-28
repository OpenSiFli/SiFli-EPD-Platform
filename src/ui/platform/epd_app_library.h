#ifndef EPD_APP_LIBRARY_H
#define EPD_APP_LIBRARY_H

/* UI-thread operations. Libraries belong to an already-loaded application and
 * must be released after its worker and all library-owned results are stopped. */
void *epd_app_library_open(const char *app_id, const char *name);
void *epd_app_library_symbol(void *library, const char *symbol);
void epd_app_library_close(void *library);

#endif
