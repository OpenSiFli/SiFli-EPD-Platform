#ifndef UI_HOME_MODEL_H
#define UI_HOME_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define UI_HOME_DOCK_CAPACITY 5u
/* The persisted identity uses the app_fwk ID capacity, not a catalog index. */
#define UI_HOME_ID_CAPACITY 16u
#define UI_HOME_GRID_CAPACITY 16u

typedef struct
{
    uint32_t count;
    char ids[UI_HOME_DOCK_CAPACITY][UI_HOME_ID_CAPACITY];
} ui_home_model_t;

bool ui_home_model_valid(const ui_home_model_t *model);
bool ui_home_model_contains(const ui_home_model_t *model, const char *id);
bool ui_home_model_pin(ui_home_model_t *model, const char *id);
bool ui_home_model_unpin(ui_home_model_t *model, const char *id);
unsigned ui_home_page_count(unsigned count);
unsigned ui_home_page_clamp(unsigned page, unsigned count);
/* x is relative to the 660 px dock; icons are 100 px wide. */
int ui_home_dock_x(unsigned index, unsigned count);

#endif
