#include "platform/epd_locale_internal.h"
#include "epd_locale.h"
#include "app_module.h"
#include "app_catalog.h"
#include "storage_file.h"
#include "preference_store.h"
#include "lv_ext_resource_manager.h"
#include <rtm.h>
#include <string.h>

#define LANGUAGE_PATH "/flash/system/language.bin"
#define LANGUAGE_MAGIC 0x4c414e47u
#define LANGUAGE_VERSION 1u

/* ISO language (2/3 letters), underscore, ISO region (2 letters), terminator. */
typedef struct { char code[7]; } language_record_t;
static uint32_t revision;
static bool initialized;

static bool supported(const char *locale)
{
    if (!locale) return false;
    for (const lv_i18n_lang_t *const *item = lv_i18n_lang_pack; *item; ++item)
        if (!strcmp((*item)->locale, locale)) return true;
    return false;
}

const char *epd_locale_get(void)
{
    const char *locale = lv_ext_get_locale();
    return locale ? locale : "zh_cn";
}
RTM_EXPORT(epd_locale_get);

uint32_t epd_locale_revision(void) { return revision; }
RTM_EXPORT(epd_locale_revision);

bool epd_locale_init(void)
{
    if (initialized) return true;
    if (resource_init() != LV_RES_OK) return false;
    language_record_t saved;
    const char *locale = "zh_cn";
    if (storage_record_load(LANGUAGE_PATH, LANGUAGE_MAGIC, LANGUAGE_VERSION,
                            &saved, sizeof(saved)) &&
        memchr(saved.code, 0, sizeof(saved.code)) && supported(saved.code))
        locale = saved.code;
    if (lv_ext_set_locale(NULL, locale) != LV_RES_OK)
    {
        resource_deinit();
        return false;
    }
    initialized = true;
    revision = 1;
    return true;
}

static bool set_locale(const char *locale, bool asynchronous)
{
    if (!initialized || !supported(locale)) return false;
    bool changed = strcmp(epd_locale_get(), locale) != 0;
    if (asynchronous && !changed && !preference_store_status(PREFERENCE_LANGUAGE).failed) return true;
    language_record_t saved = {{0}};
    size_t length = strlen(locale);
    if (length >= sizeof(saved.code)) return false;
    memcpy(saved.code, locale, length);
    bool ok;
    if (asynchronous) ok = preference_store_submit(PREFERENCE_LANGUAGE, LANGUAGE_PATH,
        LANGUAGE_MAGIC, LANGUAGE_VERSION, &saved, sizeof(saved));
    else {
        if (!preference_store_begin_sync(PREFERENCE_LANGUAGE)) return false;
        ok = storage_record_save(LANGUAGE_PATH, LANGUAGE_MAGIC, LANGUAGE_VERSION, &saved, sizeof(saved));
        preference_store_end_sync(PREFERENCE_LANGUAGE, ok ? PREFERENCE_SYNC_SAVED : PREFERENCE_SYNC_FAILED);
    }
    if (!ok) return false;
    if (!changed) return true;
    /* Both choices were validated against immutable, compiled language tables. */
    lv_ext_set_locale(NULL, saved.code);
    app_modules_set_locale(saved.code);
    ++revision;
    app_catalog_changed();
    return true;
}

bool epd_locale_set(const char *locale) { return set_locale(locale, false); }
bool epd_locale_request(const char *locale) { return set_locale(locale, true); }

RTM_EXPORT(lv_ext_str_get);
RTM_EXPORT(lv_ext_set_locale);
RTM_EXPORT(lv_ext_get_locale);
RTM_EXPORT(lv_ext_res_mng_builtin_init);
RTM_EXPORT(lv_ext_res_mng_builtin_destroy);
