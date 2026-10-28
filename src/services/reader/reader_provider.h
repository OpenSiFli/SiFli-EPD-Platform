/* SPDX-License-Identifier: Apache-2.0 */
#ifndef READER_PROVIDER_H
#define READER_PROVIDER_H
#include "../../ui/platform/epd_reader_source.h"

typedef struct {
    const char *format;
    const epd_read_provider_t *builtin;
    const char *module_path;
} reader_provider_registration_t;
typedef struct {
    void *(*open)(const char *path);
    const void *(*symbol)(void *module, const char *name);
    void (*close)(void *module);
} reader_provider_loader_t;
typedef struct {
    const reader_provider_registration_t *registration;
    const epd_read_provider_t *api;
    void *module;
    unsigned references;
} reader_provider_slot_t;
typedef struct {
    reader_provider_slot_t *slots;
    size_t count;
    reader_provider_loader_t loader;
} reader_providers_t;
typedef struct {
    reader_provider_slot_t *slot;
    const epd_read_provider_t *api;
} reader_provider_lease_t;

/* UI-thread only. Registration/slots/loader storage lives with the platform.
 * Leases are linear ownership objects: initialize to zero, never copy, release
 * exactly once. A parser and each outstanding resource stream retain a lease.
 * Module lookup is by unique registered path, never the requesting app ID. */
int reader_providers_init(reader_providers_t *registry, reader_provider_slot_t *slots,
                          const reader_provider_registration_t *registrations, size_t count,
                          const reader_provider_loader_t *loader);
int reader_provider_acquire(reader_providers_t *registry, const char *format,
                            uint32_t content_version, reader_provider_lease_t *lease);
int reader_provider_retain(const reader_provider_lease_t *source, reader_provider_lease_t *lease);
void reader_provider_release(reader_providers_t *registry, reader_provider_lease_t *lease);
bool reader_providers_busy(const reader_providers_t *registry);
#endif
