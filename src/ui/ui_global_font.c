#include "ui_global_font.h"
#include "reading_font.h"
#include "ui_font_resource.h"
#include <rtthread.h>

static bool install_on_boot;
static uint32_t boot_candidate;

void ui_global_font_boot(void)
{
    /* The installed Flash resource is the sole system font source this boot.
     * Catalog choices are candidates for the next DFU transaction only. */
    reading_font_init();
    reading_font_latch_boot(0);
    install_on_boot=true;
    boot_candidate=UINT32_MAX;
    const system_font_header_t *h=ui_font_resource();
    if (!system_font_header_valid(h)) {
        rt_kprintf("global_font invalid Flash resource\n");
        return;
    }
    rt_kprintf("global_font flash name=%s raw=%lu packed=%lu signature=%08lx\n",
        h->name,(unsigned long)h->font_bytes,(unsigned long)h->packed_bytes,(unsigned long)h->signature);
}

void ui_global_font_process(void)
{
    if (!install_on_boot) return;
    reading_font_status_t status;
    reading_font_get_status(&status);
    if (!status.ready || status.busy) return;
    if (boot_candidate == UINT32_MAX) boot_candidate=status.preferred;
    if (!status.preferred || status.preferred != boot_candidate) {
        install_on_boot=false;
        return;
    }
    /* The internal catalog can become ready before the TF mount and candidate
     * validation. Storage publication will call us again when that is ready. */
    reading_font_info_t info;
    if (!reading_font_get(boot_candidate,&info) || !info.available) return;
    install_on_boot=false;
    rt_err_t error=reading_font_install_pending();
    rt_kprintf("global_font pending install deferred: %d\n",error);
}
