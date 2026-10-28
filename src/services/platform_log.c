#include <rtthread.h>
#include <ulog.h>
#include <string.h>
#include <stdbool.h>
#if defined(AIC_WLAN_AIC8800D40N) && defined(AIC8800_USE_RT_WLAN)
#include <wlan_log.h>
#endif
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#ifdef ULOG_USING_FILTER
static const char *const routine_tags[] = {
    "audio", "drv.audprc", "drv.audcodec", "drv.lcd", "APP.FWK", "APP.FWK.INT", "APP.SCHE"
};
static void log_level(bool verbose)
{
#if defined(AIC_WLAN_AIC8800D40N) && defined(AIC8800_USE_RT_WLAN)
    wlan_log_set_verbose(verbose);
#endif
    ulog_global_filter_lvl_set(verbose ? LOG_LVL_DBG : LOG_LVL_INFO);
    for (unsigned i = 0; i < sizeof(routine_tags) / sizeof(routine_tags[0]); ++i)
        ulog_tag_lvl_filter_set(routine_tags[i], verbose ? LOG_LVL_DBG : LOG_LVL_WARNING);
}
static int platform_log_init(void)
{
    log_level(false);
    return RT_EOK;
}
INIT_ENV_EXPORT(platform_log_init);
#ifdef RT_USING_FINSH
static int epd_log(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "normal") && strcmp(argv[1], "verbose")))
    {
        rt_kprintf("epd_log normal|verbose\n");
        return -RT_EINVAL;
    }
    log_level(!strcmp(argv[1], "verbose"));
    return RT_EOK;
}
MSH_CMD_EXPORT(epd_log, Set platform log verbosity);
#endif
#endif
