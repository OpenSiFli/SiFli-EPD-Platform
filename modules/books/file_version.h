/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_FILE_VERSION_H
#define BOOK_FILE_VERSION_H
#include "storage_file.h"
#include <string.h>

/* Runtime tokens only; storage is the volume's mutation revision. A stable
 * volume proves validity even after the bounded path registry evicts a token. */
typedef struct { uint32_t path, session, storage; } book_file_version_t;

static bool book_file_version_valid(const char *path, const book_file_version_t *v)
{
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    if (!v->path || !storage_path_available(path) || !storage_session_valid(volume, v->session) ||
        (volume == STORAGE_SD && storage_changing())) return false;
    uint32_t revision = storage_volume_revision(volume);
    if (v->storage == revision) return true;
    return !storage_file_writers(path) && v->path == storage_path_revision(path) &&
        revision == storage_volume_revision(volume);
}

static book_file_version_t book_file_version(const char *path)
{
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    book_file_version_t v = {0, volume == STORAGE_SD ? storage_card_session() : 0,
                             storage_volume_revision(volume)};
    v.path = storage_path_revision(path);
    if (storage_file_writers(path)) v.path = 0;
    return v;
}
#endif
