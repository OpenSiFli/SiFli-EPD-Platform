#include "ui_weather_data.h"
#include "weather.h"
#include "platform/epd_i18n.h"
#include <stdio.h>
#include <string.h>

static ui_weather_view_t view;
static bool initialized;

static void value_text(char *buffer, size_t size, int value, const char *unit)
{
    if (value == WEATHER_MISSING) snprintf(buffer, size, "--");
    else snprintf(buffer, size, "%d%s", value, unit);
}

bool ui_weather_process(void)
{
    weather_info_t data;
    weather_get_info(&data);
    uint32_t locale_revision = epd_locale_revision();
    if (initialized && data.revision == view.revision && locale_revision == view.locale_revision) return false;
    memset(&view, 0, sizeof(view));
    initialized = true;
    view.revision = data.revision;
    view.locale_revision = locale_revision;
    view.code = data.parts[WEATHER_NOW].valid ? data.code : 999;
    snprintf(view.city, sizeof(view.city), "%s", data.city[0] ? data.city : epd_get_str(weather_title));
    snprintf(view.temperature, sizeof(view.temperature), "--℃");
    snprintf(view.description, sizeof(view.description), "%s", epd_get_str(weather_no_data));
    snprintf(view.range_wind, sizeof(view.range_wind), "%s", epd_get_str(weather_range_missing));
    EPD_FMT_weather_updated_at(view.update_time, sizeof(view.update_time), "--");
    snprintf(view.summary, sizeof(view.summary), "%s", epd_get_str(weather_no_data));
    snprintf(view.source, sizeof(view.source), "%s", epd_get_str(weather_source));
    for (unsigned i = 0; i < UI_WEATHER_METRIC_COUNT; ++i)
        snprintf(view.metrics[i], sizeof(view.metrics[i]), "--");
    for (unsigned i = 0; i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        ui_weather_forecast_t *f = &view.forecast[i];
        f->code = 999;
        snprintf(f->date, sizeof(f->date), "--");
        snprintf(f->text, sizeof(f->text), "--");
        snprintf(f->temperature_range, sizeof(f->temperature_range), "--");
        snprintf(f->wind, sizeof(f->wind), "--");
    }
    if (!data.valid) return true;
    char feels[24], high[24], low[24];
    value_text(view.temperature, sizeof(view.temperature), data.temperature, "℃");
    value_text(feels, sizeof(feels), data.feels_like, "℃");
    value_text(high, sizeof(high), data.high, "℃");
    value_text(low, sizeof(low), data.low, "℃");
    if (data.parts[WEATHER_NOW].valid)
        EPD_FMT_weather_description(view.description, sizeof(view.description), data.text, feels);
    EPD_FMT_weather_range_wind(view.range_wind, sizeof(view.range_wind), high, low,
             data.wind_dir[0] ? data.wind_dir : epd_get_str(weather_wind), data.wind_scale_text[0] ? data.wind_scale_text : "--");
    char clocks[WEATHER_PART_COUNT][6] = {"--", "--", "--"};
    for (unsigned i = 0; i < WEATHER_PART_COUNT; ++i)
    {
        const char *time = strchr(data.parts[i].update_time, 'T');
        if (data.parts[i].valid && time && strlen(time + 1) >= 5)
        {
            memcpy(clocks[i], time + 1, 5);
            clocks[i][5] = '\0';
        }
    }
    EPD_FMT_weather_updated_parts(view.update_time, sizeof(view.update_time), clocks[0], clocks[1], clocks[2]);
    if (data.parts[WEATHER_NOW].valid)
        EPD_FMT_weather_summary(view.summary, sizeof(view.summary), data.city, data.text, data.temperature);
    EPD_FMT_weather_source_region(view.source, sizeof(view.source), data.region);
    value_text(view.metrics[0], sizeof(view.metrics[0]), data.humidity, "%");
    value_text(view.metrics[1], sizeof(view.metrics[1]), data.wind_speed, " km/h");
    value_text(view.metrics[2], sizeof(view.metrics[2]), data.visibility, " km");
    value_text(view.metrics[3], sizeof(view.metrics[3]), data.cloud, "%");
    snprintf(view.metrics[4], sizeof(view.metrics[4]), "%s", data.sunrise[0] ? data.sunrise : "--");
    snprintf(view.metrics[5], sizeof(view.metrics[5]), "%s", data.sunset[0] ? data.sunset : "--");
    value_text(view.metrics[6], sizeof(view.metrics[6]), data.pressure, " hPa");
    if (data.aqi >= 0)
        snprintf(view.metrics[7], sizeof(view.metrics[7]), "%d %s", data.aqi, data.aqi_category);
    for (unsigned i = 0; i < (unsigned)data.forecast_count && i < UI_WEATHER_FORECAST_COUNT; ++i)
    {
        const weather_forecast_t *src = &data.forecast[i];
        ui_weather_forecast_t *dst = &view.forecast[i];
        dst->code = src->code;
        snprintf(dst->date, sizeof(dst->date), "%s", strlen(src->date) == 10 ? src->date + 5 : src->date);
        snprintf(dst->text, sizeof(dst->text), "%s", src->text);
        snprintf(dst->temperature_range, sizeof(dst->temperature_range), "%d℃~%d℃", src->low, src->high);
        EPD_FMT_weather_forecast_wind(dst->wind, sizeof(dst->wind), src->wind_dir,
                 src->wind_scale_text[0] ? src->wind_scale_text : "--");
    }
    return true;
}

const ui_weather_view_t *ui_weather_view(void)
{
    return &view;
}

void ui_weather_result_text(const weather_result_t *result, char *buffer, size_t capacity)
{
    const char *text;
    switch (result->code)
    {
    case WEATHER_RESULT_WORKING: text = epd_get_str(weather_working); break;
    case WEATHER_RESULT_SYNC_OK: text = epd_get_str(weather_sync_ok); break;
    case WEATHER_RESULT_SYNC_PARTIAL: text = epd_get_str(weather_sync_partial); break;
    case WEATHER_RESULT_SYNC_FAILED: text = epd_get_str(weather_sync_failed); break;
    case WEATHER_RESULT_IMPORT_OK: text = epd_get_str(weather_import_ok); break;
    case WEATHER_RESULT_CONFIG_SAVE_FAILED: text = epd_get_str(weather_config_save_failed); break;
    case WEATHER_RESULT_HISTORY_NOT_FOUND: text = epd_get_str(weather_history_not_found); break;
    case WEATHER_RESULT_CONFIG_REQUIRED: text = epd_get_str(weather_config_required); break;
    case WEATHER_RESULT_NETWORK_UNAVAILABLE: text = epd_get_str(weather_network_unavailable); break;
    case WEATHER_RESULT_CITY_LOOKUP_FAILED: text = epd_get_str(weather_city_lookup_failed); break;
    case WEATHER_RESULT_HISTORY_SAVE_FAILED: text = epd_get_str(weather_history_save_failed); break;
    case WEATHER_RESULT_CITY_OK:
        EPD_FMT_weather_city_ok(buffer, capacity, result->city);
        return;
    case WEATHER_RESULT_CITY_SAVE_FAILED: text = epd_get_str(weather_city_save_failed); break;
    case WEATHER_RESULT_CANCELLED: text = epd_get_str(weather_cancelled); break;
    case WEATHER_RESULT_SD_REQUIRED: text = epd_get_str(weather_sd_required); break;
    case WEATHER_RESULT_CONFIG_NOT_FOUND: text = epd_get_str(weather_config_not_found); break;
    case WEATHER_RESULT_CONFIG_INVALID: text = epd_get_str(weather_config_invalid); break;
    case WEATHER_RESULT_PAGE_OPEN_FAILED: text = epd_get_str(weather_page_open_failed); break;
    case WEATHER_RESULT_BACK_FAILED: text = epd_get_str(weather_back_failed); break;
    case WEATHER_RESULT_CITY_ID_REQUIRED: text = epd_get_str(weather_city_id_required); break;
    default: text = epd_get_str(weather_operation_failed); break;
    }
    rt_snprintf(buffer, capacity, "%s", text);
}
