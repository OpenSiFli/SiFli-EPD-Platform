#include "music_service.h"
#include <mod_installer.h>

EPD_APP_DEFINE("music");
MODULE_INIT_DEF(music_service_init)
MODULE_CLEANUP_DEF(music_service_cleanup)

int app_main(intent_t intent)
{
    return music_view_open(intent);
}
