#ifndef EPD_READING_FONT_H
#define EPD_READING_FONT_H

#include "storage.h"

#define READING_FONT_LIMIT 24
#define READING_FONT_NAME_MAX 96
#define READING_FONT_FILE_MAX (32u * 1024u * 1024u)
#define READING_FONT_BUILTIN 0u

typedef struct
{
    uint32_t id, signature, size;
    storage_volume_t volume;
    bool available, selected, preferred;
    char name[READING_FONT_NAME_MAX];
} reading_font_info_t;

typedef enum
{
    READING_FONT_IDLE, READING_FONT_QUEUED, READING_FONT_RECEIVING,
    READING_FONT_COPYING, READING_FONT_VALIDATING, READING_FONT_COMMITTING,
    READING_FONT_DONE, READING_FONT_FAILED
} reading_font_phase_t;

typedef struct
{
    bool ready, busy;
    reading_font_phase_t phase;
    uint32_t ticket, completed, total, result_id, revision, selected, preferred;
    rt_err_t error;
} reading_font_status_t;

typedef struct
{
    uint32_t id, signature, size, session, revision, failure_revision;
    storage_volume_t volume;
    char path[STORAGE_PATH_MAX];
} reading_font_lease_t;

/* Initialize/process on the UI thread. Requests and copied snapshots are safe
 * from workers. Successful requests are accepted, not necessarily completed. */
bool reading_font_init(void);
void reading_font_process(void);
void reading_font_get_status(reading_font_status_t *status);
/* Current task or one of the eight most recently completed tasks. */
bool reading_font_job_status(uint32_t ticket, reading_font_status_t *status);
size_t reading_font_list(reading_font_info_t *items, size_t capacity);
bool reading_font_get(uint32_t id, reading_font_info_t *info);
uint32_t reading_font_selected(void);
uint32_t reading_font_revision(void);
/* UI thread, once at the start of a new reading session. Restores an available
 * preferred font; an in-flight startup/scan completes the restore on publication.
 * Later storage recovery alone does not switch an existing session's font. */
bool reading_font_begin(void);
rt_err_t reading_font_import(const char *source, storage_volume_t destination, uint32_t *ticket);
rt_err_t reading_font_select(uint32_t id, uint32_t *ticket);
/* Deletes installed records even when validation failed or the file is missing.
 * The volume must be mounted. Active streams are revoked after fallback is
 * published; remaining leases fail the job with EBUSY without deleting data. */
rt_err_t reading_font_delete(uint32_t id, uint32_t *ticket);

/* One upload at a time, staged on its destination volume. A matching ticket
 * owns every write/finish/abort. finish queues the common validator/installer. */
rt_err_t reading_font_upload_begin(const char *name, storage_volume_t destination,
                                  uint32_t length, uint32_t *ticket);
rt_err_t reading_font_upload_write(uint32_t ticket, const void *bytes, size_t size);
rt_err_t reading_font_upload_finish(uint32_t ticket);
void reading_font_upload_abort(uint32_t ticket);

/* Stream lifetime pins immutable content. Release exactly once after closing
 * the file, including revocation. No service lock is held by the caller. */
bool reading_font_acquire(uint32_t id, reading_font_lease_t *lease);
void reading_font_release(reading_font_lease_t *lease);
/* Memory-only version/session/writer check, including immediate card removal. */
bool reading_font_lease_valid(const reading_font_lease_t *lease);
/* May be called from a font stream on a draw worker. Invalidates availability;
 * the UI later revokes the stream and publishes the built-in fallback. */
void reading_font_report_failure(uint32_t id);
/* UI thread after drawing finishes. Closes TF font streams and cancels pending
 * work before storage handoff; false means cancellation is still finishing. */
bool reading_font_release_storage(void);
void reading_font_storage_changed(void);

#endif
