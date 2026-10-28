#ifndef CALENDAR_EXCHANGE_H
#define CALENDAR_EXCHANGE_H
#include "calendar_model.h"
bool cal_import_file(cal_book_t *book,const char *path);
bool cal_export_file(const cal_book_t *book,const char *path);
bool cal_merge(cal_book_t *book,const cal_book_t *imported,bool replace_existing);
bool cal_ics_supported(const cal_book_t *book);
#endif
