#ifndef EPD_I18N_H
#define EPD_I18N_H

#include "epd_locale.h"
#include "mod_installer.h"
#include "lv_ext_resource_manager.h"

/* Each firmware/application compiles against its own generated language table. */
#define epd_get_str(key) LV_EXT_STR_GET(LV_EXT_STR_ID(key))

#include "epd_strings.h"

#endif
