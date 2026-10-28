#ifndef EPD_APP_INSTALLER_H
#define EPD_APP_INSTALLER_H
#include <stdbool.h>
#include <stdint.h>
#include "storage.h"

typedef enum { APP_JOB_IDLE, APP_JOB_RUNNING, APP_JOB_DONE, APP_JOB_FAILED } app_job_state_t;
typedef enum
{
    APP_JOB_MESSAGE_NONE,
    APP_JOB_MESSAGE_FAILED,
    APP_JOB_MESSAGE_BUSY,
    APP_JOB_MESSAGE_CARD_CHANGED,
    APP_JOB_MESSAGE_CLEANUP_FAILED,
    APP_JOB_MESSAGE_REMOVED,
    APP_JOB_MESSAGE_REMOVE_FAILED,
    APP_JOB_MESSAGE_INSERT_CARD,
    APP_JOB_MESSAGE_BAD_PACKAGE,
    APP_JOB_MESSAGE_INCOMPATIBLE,
    APP_JOB_MESSAGE_VERIFY_FAILED,
    APP_JOB_MESSAGE_KIND_CHANGED,
    APP_JOB_MESSAGE_ALREADY_INSTALLED,
    APP_JOB_MESSAGE_DATA_INCOMPATIBLE,
    APP_JOB_MESSAGE_READ_FAILED,
    APP_JOB_MESSAGE_CARD_FULL,
    APP_JOB_MESSAGE_FLASH_FULL,
    APP_JOB_MESSAGE_NO_MEMORY,
    APP_JOB_MESSAGE_MIGRATION_FAILED,
    APP_JOB_MESSAGE_UPDATED,
    APP_JOB_MESSAGE_INSTALLED,
    APP_JOB_MESSAGE_INSTALL_FAILED,
} app_job_message_t;
typedef enum
{
    APP_JOB_PHASE_PREPARING, APP_JOB_PHASE_CHECKING, APP_JOB_PHASE_VERIFYING,
    APP_JOB_PHASE_COPYING, APP_JOB_PHASE_PUBLISHING, APP_JOB_PHASE_MIGRATING,
    APP_JOB_PHASE_REMOVING, APP_JOB_PHASE_CLEANUP,
} app_job_phase_t;
typedef struct
{
    uint32_t ticket;
    app_job_state_t state;
    app_job_message_t message;
    app_job_phase_t phase;
    uint64_t completed_bytes, total_bytes;
} app_job_result_t;
const char *app_installer_phase_name(app_job_phase_t phase);
/* UI-thread translation of the worker's semantic result. */
const char *app_installer_message(app_job_message_t message);
bool app_installer_start_uninstall(const char *id, storage_volume_t volume, uint32_t *ticket);
/* UI thread; source is a .sap file on either mounted storage volume. */
bool app_installer_start_package(const char *path, storage_volume_t volume, uint32_t *ticket);
void app_installer_result(app_job_result_t *result);
bool app_installer_busy(void);
void app_installer_init(void);
void app_installer_process(void);
bool app_installer_blocks(const char *id);
bool app_installer_volume_ready(storage_volume_t volume);
bool app_installer_volume_preparing(storage_volume_t volume);
void app_installer_storage_changed(void);
#endif
