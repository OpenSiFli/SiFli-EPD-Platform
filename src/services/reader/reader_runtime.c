/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_runtime.h"
#include "platform/epd_reader_session.h"
#include "platform/app_module.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "gui_app_fwk.h"
#include <stdio.h>
#include <string.h>

bool reader_runtime_accept(void)
{ return !system_reset_blocks_apps() && !storage_changing(); }
int reader_runtime_pin(const char *id, void **module)
{
    if (!id || !*id || !module || strlen(id) >= RT_NAME_MAX) return EPD_READ_INVALID;
    *module = NULL;
    if (app_module_loaded(id))
    {
        char path[STORAGE_PATH_MAX], name[RT_NAME_MAX + 4];
        snprintf(name, sizeof(name), "%s.so", id);
        if (!storage_app_path(path, sizeof(path), id, STORAGE_APP_CODE, name)) return EPD_READ_IO;
        *module = app_module_open(path, id);
        return *module ? EPD_READ_OK : EPD_READ_NO_MEMORY;
    }
    const builtin_app_desc_t *app = gui_builtin_app_list_open();
    bool found = false;
    while (app)
    {
        if (!strcmp(app->id, id)) { found = true; break; }
        app = gui_builtin_app_list_get_next(app);
    }
    gui_builtin_app_list_close(app);
    return found ? EPD_READ_OK : EPD_READ_STALE;
}
void reader_runtime_drop(void *module) { if (module) app_module_close(module); }
