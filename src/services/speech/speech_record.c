#include "epd_record.h"
#include "speech_audio.h"
#include "speech_core.h"
#include "speech_media.h"
#include "storage.h"
#include "storage_internal.h"
#include "net/network.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <rtm.h>
#include <stdio.h>
#include <string.h>

#define RECORD_LIMIT_MS 3600000u
struct epd_record {
    speech_audio_input_t *input;
    sleep_guard_owner_t sleep;
    epd_record_cancel_fn cancel;
    void *user;
    uint32_t card, bytes, synced_at;
    epd_speech_error_t error;
    int fd;
    bool paused, finished, lease, draining;
    char path[STORAGE_PATH_MAX];
    unsigned char packet[6400];
};
static bool valid(uint32_t card)
{ return card && storage_session_valid(STORAGE_SD, card) && storage_available(STORAGE_SD); }
static bool path_valid(const char *path)
{ return path && !strncmp(path, "/sdcard/", 8) && !strstr(path, "..") && !strchr(path, '\\') && strlen(path) < STORAGE_PATH_MAX; }
static uint32_t read32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void write32(unsigned char *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i)); }
static bool header(int fd, uint32_t bytes)
{
    unsigned char h[44] = {0}; memcpy(h, "RIFF", 4); write32(h + 4, bytes + 36); memcpy(h + 8, "WAVEfmt ", 8);
    write32(h + 16, 16); h[20] = h[22] = 1; write32(h + 24, 16000); write32(h + 28, 32000);
    h[32] = 2; h[34] = 16; memcpy(h + 36, "data", 4); write32(h + 40, bytes);
    return lseek(fd, 0, SEEK_SET) == 0 && write(fd, h, sizeof(h)) == sizeof(h) &&
           lseek(fd, (off_t)(44u + bytes), SEEK_SET) == (off_t)(44u + bytes);
}
static bool wav_header(const unsigned char *h)
{
    return !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && read32(h + 16) == 16 &&
        h[20] == 1 && !h[21] && h[22] == 1 && !h[23] && read32(h + 24) == 16000 &&
        read32(h + 28) == 32000 && h[32] == 2 && !h[33] && h[34] == 16 && !h[35] && !memcmp(h + 36, "data", 4);
}
static bool cancelled(void *user)
{
    epd_record_t *r = user;
    return !valid(r->card) || (!r->draining && (storage_release_requested() || (r->cancel && r->cancel(r->user))));
}
static epd_speech_error_t checkpoint(epd_record_t *r)
{
    if (!valid(r->card)) return EPD_SPEECH_STORAGE;
    if (r->paused && r->fd < 0) return r->error;
    storage_lock(); bool ok = valid(r->card) && r->fd >= 0 && header(r->fd, r->bytes) && fsync(r->fd) == 0; storage_unlock();
    if (ok) r->synced_at = speech_now_ms();
    return ok ? EPD_SPEECH_OK : EPD_SPEECH_STORAGE;
}
static epd_speech_error_t start_capture(epd_record_t *r)
{
    if (cancelled(r)) return EPD_SPEECH_CANCELLED;
    if (r->bytes / 32u >= RECORD_LIMIT_MS) return EPD_SPEECH_AUDIO_TOO_LARGE;
    if (!sleep_guard_acquire(&r->sleep)) return EPD_SPEECH_BUSY;
    uint32_t id, intent;
    speech_tts_suspend(NULL, &id, &intent);
    epd_speech_error_t e = speech_media_acquire(EPD_SPEECH_ASR, cancelled, r);
    if (!e) {
        r->lease = true;
        e = speech_audio_input_open_limited(cancelled, r, RECORD_LIMIT_MS - r->bytes / 32u, &r->input);
    }
    if (e) {
        if (r->lease) { speech_media_release(EPD_SPEECH_ASR, false); r->lease = false; }
        sleep_guard_release(&r->sleep);
    } else { r->paused = false; r->draining = false; r->finished = false; }
    return e;
}
epd_speech_error_t epd_record_open(const char *path, uint32_t card, epd_record_cancel_fn cancel, void *user, epd_record_t **out)
{
    if (!out || !path_valid(path) || !valid(card)) return EPD_SPEECH_STORAGE;
    *out = NULL;
    epd_speech_capability_t capability;
    epd_speech_error_t capability_error = epd_speech_capability(EPD_SPEECH_ASR, &capability);
    if (capability_error) return capability_error;
    if (capability.busy) return EPD_SPEECH_BUSY;
    if (!capability.input_available) return EPD_SPEECH_INPUT_UNAVAILABLE;
    epd_record_t *r = speech_alloc(sizeof(*r)); if (!r) return EPD_SPEECH_NO_MEMORY;
    memset(r, 0, sizeof(*r)); strcpy(r->path, path); r->fd = -1;
    r->card = card; r->cancel = cancel; r->user = user; r->sleep.id = "record";
    storage_info_t info;
    if (!storage_info(STORAGE_SD, &info) || info.total < info.used || info.total - info.used < 1024u * 1024u) { speech_free(r); return EPD_SPEECH_STORAGE; }
    storage_lock(); r->fd = valid(card) ? open(path, O_WRONLY | O_CREAT | O_EXCL, 0) : -1; storage_unlock();
    epd_speech_error_t e = r->fd < 0 ? EPD_SPEECH_STORAGE : checkpoint(r);
    bool created = r->fd >= 0;
    if (!e) e = start_capture(r);
    if (e) {
        epd_record_close(r);
        storage_lock(); if (created && valid(card)) unlink(path); storage_unlock();
        return e;
    }
    *out = r; return EPD_SPEECH_OK;
}
epd_speech_error_t epd_record_pump(epd_record_t *r)
{
    if (!r) return EPD_SPEECH_INVALID;
    if (r->error || r->paused || r->finished) return r->error;
    if (!valid(r->card)) return r->error = EPD_SPEECH_STORAGE;
    if (!speech_media_can_start(EPD_SPEECH_ASR)) return r->error = EPD_SPEECH_OUTPUT_INTERRUPTED;
    bool empty = false;
    for (unsigned batch = 0; batch < 8; ++batch) {
        int count = speech_audio_input_read(r->input, r->packet, sizeof(r->packet));
        if (count < 0) return r->error = (epd_speech_error_t)-count;
        if (!count) { empty = true; break; }
        storage_lock(); int written = valid(r->card) ? write(r->fd, r->packet, count) : -1; storage_unlock();
        if (written > 0) r->bytes += (uint32_t)written & ~1u;
        if (written != count) return r->error = EPD_SPEECH_STORAGE;
        speech_audio_input_ack(r->input, (size_t)written);
    }
    speech_audio_input_status_t state; speech_audio_input_status(r->input, &state);
    if (state.finished && empty) { r->finished = true; if (state.error) r->error = state.error; }
    if (!r->error && speech_now_ms() - r->synced_at >= 2000u) r->error = checkpoint(r);
    return r->error;
}
static epd_speech_error_t stop_capture(epd_record_t *r)
{
    if (r->input) {
        r->draining = true;
        speech_audio_input_finish(r->input);
        uint32_t started = speech_now_ms();
        while (!r->error && !r->finished && speech_now_ms() - started < 2000u) {
            epd_record_pump(r); rt_thread_mdelay(5);
        }
        if (!r->error && !r->finished) r->error = EPD_SPEECH_TIMEOUT;
        speech_audio_input_close(r->input); r->input = NULL;
    }
    if (r->lease) { speech_media_release(EPD_SPEECH_ASR, false); r->lease = false; }
    epd_speech_error_t e = checkpoint(r);
    if (r->fd >= 0) {
        storage_lock();
        if (close(r->fd) && !e) e = EPD_SPEECH_STORAGE;
        storage_unlock();
        r->fd = -1;
    }
    sleep_guard_release(&r->sleep);
    return r->error ? r->error : e;
}
epd_speech_error_t epd_record_pause(epd_record_t *r)
{
    if (!r || r->paused) return EPD_SPEECH_INVALID;
    epd_speech_error_t e = stop_capture(r); r->paused = true; r->finished = false; return e;
}
epd_speech_error_t epd_record_resume(epd_record_t *r)
{
    if (!r || !r->paused || r->error) return EPD_SPEECH_INVALID;
    if (!valid(r->card)) return EPD_SPEECH_STORAGE;
    storage_lock();
    r->fd = valid(r->card) ? open(r->path, O_RDWR, 0) : -1;
    struct stat info;
    bool ok = r->fd >= 0 && !fstat(r->fd, &info) && info.st_size == (off_t)(44u + r->bytes) &&
        lseek(r->fd, 44u + r->bytes, SEEK_SET) == (off_t)(44u + r->bytes);
    storage_unlock();
    epd_speech_error_t e = ok ? start_capture(r) : EPD_SPEECH_STORAGE;
    if (e && r->fd >= 0) {
        storage_lock(); close(r->fd); storage_unlock(); r->fd = -1;
    }
    return e;
}
void epd_record_status(epd_record_t *r, epd_record_status_t *out)
{
    if (r && out) *out = (epd_record_status_t){r->bytes / 32u, r->bytes, r->paused, r->finished, r->error};
}
epd_speech_error_t epd_record_close(epd_record_t *r)
{
    if (!r) return EPD_SPEECH_INVALID;
    epd_speech_error_t e = stop_capture(r);
    if (r->fd >= 0) { storage_lock(); close(r->fd); storage_unlock(); }
    speech_free(r); return e;
}
epd_speech_error_t epd_record_recover(const char *path, uint32_t card, uint32_t *milliseconds)
{
    if (!path_valid(path) || !valid(card)) return EPD_SPEECH_STORAGE;
    storage_lock(); int fd = valid(card) ? open(path, O_RDWR, 0) : -1;
    struct stat info = {0}; unsigned char h[44]; bool ok = fd >= 0 && !fstat(fd, &info) && info.st_size >= 44 &&
        info.st_size <= INT32_MAX && read(fd, h, 44) == 44 && wav_header(h);
    uint32_t bytes = ok ? ((uint32_t)info.st_size - 44u) & ~1u : 0;
    if (ok) ok = header(fd, bytes) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    storage_unlock();
    if (ok && milliseconds) *milliseconds = bytes / 32u;
    return ok ? EPD_SPEECH_OK : EPD_SPEECH_STORAGE;
}
typedef struct {
    epd_record_cancel_fn cancel; void *user; uint32_t card;
    char *text; size_t used, capacity; bool overflow;
    const speech_account_t *account;
    uint32_t checked_at;
    bool config_changed;
} transcription_t;
static bool transcribe_cancel(void *user)
{
    transcription_t *t = user;
    if (t->account && speech_now_ms() - t->checked_at >= 250u) {
        speech_account_t current; memset(&current, 0, sizeof(current));
        t->config_changed = speech_config_account(EPD_SPEECH_ASR, &current) != EPD_SPEECH_OK ||
            current.provider != t->account->provider || current.enabled != t->account->enabled ||
            strcmp(current.app_id, t->account->app_id) || strcmp(current.secret_id, t->account->secret_id) ||
            strcmp(current.secret, t->account->secret) || strcmp(current.workspace, t->account->workspace);
        memset(&current, 0, sizeof(current)); t->checked_at = speech_now_ms();
    }
    return t->config_changed || !valid(t->card) || storage_release_requested() || (t->cancel && t->cancel(t->user));
}
static bool transcribe_text(void *user, uint32_t sentence, bool final, const char *text, size_t size)
{
    (void)sentence; transcription_t *t = user;
    if (!final) return true;
    if (size + 2 > t->capacity - t->used) { t->overflow = true; return false; }
    if (t->used) t->text[t->used++] = '\n';
    memcpy(t->text + t->used, text, size); t->used += size; t->text[t->used] = 0; return true;
}
epd_speech_error_t epd_record_transcribe(const char *path, uint32_t card, uint32_t begin, uint32_t duration,
    epd_record_cancel_fn cancel, void *user, char *text, size_t capacity)
{
    if (!path_valid(path) || !text || capacity < 2 || !duration || duration > 120000u || begin > RECORD_LIMIT_MS) return EPD_SPEECH_INVALID;
    text[0] = 0;
    if (!valid(card)) return EPD_SPEECH_STORAGE;
    if (!network_ready()) return EPD_SPEECH_NO_NETWORK;
    transcription_t task = {.cancel=cancel, .user=user, .card=card, .text=text, .capacity=capacity};
    sleep_guard_owner_t hold = {.id="file-asr"};
    if (!sleep_guard_acquire(&hold)) return EPD_SPEECH_BUSY;
    speech_account_t account; memset(&account, 0, sizeof(account));
    speech_provider_session_t *connection = NULL; int fd = -1; unsigned char *packet = NULL;
    epd_speech_error_t e = speech_config_account(EPD_SPEECH_ASR, &account); if (e) goto done;
    task.account = &account; task.checked_at = speech_now_ms();
    storage_lock(); fd = valid(card) ? open(path, O_RDONLY, 0) : -1;
    unsigned char h[44]; struct stat info;
    bool ok = fd >= 0 && !fstat(fd, &info) && read(fd, h, 44) == 44 && wav_header(h) &&
        (uint64_t)read32(h + 40) + 44u <= (uint64_t)info.st_size;
    uint32_t size = ok ? read32(h + 40) : 0;
    if (ok) ok = (uint64_t)begin * 32u < size && lseek(fd, 44u + begin * 32u, SEEK_SET) == (off_t)(44u + begin * 32u);
    storage_unlock();
    if (!ok) { e = EPD_SPEECH_UNSUPPORTED; goto done; }
    size -= begin * 32u; if (size > duration * 32u) size = duration * 32u;
    packet = speech_alloc(6400); if (!packet) { e = EPD_SPEECH_NO_MEMORY; goto done; }
    speech_provider_sink_t sink = {.text=transcribe_text, .user=&task}; speech_failure_t failure;
    memset(&failure, 0, sizeof(failure));
    e = speech_provider_open(&account, EPD_SPEECH_ASR, EPD_SPEECH_ZH_EN, transcribe_cancel, &task, &sink, &connection, &failure);
    if (e) goto done;
    uint32_t started = speech_now_ms(); bool finished = false;
    for (uint32_t pos = 0; pos < size;) {
        if (transcribe_cancel(&task)) { e = EPD_SPEECH_CANCELLED; goto done; }
        size_t take = size - pos; if (take > 6400) take = 6400;
        storage_lock(); int read_count = valid(card) ? read(fd, packet, take) : -1; storage_unlock();
        if (read_count != (int)take) { e = EPD_SPEECH_STORAGE; goto done; }
        e = speech_provider_audio(connection, packet, take); if (e) goto done;
        pos += (uint32_t)take;
        do {
            e = speech_provider_poll(connection, 10, &finished);
            if (finished && !e) e = EPD_SPEECH_PROTOCOL;
            if (e) goto done;
        } while (speech_now_ms() - started < pos / 32u && !transcribe_cancel(&task));
    }
    e = speech_provider_finish(connection);
    started = speech_now_ms();
    while (!e && !finished && !transcribe_cancel(&task) && speech_now_ms() - started < 30000u)
        e = speech_provider_poll(connection, 20, &finished);
    if (!e && !finished) e = transcribe_cancel(&task) ? EPD_SPEECH_CANCELLED : EPD_SPEECH_TIMEOUT;
done:
    if (connection) speech_provider_close(connection);
    if (fd >= 0) { storage_lock(); close(fd); storage_unlock(); }
    memset(&account, 0, sizeof(account)); speech_free(packet); sleep_guard_release(&hold);
    return task.overflow ? EPD_SPEECH_TEXT_TOO_LONG : e;
}
RTM_EXPORT(epd_record_open); RTM_EXPORT(epd_record_pump); RTM_EXPORT(epd_record_pause);
RTM_EXPORT(epd_record_resume); RTM_EXPORT(epd_record_status); RTM_EXPORT(epd_record_close);
RTM_EXPORT(epd_record_recover); RTM_EXPORT(epd_record_transcribe);
