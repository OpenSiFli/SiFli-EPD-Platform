#include "config_io.h"
#include "storage_file.h"
#include "storage.h"
#include "platform/epd_app.h"
#include <string.h>

#define CONFIG_MAGIC 0x43464731u
#define CONFIG_VERSION 1u

struct config_io {
    epd_app_task_group_t group;
    epd_memory_owner_t task;
    epd_operation_scope_t business, cleanup;
    epd_operation_t operation;
    lv_timer_t *completion;
    struct config_io *next;
    bool save, cancelled;
    uint32_t presses;
    config_io_done_t done;
    void *context;
};
/* UI lane only. A new page reads after the previous accepted write settles. */
static config_io_t *writing, *waiting_reads;

static void wake(config_io_t *io)
{
    lv_timer_reset(io->completion);
    lv_timer_resume(io->completion);
}

static void retired(void *context) { wake(context); }

static void execute(epd_operation_t operation, void *context)
{
    config_io_t *io = context;
    epd_operation_snapshot_t state;
    if (!epd_operation_get(operation, &state)) return;
    if (state.cancel_requested) {
        epd_operation_complete(operation, EPD_OPERATION_CANCELLED, 0, NULL); return;
    }
    char path[STORAGE_PATH_MAX];
    uint32_t value = io->presses;
    bool ok = !storage_changing() &&
              storage_app_path(path, sizeof(path), "cfgdemo", STORAGE_APP_DATA, "settings.bin");
    if (ok) ok = io->save ? storage_record_save(path, CONFIG_MAGIC, CONFIG_VERSION, &value, sizeof(value)) :
                           storage_record_load(path, CONFIG_MAGIC, CONFIG_VERSION, &value, sizeof(value));
    if (ok) io->presses = value;
    epd_operation_complete(operation, ok ? EPD_OPERATION_OK : EPD_OPERATION_FAILED, 0, NULL);
}

static bool received(epd_operation_t operation, const epd_operation_snapshot_t *state,
                     void *result, void *context)
{
    (void)operation; (void)state; (void)result; (void)context;
    return true;
}

static void release(config_io_t *io)
{
    if (io->operation) epd_operation_release(io->operation);
    if (io->business) {
        epd_operation_scope_close(io->business); epd_operation_scope_release(io->business);
    }
    if (io->cleanup) {
        epd_operation_scope_close(io->cleanup); epd_operation_scope_release(io->cleanup);
    }
    if (io->task != EPD_MEMORY_OWNER_INVALID) epd_app_task_end(io->task);
    if (io->group) epd_app_task_group_release(io->group);
}

static void process(lv_timer_t *timer)
{
    config_io_t *io = lv_timer_get_user_data(timer);
    lv_timer_pause(timer);
    bool ok = false;
    if (!io->operation && !io->cancelled) {
        if (!io->save && writing) {
            io->next = waiting_reads;
            waiting_reads = io;
            return;
        }
        epd_operation_spec_t spec = {0};
        if (epd_operation_runtime_executors(&spec.execute, &spec.deliver, &spec.cleanup)) {
            spec.cleanup = spec.deliver;
            spec.deliver = spec.execute;
            spec.scope = io->business; spec.cleanup_scope = io->cleanup;
            spec.run = execute; spec.receive = received; spec.dispose_user = retired; spec.user = io;
            if (epd_operation_submit(&spec, &io->operation)) return;
        }
    }
    if (io->operation) {
        epd_operation_snapshot_t state;
        if (!epd_operation_get(io->operation, &state) || state.state != EPD_OPERATION_FINISHED) return;
        ok = state.status == EPD_OPERATION_OK;
    }
    if (writing == io) {
        writing = NULL;
        while (waiting_reads) {
            config_io_t *reader = waiting_reads;
            waiting_reads = reader->next;
            reader->next = NULL;
            wake(reader);
        }
    }
    /* UI delivery precedes destruction, after all worker accesses have ended. */
    if (io->done) io->done(io->save, ok, io->presses, io->context);
    release(io);
    lv_timer_delete(timer);
    epd_app_free(io);
}

config_io_t *config_io_start(bool save, uint32_t presses, config_io_done_t done, void *context)
{
    if (!done || (save && writing)) return NULL;
    config_io_t *io = epd_app_alloc(sizeof(*io), EPD_APP_PSRAM);
    if (!io) return NULL;
    memset(io, 0, sizeof(*io)); io->task = EPD_MEMORY_OWNER_INVALID;
    io->save = save; io->presses = presses; io->done = done; io->context = context;
    if (epd_app_task_group_create(&io->group) != EPD_MEMORY_OK ||
        epd_app_task_begin(io->group, &io->task) != EPD_MEMORY_OK) goto failed;
    epd_app_background_policy_t policy = {EPD_APP_BG_TASKS, io->group, EPD_APP_BG_RELEASE};
    if (epd_app_background_set(&policy, NULL) != EPD_MEMORY_OK ||
        !epd_operation_scope_open(io->task, io->task, &io->business) ||
        !epd_operation_scope_open(io->task, io->task, &io->cleanup)) goto failed;
    epd_memory_owner_t previous = epd_memory_owner_enter(io->task);
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
    io->completion = lv_timer_create(process, 0, io);
    epd_memory_owner_leave(previous);
    if (!io->completion) goto failed;
    if (save) writing = io;
    return io;
failed:
    release(io); epd_app_free(io); return NULL;
}

void config_io_close(config_io_t *io)
{
    if (!io) return;
    io->done = NULL; io->context = NULL;
    if (!io->save) {
        io->cancelled = true;
        if (io->operation) epd_operation_cancel(io->operation);
    }
}
