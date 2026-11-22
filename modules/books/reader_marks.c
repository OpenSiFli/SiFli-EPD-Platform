/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_marks.h"
#include "compute/document_wire.h"
#include "platform/epd_app.h"
#include "storage_file.h"
#include <errno.h>
#include <rthw.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MARK_MAGIC 0x4b52414du
#define MARK_VERSION 1u

typedef struct {
    char source[EPD_READER_PATH_MAX];
    uint32_t size, modified, conversion, count;
    books_mark_t marks[BOOKS_MARK_LIMIT];
} marks_record_t;

typedef struct {
    marks_record_t record;
    char path[STORAGE_PATH_MAX];
    bool write, ok;
    unsigned retries;
    rt_tick_t after;
} marks_job_t;

static marks_job_t *resident, *job, *result;
static bool running, failed;

bool books_marks_busy(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = running || job;
    rt_hw_interrupt_enable(level);
    return busy;
}

static bool enqueue(marks_job_t *next, epd_service_t *service)
{
    if (!service || epd_service_cancelled(service) || books_marks_busy() || result) {
        epd_app_free(next);
        return false;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    next->retries = 0; next->after = rt_tick_get();
    job = next;
    rt_hw_interrupt_enable(level);
    epd_service_wake(service);
    failed = false;
    return true;
}

bool books_marks_request(const bookshelf_item_t *file, epd_service_t *service)
{
    if (!file) return false;
    if (resident && !strcmp(resident->record.source, file->path) &&
        resident->record.size == file->size && resident->record.modified == file->modified) return true;
    if (books_marks_busy() || result) return false;
    epd_app_free(resident);
    resident = NULL;
    marks_job_t *next = epd_app_alloc(sizeof(*next), EPD_APP_PSRAM);
    if (!next) return false;
    memset(next, 0, sizeof(*next));
    snprintf(next->record.source, sizeof(next->record.source), "%s", file->path);
    next->record.size = file->size;
    next->record.modified = file->modified;
    next->record.conversion = DOCUMENT_CONVERSION_VERSION;
    char name[32];
    snprintf(name, sizeof(name), "marks-%08lx.bin",
             (unsigned long)storage_crc32(file->path, strlen(file->path)));
    if (!storage_app_path(next->path, sizeof(next->path), "books", STORAGE_APP_DATA, name)) {
        epd_app_free(next);
        return false;
    }
    return enqueue(next, service);
}

rt_int32_t books_marks_delay(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_int32_t delay = job ? (rt_int32_t)(job->after - rt_tick_get()) : RT_WAITING_FOREVER;
    if (job && delay < 0) delay = 0;
    rt_hw_interrupt_enable(level);
    return delay;
}

bool books_marks_worker(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    marks_job_t *next = job;
    if (next && (rt_int32_t)(rt_tick_get() - next->after) < 0) {
        rt_hw_interrupt_enable(level);
        return false;
    }
    job = NULL;
    running = next != NULL;
    rt_hw_interrupt_enable(level);
    if (!next) return false;
    if (!storage_changing()) {
        if (next->write) {
            errno = 0;
            next->ok = storage_record_save(next->path, MARK_MAGIC, MARK_VERSION,
                                           &next->record, sizeof(next->record));
            if (!next->ok && (errno == -ENOMEM || errno == ENOMEM || errno == -EBUSY || errno == EBUSY)
                && next->retries++ < 2) {
                next->after = rt_tick_get() + rt_tick_from_millisecond(250);
                level = rt_hw_interrupt_disable();
                job = next;
                running = false;
                rt_hw_interrupt_enable(level);
                return true;
            }
            if (!next->ok) rt_kprintf("books: mark save failed errno=%d\n", errno);
        } else {
            char expected[EPD_READER_PATH_MAX];
            memcpy(expected, next->record.source, sizeof(expected));
            uint32_t size = next->record.size, modified = next->record.modified;
            bool loaded = storage_record_load(next->path, MARK_MAGIC, MARK_VERSION,
                                             &next->record, sizeof(next->record));
            if (loaded && memchr(next->record.source, 0, sizeof(expected)) &&
                !strcmp(expected, next->record.source) && next->record.count <= BOOKS_MARK_LIMIT) {
                next->ok = true;
                if (size != next->record.size || modified != next->record.modified)
                    next->record.count = 0;
                for (unsigned i = 0; i < next->record.count; ++i) {
                    books_mark_t *mark = &next->record.marks[i];
                    mark->note[sizeof(mark->note) - 1u] = 0;
                    mark->excerpt[sizeof(mark->excerpt) - 1u] = 0;
                    if (next->record.conversion != DOCUMENT_CONVERSION_VERSION &&
                        mark->source.unit != BOOK_DOCUMENT_SOURCE_FILE_BYTE) {
                        next->ok = false;
                        break;
                    }
                }
                next->record.size = size; next->record.modified = modified;
            } else if (!loaded) {
                struct stat st;
                storage_lock();
                next->ok = stat(next->path, &st) < 0 && (errno == ENOENT || errno == -ENOENT);
                storage_unlock();
                if (next->ok) {
                    memset(&next->record, 0, sizeof(next->record));
                    memcpy(next->record.source, expected, sizeof(expected));
                    next->record.size = size; next->record.modified = modified;
                }
            }
            next->record.conversion = DOCUMENT_CONVERSION_VERSION;
        }
    }
    level = rt_hw_interrupt_disable();
    result = next;
    running = false;
    rt_hw_interrupt_enable(level);
    epd_service_ui_request(service, 0);
    return true;
}

bool books_marks_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    marks_job_t *ready = result;
    result = NULL;
    rt_hw_interrupt_enable(level);
    if (!ready) return false;
    failed = !ready->ok;
    if (ready->ok) {
        epd_app_free(resident);
        resident = ready;
    } else epd_app_free(ready);
    return true;
}

bool books_marks_ready(void) { return resident != NULL; }
bool books_marks_failed(void) { return failed; }

const books_mark_t *books_marks_list(unsigned *count)
{
    *count = resident ? resident->record.count : 0;
    return resident ? resident->record.marks : NULL;
}

bool books_marks_submit(epd_service_t *service, unsigned index, const books_mark_t *mark)
{
    if (!resident || books_marks_busy() || index > resident->record.count ||
        (index == resident->record.count && (!mark || index == BOOKS_MARK_LIMIT))) return false;
    marks_job_t *next = epd_app_alloc(sizeof(*next), EPD_APP_PSRAM);
    if (!next) return false;
    *next = *resident;
    next->write = true; next->ok = false;
    if (mark) {
        next->record.marks[index] = *mark;
        if (index == next->record.count) ++next->record.count;
    } else {
        --next->record.count;
        memmove(next->record.marks + index, next->record.marks + index + 1,
                (next->record.count - index) * sizeof(*next->record.marks));
        memset(next->record.marks + next->record.count, 0, sizeof(*next->record.marks));
    }
    return enqueue(next, service);
}

void books_marks_close(void)
{
    epd_app_free(resident); epd_app_free(job); epd_app_free(result);
    resident = job = result = NULL;
    failed = running = false;
}
