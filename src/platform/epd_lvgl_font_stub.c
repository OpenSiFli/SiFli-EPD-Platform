#include <lvgl.h>

#if LV_USE_TINY_TTF
/* The EPD UI uses MiSans and built-in Montserrat fonts; omit the SDK's
 * unused 191 KB embedded Ubuntu TTF and its boot-time font instances. */
void __wrap_lv_font_tiny_init(void)
{
}
#endif
