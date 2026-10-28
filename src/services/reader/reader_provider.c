/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_provider.h"
#include <limits.h>
#include <string.h>

static const char *basename(const char *path)
{
    const char *last = path ? strrchr(path, '/') : NULL;
    return last ? last + 1 : path;
}
static bool api_valid(const epd_read_provider_t *api, const char *format, uint32_t version)
{
    return api && api->abi == EPD_READER_SOURCE_ABI && api->size >= sizeof(*api) &&
        api->format && !strcmp(api->format, format) && api->open && api->next && api->seek && api->close &&
        api->content_version && (!version || api->content_version == version);
}
int reader_providers_init(reader_providers_t *r, reader_provider_slot_t *slots,
                          const reader_provider_registration_t *registrations, size_t count,
                          const reader_provider_loader_t *loader)
{
    if (!r || !slots || !registrations || !count) return EPD_READ_INVALID;
    for (size_t i = 0; i < count; ++i)
    {
        const reader_provider_registration_t *p = &registrations[i];
        if (!p->format || !*p->format || (!!p->builtin == !!p->module_path)) return EPD_READ_INVALID;
        if (p->builtin && !api_valid(p->builtin, p->format, 0)) return EPD_READ_INVALID;
        if (p->module_path && (p->module_path[0] != '/' || !*basename(p->module_path) ||
            !loader || !loader->open || !loader->symbol || !loader->close)) return EPD_READ_INVALID;
        for (size_t j = 0; j < i; ++j)
        {
            const reader_provider_registration_t *q = &registrations[j];
            if (!strcmp(p->format, q->format) ||
                (p->module_path && q->module_path && !strcmp(basename(p->module_path), basename(q->module_path))))
                return EPD_READ_INVALID;
        }
    }
    memset(slots, 0, count * sizeof(*slots));
    *r = (reader_providers_t){.slots = slots, .count = count};
    if (loader) r->loader = *loader;
    for (size_t i = 0; i < count; ++i) slots[i].registration = &registrations[i];
    return EPD_READ_OK;
}
int reader_provider_acquire(reader_providers_t *r, const char *format,
                            uint32_t version, reader_provider_lease_t *lease)
{
    if (!r || !format || !lease || lease->slot || lease->api) return EPD_READ_INVALID;
    reader_provider_slot_t *p = NULL;
    for (size_t i = 0; i < r->count; ++i)
        if (!strcmp(r->slots[i].registration->format, format)) { p = &r->slots[i]; break; }
    if (!p) return EPD_READ_UNSUPPORTED;
    if (p->references == UINT_MAX) return EPD_READ_BUSY;
    if (!p->api)
    {
        p->api = p->registration->builtin;
        if (!p->api)
        {
            p->module = r->loader.open(p->registration->module_path);
            if (!p->module) return EPD_READ_UNSUPPORTED;
            p->api = r->loader.symbol(p->module, "epd_reader_provider");
        }
        if (!api_valid(p->api, format, version))
        {
            if (p->module) r->loader.close(p->module);
            p->module = NULL; p->api = NULL;
            return EPD_READ_UNSUPPORTED;
        }
    }
    if (!api_valid(p->api, format, version)) return EPD_READ_UNSUPPORTED;
    ++p->references; *lease = (reader_provider_lease_t){p, p->api};
    return EPD_READ_OK;
}
int reader_provider_retain(const reader_provider_lease_t *source, reader_provider_lease_t *lease)
{
    if (!source || !source->slot || !source->api || !source->slot->references ||
        source->slot->api != source->api || !lease || lease->slot || lease->api) return EPD_READ_INVALID;
    if (source->slot->references == UINT_MAX) return EPD_READ_BUSY;
    ++source->slot->references; *lease = *source;
    return EPD_READ_OK;
}
void reader_provider_release(reader_providers_t *r, reader_provider_lease_t *lease)
{
    if (!r || !lease || !lease->slot) return;
    reader_provider_slot_t *p = NULL;
    for (size_t i = 0; i < r->count; ++i) if (&r->slots[i] == lease->slot) { p = lease->slot; break; }
    if (!p || !p->references || p->api != lease->api) return;
    *lease = (reader_provider_lease_t){0};
    if (!--p->references)
    {
        if (p->module) r->loader.close(p->module);
        p->module = NULL; p->api = NULL;
    }
}
bool reader_providers_busy(const reader_providers_t *r)
{
    if (!r) return false;
    for (size_t i = 0; i < r->count; ++i) if (r->slots[i].references) return true;
    return false;
}
