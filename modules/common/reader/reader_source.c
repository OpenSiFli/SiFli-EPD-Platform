/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service.h"
#include "reader_internal.h"
#include "reader_compute_client.h"

static epd_service_t *reader_worker_service;
static void worker_cancel(void *unused)
{
    (void)unused;
    reader_compute_stop();
    document_worker_cancel();
}
void epd_reader_worker_wake(void)
{
    rt_enter_critical();
    if (reader_worker_service) epd_service_wake(reader_worker_service);
    rt_exit_critical();
}

void epd_reader_ui_wake(void)
{
    rt_enter_critical();
    if (reader_worker_service && !epd_service_cancelled(reader_worker_service))
        epd_service_ui_request(reader_worker_service, 0);
    rt_exit_critical();
}

bool epd_reader_open(const reader_options_t *options)
{
    epd_reader_close();
    if (!options) { epd_reader_set_error_code(READER_ERROR_INPUT); return false; }
    char error[96] = {0};
    document_t *document = NULL;
    if (options->kind == READER_SOURCE_TXT || options->kind == READER_SOURCE_XHTML)
    {
        document_source_t source = {
            .app_id = options->app_id, .cache_path = options->cache_path,
            .identity = options->content_identity, .path = options->path, .title = options->title,
            .format = options->kind == READER_SOURCE_TXT ? DOCUMENT_TXT : DOCUMENT_XHTML,
            .resources = options->resources, .cancelled = options->cancelled,
            .release_user = options->release_user, .user = options->user, .external = true
        };
        document = document_open_source(&source, error, sizeof(error));
    }
    if (!document)
    {
        epd_reader_set_error_code((options->kind == READER_SOURCE_TXT || options->kind == READER_SOURCE_XHTML) ?
                document_open_error() : READER_ERROR_TYPE);
        if (options->release_user) options->release_user(options->user);
        return false;
    }
    return epd_reader_attach(document, options->title, &options->restore, options->save, NULL, options->user);
}

bool epd_reader_worker_step(epd_service_t *service)
{
    rt_enter_critical();
    bool changed = reader_worker_service != service;
    reader_worker_service = service;
    rt_exit_critical();
    if (changed && service && !epd_service_on_stop(service, worker_cancel, NULL)) return false;
    if (service && epd_service_cancelled(service)) return false;
    uint32_t revision = document_worker_revision();
    bool worked = reader_compute_worker_step(service);
    bool advance = worked;
    if (!worked) worked = document_worker_step(service);
    if (advance || document_worker_revision() != revision) epd_reader_view_advance();
    return worked;
}
void epd_reader_worker_shutdown(void)
{
    reader_compute_stop();
    epd_reader_view_worker_shutdown();
    unsigned retries = 0;
    while (reader_compute_shutdown() != 0)
    {
        rt_thread_mdelay(retries < 3 ? 20 : 250);
        if (retries < 3) ++retries;
    }
    document_worker_shutdown();
    if (reader_worker_service) epd_service_on_stop(reader_worker_service, NULL, NULL);
    rt_enter_critical(); reader_worker_service = NULL; rt_exit_critical();
}
void epd_reader_worker(epd_service_t *service)
{
    while (!epd_service_cancelled(service))
        if (!epd_reader_worker_step(service)) epd_service_wait(service, 5);
    epd_reader_worker_shutdown();
}
