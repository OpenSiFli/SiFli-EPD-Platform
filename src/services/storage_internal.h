#ifndef EPD_STORAGE_INTERNAL_H
#define EPD_STORAGE_INTERNAL_H

#include "storage.h"

typedef enum
{
    STORAGE_REPLACE_UNAVAILABLE,
    STORAGE_REPLACE_ATOMIC,
    STORAGE_REPLACE_BACKUP
} storage_replace_mode_t;

void storage_init(void);

/* Determined by the mounted filesystem, not by the physical medium. */
storage_replace_mode_t storage_replace_mode(storage_volume_t volume);

bool storage_apps_enabled(storage_volume_t volume);
bool storage_card_enable_apps(uint32_t session);

/* The UI owner closes its persistent files/fonts before acknowledging removal. */
bool storage_release_requested(void);
bool storage_release_for_export(void);
uint32_t storage_release_request_id(void);
rt_err_t storage_release_complete(uint32_t release_id);

bool storage_release_timed_out(void);

void storage_format_size(uint64_t bytes, char *text, size_t size);

#endif
