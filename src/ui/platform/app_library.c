/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_file.h"
#include "platform/epd_app_library.h"
#include "epd_app.h"
#include "app_package.h"
#include "app_installer.h"
#include "app_module.h"
#include "ui_reader_font_cache.h"
#include "storage.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rtm.h>
#include <stdio.h>

void *epd_app_library_open(const char *app_id, const char *name)
{
    if (!app_id || !name || !app_module_loaded(app_id) || app_installer_blocks(app_id)) return NULL;
    char root[96], path[128];
    if (!storage_app_path(root, sizeof(root), app_id, STORAGE_APP_CODE, "")) return NULL;
    size_t peak = 0;
    app_package_status_t status = app_package_verify_library(root, app_id, name, &peak);
    if (status != APP_PACKAGE_OK)
    {
        rt_kprintf("app %s library %s: %s\n", app_id, name, app_package_error(status));
        return NULL;
    }
    if (!dlmodule_find(name)) ui_reader_font_cache_reserve(peak);
    int n = snprintf(path, sizeof(path), "%s/codecs/%s.so", root, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NULL;
    return app_module_open(path, app_id);
}
RTM_EXPORT(epd_app_library_open);

void *epd_app_library_symbol(void *library, const char *symbol)
{
    return library && symbol ? dlsym(library, symbol) : NULL;
}
RTM_EXPORT(epd_app_library_symbol);

void epd_app_library_close(void *library)
{
    if (library) app_module_close(library);
}
RTM_EXPORT(epd_app_library_close);
