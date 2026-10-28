#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H

#include "ui_model.h"
#include "gui_app_fwk.h"

/* UI-thread-only adapter. app_fwk owns the page stack, screens and page memory. */
typedef void (*ui_page_lifecycle_t)(ui_page_id_t page, gui_app_msg_type_t message, void *memory);

bool ui_navigation_init(ui_page_lifecycle_t lifecycle, uint32_t page_memory_size);
int ui_navigation_start(const char *app, ui_page_id_t root);
bool ui_navigation_run(const char *app);
/* Navigation succeeds only when the requested foreground state is reached. */
bool ui_navigation_open(ui_page_id_t page);
bool ui_navigation_back(void);

#endif
