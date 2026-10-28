/* SPDX-License-Identifier: Apache-2.0 */
#include "ai_settings_draft.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define DRAFT_MAGIC 0x41494452u
#define DRAFT_VERSION 1u
static bool ended(const char *text, size_t size) { return memchr(text, 0, size) != NULL; }
bool ai_settings_draft_valid(const ai_settings_draft_t *d)
{
    if (!d || d->present > 1 || d->keep_secret > 1 || d->has_secret > 1 || d->dirty > 1 ||
        d->input_numeric > 1 || d->model >= EPD_AI_MAX_MODELS || d->header >= AI_EXTRA_HEADERS ||
        d->profile.public.model_count > EPD_AI_MAX_MODELS || d->profile.header_count > AI_EXTRA_HEADERS ||
        d->input_capacity > AI_DRAFT_INPUT_SIZE ||
        !ended(d->selected_id, sizeof(d->selected_id)) || !ended(d->input, sizeof(d->input)) ||
        !ended(d->profile.public.id, sizeof(d->profile.public.id)) ||
        !ended(d->profile.public.name, sizeof(d->profile.public.name)) ||
        !ended(d->profile.endpoint, sizeof(d->profile.endpoint)) ||
        !ended(d->profile.secret, sizeof(d->profile.secret))) return false;
    for (size_t i = 0; i < EPD_AI_MAX_MODELS; ++i)
        if (!ended(d->profile.public.models[i].id, EPD_AI_MODEL_SIZE)) return false;
    for (size_t i = 0; i < AI_EXTRA_HEADERS; ++i)
        if (!ended(d->profile.headers[i].name, sizeof(d->profile.headers[i].name)) ||
            !ended(d->profile.headers[i].value, sizeof(d->profile.headers[i].value))) return false;
    return true;
}

epd_ai_error_t ai_settings_draft_load(ai_settings_draft_t *draft)
{
    memset(draft, 0, sizeof(*draft));
    storage_lock();
    epd_ai_error_t error = EPD_AI_STORAGE;
    if (storage_path_available(AI_DRAFT_PATH) && storage_file_recover(AI_DRAFT_PATH))
    {
        int fd = open(AI_DRAFT_PATH, O_RDONLY, 0);
        if (fd < 0 && (errno == ENOENT || errno == -ENOENT)) error = EPD_AI_OK;
        else if (fd >= 0 && close(fd) == 0 &&
                 storage_record_load(AI_DRAFT_PATH, DRAFT_MAGIC, DRAFT_VERSION, draft, sizeof(*draft)))
            error = ai_settings_draft_valid(draft) ? EPD_AI_OK : EPD_AI_INVALID;
    }
    storage_unlock();
    if (error != EPD_AI_OK) memset(draft, 0, sizeof(*draft));
    return error;
}

epd_ai_error_t ai_settings_draft_store(const ai_settings_draft_t *draft)
{
    if (!ai_settings_draft_valid(draft)) return EPD_AI_INVALID;
    return storage_record_save(AI_DRAFT_PATH, DRAFT_MAGIC, DRAFT_VERSION, draft, sizeof(*draft)) ?
           EPD_AI_OK : EPD_AI_STORAGE;
}
