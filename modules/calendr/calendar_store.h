#ifndef CALENDAR_STORE_H
#define CALENDAR_STORE_H
#include "calendar_model.h"
bool cal_document_encode(const cal_book_t *,const cal_event_t *,int32_t,bool,void **,size_t *);
bool cal_document_decode(cal_book_t *,cal_event_t *,int32_t *,bool *,const void *,size_t);
#endif
