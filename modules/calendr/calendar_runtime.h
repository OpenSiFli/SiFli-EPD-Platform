#ifndef CALENDAR_RUNTIME_H
#define CALENDAR_RUNTIME_H
#include "calendar_model.h"
#include "app_service.h"
#define CAL_ROWS 48
enum { CAL_QUERY_ROWS=1u, CAL_QUERY_GRID=2u, CAL_QUERY_UPCOMING=4u, CAL_QUERY_SEARCH=8u };
typedef enum { CAL_REFRESH, CAL_SAVE, CAL_REMOVE, CAL_PREFERENCES, CAL_EXPORT, CAL_IMPORT, CAL_PREVIEW, CAL_CANCEL_IMPORT, CAL_DRAFT, CAL_CLEAR_DRAFT } cal_command_t;
typedef struct {
    cal_occurrence_t occurrence;
    char title[241],place[361];
    uint8_t kind;
    uint32_t duration;
} cal_row_t;
typedef struct {
    uint32_t revision,completed,query_revision,data_revision;
    bool ready,busy,preview,has_draft;
    size_t import_new,import_existing;
    char error[160],result[160];
    cal_row_t rows[CAL_ROWS],upcoming[3];
    size_t upcoming_count;
    size_t count;
    int32_t grid_first;uint64_t busy_days;
    cal_book_t preferences;
} cal_status_t;
void cal_runtime_init(void);
void cal_runtime_cleanup(void);
bool cal_runtime_start(void);
/* Transfer an unsaved page draft to the worker; draft is cleared on acceptance. */
void cal_runtime_ui_closed(cal_event_t *draft,int32_t day,bool instance);
bool cal_runtime_draft_saved(void);
void cal_runtime_status(cal_status_t *out);
bool cal_runtime_event(uint32_t id,cal_event_t *out);
uint32_t cal_runtime_query(int32_t first,int32_t last,int timezone,const char *query,int kind,size_t offset,unsigned fields);
bool cal_runtime_submit(cal_command_t command,const cal_event_t *event,uint32_t id,int32_t day,bool instance,const char *path,const cal_book_t *preferences);
bool cal_runtime_draft(cal_event_t *,int32_t *,bool *);
bool cal_ui_flush(void);
bool cal_ui_present(void);
void cal_ui_retire(void);
extern const epd_background_t epd_app_background;
#endif
