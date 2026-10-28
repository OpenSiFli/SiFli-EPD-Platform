#include "app_service.h"
#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "words_store.h"
#include <dfs_posix.h>
#include <dlmodule.h>
#include <stdio.h>
#include <string.h>
#include "system_time.h"

#define JOB_COUNT 8u

static epd_service_t *worker;
static uint32_t generation, failed_serial, queue_head, queue_count, card_token, active_slot;
static words_job_t jobs[JOB_COUNT];
static words_result_t *completed;
static bool stopping, flush_requested, flush_finished, flush_success;
static bool scope_unavailable = true;
static words_counts_t daily;
static int32_t daily_day;

typedef struct
{
    const words_job_t *job;
    const char *path;
    int fd;
    storage_volume_t volume;
    uint32_t session;
} input_t;

static bool stale(const words_job_t *job)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool value = job->serial != generation || stopping || (worker && epd_service_cancelled(worker));
    rt_hw_interrupt_enable(level);
    return value;
}
static bool cancellable(words_job_kind_t kind)
{
    return kind == WORDS_SEARCH || kind == WORDS_DETAIL || kind == WORDS_SCOPES || kind == WORDS_SCOPE_SELECT;
}

static bool read_at(void *context, uint32_t offset, void *buffer, size_t size)
{
    input_t *input = context;
    if (stopping || (worker && epd_service_cancelled(worker)) ||
        (cancellable(input->job->kind) && stale(input->job))) return false;
    storage_lock();
    bool ok = storage_session_valid(input->volume, input->session) &&
              lseek(input->fd, offset, SEEK_SET) == (off_t)offset;
    size_t done = 0;
    while (ok && done < size)
    {
        if (!storage_session_valid(input->volume, input->session) || stopping ||
            (worker && epd_service_cancelled(worker)) ||
            (cancellable(input->job->kind) && stale(input->job)))
        { ok = false; break; }
        int n = read(input->fd, (char *)buffer + done, size - done);
        if (n <= 0) ok = false; else done += n;
    }
    storage_unlock();
    return ok;
}

static void input_close(input_t *input)
{
    if (input->fd >= 0) { storage_lock(); close(input->fd); storage_unlock(); input->fd = -1; }
}

static bool open_path(input_t *input, words_result_t *result, words_dictionary_t *dictionary, const char *path)
{
    struct stat st = {0};
    snprintf(result->path, sizeof(result->path), "%s", path);
    input->path = result->path;
    storage_lock();
    input->volume = !strcmp(storage_path_root(path), "/sdcard") ? STORAGE_SD : STORAGE_FLASH;
    input->session = storage_card_session();
    bool valid = storage_session_valid(input->volume, input->session) && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size > 0 && st.st_size <= INT32_MAX;
    input->fd = valid ? open(path, O_RDONLY) : -1;
    storage_unlock();
    if (input->fd < 0) return false;
    if (words_dictionary_open(dictionary, read_at, input, (uint32_t)st.st_size) &&
        read_at(input, 40, result->identity, sizeof(result->identity))) return true;
    input_close(input); result->error = WORDS_MESSAGE_DICTIONARY_INVALID; return false;
}

static bool open_dictionary(input_t *input, words_result_t *result, words_dictionary_t *dictionary)
{
    char builtin[128];
    storage_app_path(builtin, sizeof(builtin), "words", STORAGE_APP_CODE, "res/library.wdb");
    char custom[128];
    storage_app_path(custom, sizeof(custom), "words", STORAGE_APP_DATA, "library.wdb");
    const char *paths[] = {custom, "/sdcard/words/library.wdb", builtin};
    if (input->job->path[0]) return open_path(input, result, dictionary, input->job->path);
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        if (!paths[i][0]) continue;
        if (open_path(input, result, dictionary, paths[i])) return true;
        if (result->error) return false;
    }
    return false;
}

static int64_t clock_now(int32_t *day)
{
    int64_t now = system_time_get_utc();
    *day = system_time_local_day(now);
    return now;
}

static bool update_day(words_result_t *result, int64_t *now, int32_t *day)
{
    *now = clock_now(day);
    result->clock_valid = system_time_utc_valid(*now);
    if (daily_day != *day)
    {
        if (!words_store_counts(*day, *now, &daily)) return false;
        daily_day = *day;
    }
    return true;
}

static bool put_record(uint32_t slot, const words_record_t *old, const words_record_t *value, int32_t day)
{
    if (!words_store_put(slot, value)) return false;
    words_counts_t before = {0}, after = {0};
    words_count_record(&before, old, day); words_count_record(&after, value, day);
    daily.added += after.added - before.added; daily.reviewed += after.reviewed - before.reviewed;
    daily.ratings += after.ratings - before.ratings; daily.collected += after.collected - before.collected;
    return true;
}

static void summary(words_result_t *result, int64_t now)
{
    words_config_t *config = words_store_config();
    result->quota = config->quota; strcpy(result->scope_name, config->scope_name);
    strcpy(result->scope_id, config->scope_id);
    result->scope_missing = scope_unavailable;
    result->counts = daily; result->counts.due = 0;
    const words_index_t *index = words_store_index();
    for (uint32_t i = 0; i < words_store_count(); ++i)
        if (index[i].phase != WORDS_NEW && !(index[i].flags & WORDS_PAUSED) && index[i].due <= now)
            ++result->counts.due;
}

static bool load_entry(input_t *input, words_result_t *result, words_dictionary_t *dictionary, uint32_t entry)
{
    uint32_t size;
    if (!words_dictionary_entry_size(dictionary, entry, &size) || !words_result_reserve(result, size)) return false;
    if (!words_dictionary_read_entry(dictionary, entry, result->content, size, &result->view) ||
        !words_dictionary_unchanged(dictionary))
    { result->error = WORDS_MESSAGE_DICTIONARY_READ_CHANGED; return false; }
    (void)input;
    return words_result_parse(result);
}

/* Due cards survive dictionary removal; available current content replaces only their snapshot. */
static bool saved_entry(input_t *input, words_result_t *result, uint32_t slot)
{
    if (!words_store_content(slot, result)) return false;
    char word[WORDS_KEY_SIZE]; strcpy(word, result->fields[WORDS_WORD]);
    words_dictionary_t dictionary;
    if (!open_dictionary(input, result, &dictionary)) return !result->error;
    uint32_t entry; bool found;
    bool ok = words_dictionary_find(&dictionary, word, &entry, &found);
    if (ok && found) ok = load_entry(input, result, &dictionary, entry) &&
        !strcmp(word, result->fields[WORDS_WORD]) && words_store_ensure(result) == (int)slot;
    if (ok) ok = words_dictionary_unchanged(&dictionary);
    input_close(input);
    if (ok) { result->slot = slot + 1; result->flags = words_store_index()[slot].flags; }
    return ok;
}

static bool read_entry(input_t *input, words_result_t *result)
{
    if (input->job->slot) return saved_entry(input, result, input->job->slot - 1);
    words_dictionary_t dictionary;
    if (!open_dictionary(input, result, &dictionary))
    { if (!result->error) result->error = WORDS_MESSAGE_DICTIONARY_MISSING; return false; }
    bool ok = !memcmp(input->job->identity, result->identity, sizeof(result->identity));
    if (!ok) result->error = WORDS_MESSAGE_DICTIONARY_CHANGED_SEARCH;
    else ok = load_entry(input, result, &dictionary, input->job->entry);
    input_close(input);
    if (ok)
    {
        int slot = words_store_find(result->fields[WORDS_WORD]);
        if (slot == -2) return false;
        if (slot >= 0) { result->slot = slot + 1; result->flags = words_store_index()[slot].flags; }
    }
    return ok;
}

static bool current_scope(words_dictionary_t *dictionary, words_scope_t *scope)
{
    scope_unavailable = true;
    words_config_t *config = words_store_config();
    bool found = false; words_scope_t candidate, first = {0};
    for (uint32_t i = 0; i < dictionary->scopes; ++i)
    {
        if (!words_dictionary_scope(dictionary, i, &candidate)) return false;
        if (!i) first = candidate;
        if (!strcmp(candidate.id, config->scope_id) ||
            (!config->scope_id[0] && !strcmp(candidate.id, "cet4")))
        { *scope = candidate; found = true; }
    }
    if (!words_dictionary_unchanged(dictionary)) return false;
    if (!found && !config->scope_id[0] && dictionary->scopes) { *scope = first; found = true; }
    if (found && (memcmp(dictionary->identity, config->identity, sizeof(config->identity)) ||
        strcmp(config->scope_id, scope->id) || strcmp(config->scope_name, scope->name)))
    {
        memcpy(config->identity, dictionary->identity, sizeof(config->identity));
        strcpy(config->scope_id, scope->id); strcpy(config->scope_name, scope->name);
        config->cursor = 0; words_store_config_changed();
    }
    scope_unavailable = !found;
    return found;
}

static int collection_new(void)
{
    const words_index_t *index = words_store_index();
    for (uint32_t i = 0; i < words_store_count(); ++i)
        if (index[i].phase == WORDS_NEW && (index[i].flags & (WORDS_PAUSED | WORDS_COLLECTED)) == WORDS_COLLECTED)
            return (int)i;
    return -1;
}

static bool next_card(input_t *input, words_result_t *result, int64_t now)
{
    words_config_t *config = words_store_config();
    const words_index_t *index = words_store_index();
    if (!result->clock_valid) { result->error = WORDS_MESSAGE_CLOCK_REQUIRED; return false; }
    int selected = -1;
    if (config->pending && config->pending <= words_store_count())
    {
        uint32_t i = config->pending - 1;
        if (!(index[i].flags & WORDS_PAUSED) && (index[i].phase != WORDS_NEW || daily.added < config->quota))
            selected = (int)i;
    }
    if (selected < 0) selected = words_due_select(index, words_store_count(), now);
    /* Prefer scope members while allowing collected words a regular share of new cards. */
    if (selected < 0 && daily.added < config->quota && daily.added % 4 == 3) selected = collection_new();
    if (selected < 0 && daily.added < config->quota)
    {
        words_dictionary_t dictionary;
        if (!open_dictionary(input, result, &dictionary))
        { if (!result->error) result->error = WORDS_MESSAGE_DICTIONARY_MISSING; return false; }
        words_scope_t scope;
        if (!current_scope(&dictionary, &scope))
        { input_close(input); result->error = WORDS_MESSAGE_SCOPE_UNAVAILABLE; return false; }
        while (config->cursor < scope.count)
        {
            if (stale(input->job)) break;
            uint32_t entry;
            if (!words_dictionary_member(&dictionary, &scope, config->cursor, &entry) ||
                !load_entry(input, result, &dictionary, entry))
            { input_close(input); if (!result->error) result->error = WORDS_MESSAGE_DICTIONARY_READ_FAILED; return false; }
            int slot = words_store_find(result->fields[WORDS_WORD]);
            if (slot == -2) { input_close(input); return false; }
            if ((slot >= 0 && (words_store_index()[slot].phase != WORDS_NEW ||
                 (words_store_index()[slot].flags & WORDS_PAUSED))) || !result->fields[WORDS_TRANSLATION][0])
            { ++config->cursor; words_store_config_changed(); continue; }
            selected = words_store_ensure(result);
            if (selected < 0) { input_close(input); return false; }
            ++config->cursor; words_store_config_changed();
            break;
        }
        input_close(input);
        if (selected < 0) selected = collection_new();
    }
    result->fields[0] = NULL; result->slot = 0;
    if (selected < 0) { active_slot = 0; return true; }
    /* Keep the selected word pending even if loading its display snapshot fails. */
    if (config->pending != (uint32_t)selected + 1)
    { config->pending = selected + 1; words_store_config_changed(); }
    if (!saved_entry(input, result, selected)) return false;
    if (active_slot != (uint32_t)selected + 1 || !card_token) ++card_token;
    if (!card_token) ++card_token;
    active_slot = selected + 1; result->token = card_token;
    return true;
}

static bool list_scopes(input_t *input, words_result_t *result, bool select)
{
    words_dictionary_t dictionary; words_scope_t scope;
    if (!open_dictionary(input, result, &dictionary))
    { if (!result->error) result->error = WORDS_MESSAGE_DICTIONARY_MISSING; return false; }
    bool ok = true, selected = false;
    if (select && memcmp(input->job->identity, dictionary.identity, sizeof(dictionary.identity)))
    { result->error = WORDS_MESSAGE_DICTIONARY_CHANGED_SCOPE; ok = false; }
    for (uint32_t i = 0; ok && i < dictionary.scopes; ++i)
    {
        if (!words_dictionary_scope(&dictionary, i, &scope)) { ok = false; break; }
        if (select)
        {
            if (strcmp(scope.id, input->job->query)) continue;
            selected = true;
            break;
        }
        if (i < input->job->value) continue;
        if (result->matches.count == WORDS_MATCH_MAX) { result->matches.more = true; break; }
        unsigned n = result->matches.count++;
        strcpy(result->scope_ids[n], scope.id); strcpy(result->names[n], scope.name);
    }
    if (ok) ok = words_dictionary_unchanged(&dictionary);
    if (ok && select && selected)
    {
        words_config_t *config = words_store_config();
        if (strcmp(config->scope_id, scope.id) || memcmp(config->identity, dictionary.identity, sizeof(config->identity)))
        { strcpy(config->scope_id, scope.id); config->cursor = 0; }
        memcpy(config->identity, dictionary.identity, sizeof(config->identity));
        strcpy(config->scope_name, scope.name);
        scope_unavailable = false;
        words_store_config_changed(); result->accepted = true;
    }
    input_close(input);
    result->offset = input->job->value;
    return ok && (!select || selected);
}

static void home_scope(input_t *input, words_result_t *result)
{
    words_dictionary_t dictionary; words_scope_t scope;
    scope_unavailable = true;
    if (open_dictionary(input, result, &dictionary))
    {
        current_scope(&dictionary, &scope);
        input_close(input);
    }
}

static bool execute(input_t *input, words_result_t *result, bool ready)
{
    const words_job_t *job = input->job;
    int64_t now; int32_t day;
    if (!ready) { result->error = words_store_error(); return false; }
    if (!update_day(result, &now, &day)) return false;
    bool ok = true;
    switch (job->kind)
    {
    case WORDS_SEARCH:
    {
        words_dictionary_t dictionary;
        ok = open_dictionary(input, result, &dictionary);
        if (!ok && !result->error) result->error = WORDS_MESSAGE_DICTIONARY_MISSING;
        if (ok) ok = words_dictionary_search(&dictionary, job->query, &result->matches);
        for (unsigned i = 0; ok && i < result->matches.count; ++i)
        {
            ok = words_dictionary_word(&dictionary, result->matches.entry[i], result->names[i]);
        }
        if (ok) ok = words_dictionary_unchanged(&dictionary);
        input_close(input); break;
    }
    case WORDS_DETAIL: ok = read_entry(input, result); break;
    case WORDS_HOME: home_scope(input, result); break;
    case WORDS_NEXT: ok = next_card(input, result, now); break;
    case WORDS_RATE:
    {
        if (!job->slot || job->slot != active_slot || job->token != card_token)
        { result->error = WORDS_MESSAGE_ALREADY_RATED; return false; }
        words_record_t old, value;
        if (!result->clock_valid) { result->error = WORDS_MESSAGE_CLOCK_REQUIRED; return false; }
        if (!words_store_get(job->slot - 1, &old)) return false;
        if (!words_record_rate(&old, (words_rating_t)job->value, now, day, &value))
        { result->error = WORDS_MESSAGE_CLOCK_OR_PAUSED; return false; }
        if (!put_record(job->slot - 1, &old, &value, day)) return false;
        ++card_token; active_slot = 0;
        words_store_config()->pending = 0; words_store_config_changed(); result->accepted = true;
        if (!stale(job)) ok = next_card(input, result, now);
        break;
    }
    case WORDS_COLLECT:
    case WORDS_PAUSE:
    {
        if (!read_entry(input, result)) return false;
        int slot = words_store_ensure(result);
        words_record_t old, value;
        if (slot < 0 || !words_store_get(slot, &old)) return false;
        value = old;
        uint32_t flag = job->kind == WORDS_COLLECT ? WORDS_COLLECTED : WORDS_PAUSED;
        if (job->value) value.flags |= flag; else value.flags &= ~flag;
        if (!put_record(slot, &old, &value, day)) return false;
        result->slot = slot + 1; result->flags = value.flags; result->accepted = true;
        if (result->slot == active_slot) result->token = card_token;
        if (job->kind == WORDS_COLLECT && !job->value && value.card.phase == WORDS_NEW &&
            words_store_config()->pending == (uint32_t)slot + 1)
        {
            /* Re-evaluate scope membership after removing an unseen collection card. */
            words_store_config()->pending = words_store_config()->cursor = 0;
            words_store_config_changed(); active_slot = 0;
            if (job->token) ok = next_card(input, result, now);
        }
        break;
    }
    case WORDS_COLLECTION:
    {
        unsigned skipped = 0;
        for (uint32_t i = 0; i < words_store_count(); ++i)
        {
            if (!(words_store_index()[i].flags & WORDS_COLLECTED)) continue;
            if (skipped++ < job->value) continue;
            if (result->matches.count == WORDS_MATCH_MAX) { result->matches.more = true; break; }
            words_record_t record;
            if (!words_store_get(i, &record)) return false;
            unsigned row = result->matches.count++;
            strcpy(result->names[row], record.word); result->matches.entry[row] = i + 1;
        }
        result->offset = job->value; break;
    }
    case WORDS_QUOTA:
        if (job->value > 200) return false;
        words_store_config()->quota = job->value; words_store_config_changed(); result->accepted = true; break;
    case WORDS_SCOPES: ok = list_scopes(input, result, false); break;
    case WORDS_SCOPE_SELECT: ok = list_scopes(input, result, true); break;
    }
    summary(result, now);
    return ok;
}

void words_cancel(uint32_t serial)
{
    rt_base_t level = rt_hw_interrupt_disable();
    words_result_t *old = NULL;
    if (serial && serial == generation) { ++generation; old = completed; completed = NULL; }
    rt_hw_interrupt_enable(level); words_result_free(old);
}

uint32_t words_submit(const words_job_t *job)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker || stopping || epd_service_cancelled(worker) || queue_count == JOB_COUNT) { rt_hw_interrupt_enable(level); return 0; }
    words_result_t *old = completed; completed = NULL;
    ++generation; if (!generation) ++generation;
    jobs[(queue_head + queue_count) % JOB_COUNT] = *job;
    jobs[(queue_head + queue_count) % JOB_COUNT].serial = generation;
    ++queue_count; uint32_t serial = generation;
    rt_hw_interrupt_enable(level);
    words_result_free(old); epd_service_wake(worker); return serial;
}

words_result_t *words_take(uint32_t serial, bool *failed)
{
    rt_base_t level = rt_hw_interrupt_disable();
    words_result_t *result = completed;
    if (result && result->serial == serial) completed = NULL; else result = NULL;
    *failed = serial && failed_serial == serial;
    rt_hw_interrupt_enable(level); return result;
}

static void run(epd_service_t *service)
{
    bool ready = words_store_open(); daily_day = INT32_MIN; active_slot = 0;
    for (;;)
    {
        if (epd_service_cancelled(service)) break;
        words_job_t job = {0};
        rt_base_t level = rt_hw_interrupt_disable();
        bool pending = queue_count != 0;
        if (pending) { job = jobs[queue_head]; queue_head = (queue_head + 1) % JOB_COUNT; --queue_count; }
        rt_hw_interrupt_enable(level);
        if (!pending)
        {
            bool saved = !ready || words_store_flush();
            level = rt_hw_interrupt_disable();
            if (flush_requested) { flush_requested = false; flush_success = saved; flush_finished = true; }
            rt_hw_interrupt_enable(level);
            if (epd_service_cancelled(service)) break;
            epd_service_wait(service, RT_WAITING_FOREVER); continue;
        }
        if (cancellable(job.kind) && stale(&job)) continue;
        words_result_t *result = epd_app_alloc(sizeof(*result), EPD_APP_PSRAM);
        if (!result)
        { level = rt_hw_interrupt_disable(); failed_serial = job.serial; rt_hw_interrupt_enable(level); continue; }
        memset(result, 0, sizeof(*result)); result->serial = job.serial;
        input_t input = {&job, NULL, -1};
        bool ok = !ready || words_store_flush();
        if (ok) ok = execute(&input, result, ready);
        input_close(&input);
        if (!ok && !result->error) result->error =
              words_store_error() ? words_store_error() : WORDS_MESSAGE_DICTIONARY_READ_FAILED;
        level = rt_hw_interrupt_disable();
        if (job.serial == generation && !stopping && !epd_service_cancelled(service)) { completed = result; result = NULL; }
        rt_hw_interrupt_enable(level); words_result_free(result);
        /* Publish first; persistence is not an LVGL-thread operation. */
        if (ready && !words_store_flush()) rt_kprintf("words: storage error %u\n", (unsigned)words_store_error());
    }
    if (ready && storage_app_available("words")) words_store_flush();
    words_store_close();
    rt_base_t level = rt_hw_interrupt_disable();
    words_result_t *old = completed; completed = NULL;
    queue_count = 0; worker = NULL;
    rt_hw_interrupt_enable(level);
    words_result_free(old);
}

bool words_service_flush(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!worker) { rt_hw_interrupt_enable(level); return !words_store_dirty(); }
    if (epd_service_cancelled(worker)) { rt_hw_interrupt_enable(level); return false; }
    flush_requested = true; flush_finished = false;
    rt_hw_interrupt_enable(level); epd_service_wake(worker);
    for (unsigned i = 0; i < 500; ++i)
    {
        level = rt_hw_interrupt_disable(); bool done = flush_finished, ok = flush_success;
        bool cancelled = !worker || epd_service_cancelled(worker);
        rt_hw_interrupt_enable(level);
        if (done) return ok;
        if (cancelled) return false;
        rt_thread_mdelay(10);
    }
    return false;
}

static const epd_background_t definition = {.run = run, .stack_size = 6144, .priority = 25, .flush = words_service_flush};
bool words_service_start(void)
{
    if (worker) return !stopping;
    stopping = false; queue_head = queue_count = 0;
    scope_unavailable = true;
    failed_serial = 0; flush_requested = flush_finished = flush_success = false;
    worker = app_service_start("words", &definition, dlmodule_find("words"));
    return worker != NULL;
}
void words_service_stop(void)
{
    words_cancel(generation);
    rt_base_t level = rt_hw_interrupt_disable(); stopping = true; rt_hw_interrupt_enable(level);
    app_service_stop("words");
}
