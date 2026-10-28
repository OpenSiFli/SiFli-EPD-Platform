#include "weather.h"
#include <string.h>

static const char *weather_state_name(weather_state_t st)
{
    switch (st)
    {
    case WEATHER_STATE_IDLE:       return "空闲";
    case WEATHER_STATE_REFRESHING: return "更新中";
    case WEATHER_STATE_UPDATED:    return "已更新";
    case WEATHER_STATE_CACHED:     return "缓存数据";
    case WEATHER_STATE_FAILED:     return "失败";
    default:                       return "?";
    }
}

static void print_weather_snapshot(void)
{
    weather_info_t w;
    int i;

    weather_get_info(&w);

    rt_kprintf("========== 天气快照 ==========\n");
    rt_kprintf("有效    : %s\n", w.valid ? "是" : "否");
    rt_kprintf("状态    : %s\n", weather_state_name(w.state));
    rt_kprintf("状态描述: %s\n", w.status);
    rt_kprintf("城市    : %s\n", w.city);
    for (i = 0; i < WEATHER_PART_COUNT; ++i)
        rt_kprintf("part %d: valid=%d error=%d updated=%s\n", i, w.parts[i].valid,
                   (int)w.parts[i].error, w.parts[i].update_time);
    rt_kprintf("实况    : %s (%d) %d℃  体感 %d℃  今日 %d~%d℃\n",
               w.text, w.code, w.temperature, w.feels_like, w.low, w.high);
    rt_kprintf("详情    : 湿度 %d%% | %s %d级 %dkm/h | 能见度 %dkm | 气压 %dhPa | 云量 %d%%\n",
               w.humidity, w.wind_dir, w.wind_scale, w.wind_speed,
               w.visibility, w.pressure, w.cloud);
    if (w.aqi >= 0)
        rt_kprintf("空气    : 空气质量 %d %s | 日出 %s | 日落 %s\n",
                   w.aqi, w.aqi_category, w.sunrise, w.sunset);
    else
        rt_kprintf("空气    : 空气质量 -- | 日出 %s | 日落 %s\n",
                   w.sunrise, w.sunset);

    for (i = 0; i < w.forecast_count; i++)
    {
        const weather_forecast_t *f = &w.forecast[i];
        rt_kprintf("预报%d   : %s %s (%d) %d~%d℃  %s %d级\n",
                   i, f->date, f->text, f->code, f->low, f->high, f->wind_dir, f->wind_scale);
    }
    rt_kprintf("======================================\n");
}

static void cmd_weather_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_weather_snapshot();
    weather_result_t job;
    weather_get_result(&job);
    rt_kprintf("job %u: busy=%d result=%d city=%s detail=%s\n", (unsigned)job.ticket,
               job.busy, (int)job.code, job.city, job.detail);
}

static void cmd_weather_refresh(int argc, char **argv)
{
    rt_err_t ret;

    (void)argc;
    (void)argv;

    ret = weather_request_refresh();
    if (ret != RT_EOK)
    {
        rt_kprintf("request failed (%d), service not ready?\n", ret);
        return;
    }

    rt_kprintf("refresh queued; use 'svc weather status' to check the result\n");
}

static void cmd_weather_city(int argc, char **argv)
{
    if (argc < 2)
    {
        weather_config_t cfg;
        weather_get_config(&cfg);
        rt_kprintf("current city: %s (%s)\n", cfg.city, cfg.city_id);
        rt_kprintf("usage: svc weather city <numeric LocationID>\n");
        return;
    }

    if (weather_set_city(argv[1]) != RT_EOK)
        rt_kprintf("city request rejected (invalid ID or service busy)\n");
    else
        rt_kprintf("city validation queued; setting changes only after successful lookup and save\n");
}

int weather_command(int argc, char **argv)
{
    if (argc < 1) rt_kprintf("svc weather status|refresh|city [id]\n");
    else if (!strcmp(argv[0], "status")) cmd_weather_status(argc, argv);
    else if (!strcmp(argv[0], "refresh")) cmd_weather_refresh(argc, argv);
    else if (!strcmp(argv[0], "city")) cmd_weather_city(argc, argv);
    else return -RT_EINVAL;
    return RT_EOK;
}
