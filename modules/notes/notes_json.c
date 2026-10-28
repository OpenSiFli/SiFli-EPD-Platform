#include "notes_json.h"
#include <stdlib.h>
#ifndef NOTES_HOST
#include <rtthread.h>
static void *json_default_alloc(size_t size) { return rt_malloc(size); }
static void *json_default_realloc(void *pointer, size_t size) { return rt_realloc(pointer, size); }
#define malloc json_default_alloc
#define free rt_free
#define realloc json_default_realloc
#endif
#include "../../SiFli-SDK/external/cJSON-1.7.17/cJSON.c"
