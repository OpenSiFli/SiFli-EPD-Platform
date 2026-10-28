#ifndef APP_CATALOG_H
#define APP_CATALOG_H

#include "storage_file_internal.h"
#include "gui_app_fwk.h"
#include "storage_file.h"

#define EPD_APP_DIRECTORY STORAGE_FLASH_APPS

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    char name[48];
    const void *icon;
    char icon_file[160];
    bool has_settings;
    bool resources_only;
    bool background;
    bool handles_content;
    bool lockscreen;
    uint32_t version, data_version;
    char build_id[65];
    storage_volume_t volume;
} epd_app_entry_t;

void app_catalog_init(void);
void app_catalog_process(void);
bool app_catalog_refresh(void);
/* Worker publishes a generation only; the UI thread reads metadata and images. */
void app_catalog_changed(void);
uint32_t app_catalog_revision(void);
/* UI thread: stop loaded modules so their ONSTOP handlers close removable files. */
bool app_catalog_release_storage(void);
unsigned app_catalog_count(void);
const epd_app_entry_t *app_catalog_get(unsigned index);
/* UI thread: resolve the selected installed package, including card availability. */
bool app_catalog_lockscreen_available(const char *id);
bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry);
/* UI-thread snapshot with localized package names. */
bool app_package_read_display(const char *directory, const char *id, epd_app_entry_t *entry);

#endif
