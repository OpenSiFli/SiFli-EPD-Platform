/* Real audio clients/decoders, scheduler, wire validation and remote dispatcher. */
#define COMPUTE_REAL_COMPONENTS
#define main scheduler_cases_main
#include "host.c"
#undef main
#include "../../components/compute_mp3/mp3_client.c"
#include "../../components/compute_flac/flac_client.c"
#include "../../components/compute_pcm/pcm_client.c"
#include "epd_jpeg_compute.h"

typedef struct { uint32_t marker, batches; } parked_t;
static int32_t park_open(void **state, const epd_compute_wire_t *wire)
{
    parked_t *p = calloc(1, sizeof(*p)); assert(p);
    p->marker = wire->capability; *state = p; return EPD_COMPUTE_OK;
}
static int32_t park_step(void *state, const epd_compute_wire_t *wire)
{
    parked_t *p = state; assert(p && p->marker == wire->capability); ++p->batches;
    return EPD_COMPUTE_MORE;
}
static int32_t park_reset(void *state, const epd_compute_wire_t *wire)
{ parked_t *p = state; assert(p->marker == wire->capability); p->batches = 0; return EPD_COMPUTE_OK; }
static void park_close(void *state, const epd_compute_wire_t *wire)
{ assert(((parked_t *)state)->marker == wire->capability); free(state); }
#include "parked_ops.h"

static unsigned capability(const char *name)
{
    epd_compute_capability_t cap;
    uint32_t version = 0;
    for (uint32_t i = 0; i < epd_compute_capability_count; ++i)
        if (!strcmp(name, epd_compute_capabilities[i].name)) version = epd_compute_capabilities[i].version;
    assert(version && epd_compute_query(name, version, &cap) == 0 && cap.ready);
    assert(cap.registration.flags & EPD_COMPUTE_STEP_SHARED);
    return cap.index;
}
static unsigned char *load_file(const char *directory, const char *name, size_t *size)
{
    char path[1024];
    if (name[0] && name[1] == ':') snprintf(path, sizeof(path), "%s", name);
    else snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); rewind(f);
    unsigned char *data = calloc(1, *size + 4096); assert(data);
    assert(fread(data, 1, *size, f) == *size); fclose(f); return data;
}

typedef struct {
    epd_compute_session_t session;
    epd_compute_buffer_t input, pool, params, output;
    epd_compute_region_t region;
    epd_compute_request_t request;
    uint8_t pixels[48 * 40];
    bool done;
} jpeg_test_t;
static void jpeg_open(jpeg_test_t *j, const char *directory, unsigned number)
{
    memset(j, 0, sizeof(*j)); char name[32]; snprintf(name, sizeof(name), "image%u.jpg", number);
    size_t size; uint8_t *bytes = load_file(directory, name, &size);
    assert(epd_compute_buffer_alloc(&j->input, size));
    assert(epd_compute_buffer_alloc(&j->pool, EPD_JPEG_POOL_BYTES));
    assert(epd_compute_buffer_alloc(&j->params, sizeof(epd_jpeg_open_t)));
    assert(epd_compute_buffer_alloc(&j->output, sizeof(epd_jpeg_result_t) + 8 * (sizeof(epd_jpeg_block_t) + EPD_JPEG_MCU_PIXELS)));
    memcpy(j->input.data, bytes, size); free(bytes);
    *(epd_jpeg_open_t *)j->params.data = (epd_jpeg_open_t){{0, 0, (uint32_t)size}, {1, 0, EPD_JPEG_POOL_BYTES}, 0};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&j->input, (uint32_t)size, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&j->pool, 0, EPD_COMPUTE_WRITE),
        epd_compute_buffer_region(&j->params, sizeof(epd_jpeg_open_t), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&j->output, 0, EPD_COMPUTE_WRITE)};
    epd_compute_capability_t cap; assert(epd_compute_query(EPD_JPEG_CAPABILITY, 1, &cap) == 0 && cap.ready);
    epd_compute_requirements_t requirements = {EPD_COMPUTE_BACKGROUND, 4};
    epd_compute_request_t request = {regions, 4, {2, 0, sizeof(epd_jpeg_open_t)}, {3, 0, j->output.capacity}, NULL, NULL};
    assert(epd_compute_open(&cap, &requirements, &request, &j->session) == 0);
    j->region = regions[3];
    j->request = (epd_compute_request_t){&j->region, 1, {EPD_COMPUTE_REGION_NONE, 0, 0}, {0, 0, j->output.capacity}, NULL, NULL};
}
static void jpeg_step(jpeg_test_t *j)
{
    if (j->done) return;
    int status = epd_compute_step(j->session, &j->request);
    assert(status == EPD_COMPUTE_OK || status == EPD_COMPUTE_MORE);
    epd_jpeg_result_t *r = j->output.data; assert(r->width == 48 && r->height == 40 && r->blocks <= 8);
    uint32_t offset = sizeof(*r);
    for (uint32_t i = 0; i < r->blocks; ++i) {
        epd_jpeg_block_t *b = (void *)((uint8_t *)r + offset);
        uint32_t width = b->right - b->left + 1;
        const uint8_t *p = (const void *)(b + 1);
        for (uint32_t y = b->top; y <= b->bottom; ++y) { memcpy(j->pixels + y * 48 + b->left, p, width); p += width; }
        offset += (sizeof(*b) + b->bytes + 3) & ~3u;
    }
    assert(offset == r->bytes); j->done = status == EPD_COMPUTE_OK;
}
static void jpeg_close(jpeg_test_t *j)
{
    close_session(j->session);
    epd_compute_buffer_free(&j->output); epd_compute_buffer_free(&j->params);
    epd_compute_buffer_free(&j->pool); epd_compute_buffer_free(&j->input);
}

typedef struct { epd_flac_t decoder; uint8_t *data, *at; } flac_test_t;
static void flac_open(flac_test_t *f, const char *directory, unsigned number)
{
    char name[32]; snprintf(name, sizeof(name), "flac%u.bin", number); size_t size;
    f->data = load_file(directory, name, &size); assert(size > 34); f->at = f->data + 34;
    epd_flac_config_t config = {0}; memcpy(config.streaminfo, f->data, 34);
    assert(epd_flac_open(&config, EPD_COMPUTE_CONTINUOUS, &f->decoder) == 0);
    assert(f->decoder->session);
}
static void flac_step(flac_test_t *f, unsigned frame)
{
    uint32_t size; memcpy(&size, f->at, 4); f->at += 4;
    int16_t pcm[16]; epd_flac_result_t result;
    assert(epd_flac_decode(f->decoder, f->at, size, true, pcm, 16, &result, NULL, NULL) == 0);
    assert(result.samples == 16 && result.sample_low == frame * 16 && result.consumed == size);
    assert(!memcmp(pcm, f->at + size, sizeof(pcm))); f->at += size + sizeof(pcm);
}

typedef struct { epd_mp3_t decoder; HMP3Decoder reference; size_t offset; unsigned good; } mp3_test_t;
static void mp3_step(mp3_test_t *m, uint8_t *data, size_t size)
{
    int count = (int)(size - m->offset); if (count > 8192) count = 8192;
    int sync = MP3FindSyncWord(data + m->offset, count); assert(sync >= 0);
    m->offset += (size_t)sync; count -= sync;
    uint8_t *a = data + m->offset, *b = a; int na = count, nb = count;
    short actual[EPD_MP3_PCM_SAMPLES] = {0}, expected[EPD_MP3_PCM_SAMPLES] = {0};
    int rc = epd_mp3_decode(m->decoder, &a, &na, actual, 0, 0);
    int reference = MP3Decode(m->reference, &b, &nb, expected, 0, 0);
    assert(rc == reference && a == b && na == nb && a > data + m->offset);
    assert(!memcmp(actual, expected, sizeof(actual))); m->offset = (size_t)(a - data);
    MP3FrameInfo x, y; epd_mp3_last_info(m->decoder, &x); MP3GetLastFrameInfo(m->reference, &y);
    assert(!memcmp(&x, &y, sizeof(x))); if (!rc) ++m->good;
    assert(m->decoder->remote && !m->decoder->local);
}

static void real_priority(epd_compute_session_t font, epd_compute_session_t document, epd_pcm_t pcm)
{
    uint8_t input[16] = {0}; int16_t output[8];
    assert(epd_pcm_convert(pcm, input, 8, output, NULL, NULL) == 0);
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&pcm->params, sizeof(epd_pcm_request_t), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&pcm->input, 16, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&pcm->output, 0, EPD_COMPUTE_WRITE)};
    gate_on();
    job_t first = step_job(font), background = step_job(document), audio = step_job(pcm->session);
    audio.request = (epd_compute_request_t){regions, 3, {0, 0, sizeof(epd_pcm_request_t)}, {EPD_COMPUTE_REGION_NONE, 0, 0}, NULL, NULL};
    start(&first); await_counts(1, 0, 0); start(&background); await_counts(1, 1, 0);
    start(&audio); await_counts(1, 2, 0);
    assert(font->signals & EPD_COMPUTE_YIELD);
    permit(); join(&first, EPD_COMPUTE_MORE); await_counts(2, 1, 0);
    assert(log_capability[1] == capability(EPD_PCM_CAPABILITY));
    permit(); join(&audio, EPD_COMPUTE_OK); await_counts(3, 0, 0);
    permit(); join(&background, EPD_COMPUTE_MORE); gate_off();
}

static void real_background_progress(epd_compute_session_t font, epd_compute_session_t document)
{
    enum { N = 12 };
    epd_pcm_t audio[N]; job_t jobs[N]; epd_compute_region_t regions[N][3];
    for (unsigned i = 0; i < N; ++i) {
        assert(epd_pcm_open((epd_pcm_format_t){16, 1}, EPD_COMPUTE_CONTINUOUS, &audio[i]) == 0);
        int16_t input[8] = {1, 2, 3}, output[8];
        assert(epd_pcm_is_remote(audio[i]) && epd_pcm_convert(audio[i], input, 8, output, NULL, NULL) == 0);
        regions[i][0] = epd_compute_buffer_region(&audio[i]->params, sizeof(epd_pcm_request_t), EPD_COMPUTE_READ);
        regions[i][1] = epd_compute_buffer_region(&audio[i]->input, sizeof(input), EPD_COMPUTE_READ);
        regions[i][2] = epd_compute_buffer_region(&audio[i]->output, 0, EPD_COMPUTE_WRITE);
        jobs[i] = step_job(audio[i]->session);
        jobs[i].request = (epd_compute_request_t){regions[i], 3, {0, 0, sizeof(epd_pcm_request_t)},
            {EPD_COMPUTE_REGION_NONE, 0, 0}, NULL, NULL};
    }
    gate_on(); job_t first = step_job(font), background = step_job(document);
    start(&first); await_counts(1, 0, 0); start(&background); await_counts(1, 1, 0);
    for (unsigned i = 0; i < N; ++i) { start(&jobs[i]); await_counts(1, i + 2, 0); }
    unsigned background_at = N + 2;
    for (unsigned i = 0; i < N + 2; ++i) {
        await_counts(i + 1, N + 1 - i, 0);
        if (log_session[i] == document->wire.session) {
            background_at = i; assert(!(document->signals & EPD_COMPUTE_YIELD));
        }
        permit();
    }
    join(&first, EPD_COMPUTE_MORE); join(&background, EPD_COMPUTE_MORE);
    assert(background_at == EPD_COMPUTE_MAX_BYPASSES + 1);
    for (unsigned i = 0; i < N; ++i) join(&jobs[i], EPD_COMPUTE_OK);
    gate_off();
    for (unsigned i = 0; i < N; ++i) assert(epd_pcm_close(audio[i]) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 3 && sizeof(void *) == 4);
    InitializeCriticalSection(&irq); InitializeConditionVariable(&changed);
    transport_gate = CreateSemaphoreA(NULL, 0, 65535, NULL); assert(transport_gate && compute_init() == 0);
    epd_compute_session_t font = open_as(capability("epd/reader-font"), EPD_COMPUTE_FOREGROUND);
    epd_compute_session_t document = open_as(capability("epd/reader-document"), EPD_COMPUTE_BACKGROUND);
    unsigned font_id = font->wire.session, document_id = document->wire.session;
    mp3_test_t mp3[2] = {0}; flac_test_t flac[2]; jpeg_test_t jpeg[2], baseline[2]; epd_pcm_t pcm;
    size_t mp3_size; uint8_t *mp3_data = load_file(".", argv[2], &mp3_size);
    for (unsigned i = 0; i < 2; ++i) {
        assert(epd_mp3_open(&mp3[i].decoder) == 0 && mp3[i].decoder->remote && !mp3[i].decoder->local);
        mp3[i].reference = MP3InitDecoder(); assert(mp3[i].reference);
        flac_open(&flac[i], argv[1], i);
        jpeg_open(&baseline[i], argv[1], i);
        while (!baseline[i].done) jpeg_step(&baseline[i]);
        jpeg_close(&baseline[i]); jpeg_open(&jpeg[i], argv[1], i);
    }
    assert(epd_pcm_open((epd_pcm_format_t){16, 1}, EPD_COMPUTE_CONTINUOUS, &pcm) == 0 && epd_pcm_is_remote(pcm));
    real_priority(font, document, pcm);
    real_background_progress(font, document);
    epd_compute_session_t late_reader = open_as(capability("epd/reader-layout"), EPD_COMPUTE_BACKGROUND);
    assert(epd_compute_step(late_reader, &empty_request) == EPD_COMPUTE_MORE); close_session(late_reader);
    unsigned before = opens;
    for (unsigned frame = 0; frame < 24; ++frame) {
        if (frame == 2) {
            uint32_t bytes; memcpy(&bytes, flac[0].at, 4);
            int16_t untouched[16]; memset(untouched, 0x5a, sizeof(untouched)); epd_flac_result_t partial;
            assert(epd_flac_decode(flac[0].decoder, flac[0].at + 4, bytes / 2, false,
                untouched, 16, &partial, NULL, NULL) == EPD_COMPUTE_MORE);
            assert(!partial.samples && untouched[0] == 0x5a5a);
        }
        for (unsigned i = 0; i < 2; ++i) {
            mp3_step(&mp3[i], mp3_data, mp3_size);
            if (frame < 8) flac_step(&flac[i], frame);
            jpeg_step(&jpeg[i]);
        }
        int16_t input[1024], output[1024];
        for (unsigned i = 0; i < 1024; ++i) input[i] = (int16_t)(i * 31 + frame);
        assert(epd_pcm_convert(pcm, input, 1024, output, NULL, NULL) == 0 && !memcmp(input, output, sizeof(input)));
        assert(epd_compute_set_purpose(font, frame % 2 ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND) == 0);
        assert(epd_compute_step(font, &empty_request) == EPD_COMPUTE_MORE);
        assert(epd_compute_step(document, &empty_request) == EPD_COMPUTE_MORE);
        if (frame == 5) {
            assert(epd_mp3_reset(mp3[0].decoder) == 0); MP3FreeDecoder(mp3[0].reference); mp3[0].reference = MP3InitDecoder();
            assert(epd_flac_reset(flac[0].decoder) == 0);
        }
    }
    assert(opens == before && font->wire.session == font_id && document->wire.session == document_id);
    epd_compute_cancel(mp3[0].decoder->remote);
    assert(epd_compute_step(mp3[0].decoder->remote, &empty_request) == EPD_COMPUTE_CANCELLED);
    assert(epd_compute_step(document, &empty_request) == EPD_COMPUTE_MORE);
    mp3_step(&mp3[1], mp3_data, mp3_size);
    epd_compute_cancel(font); assert(epd_compute_step(font, &empty_request) == EPD_COMPUTE_CANCELLED);
    mp3_step(&mp3[1], mp3_data, mp3_size);
    for (unsigned i = 0; i < 2; ++i) {
        assert(mp3[i].good > 10 && jpeg[i].done && !memcmp(jpeg[i].pixels, baseline[i].pixels, sizeof(jpeg[i].pixels)));
        assert(epd_mp3_close(mp3[i].decoder) == 0); MP3FreeDecoder(mp3[i].reference);
        assert(epd_flac_close(flac[i].decoder) == 0); free(flac[i].data); jpeg_close(&jpeg[i]);
    }
    assert(epd_pcm_close(pcm) == 0); close_session(font); close_session(document); free(mp3_data);
    assert(!active && !running && !queue_head && !remote_sessions && allocations == 1);
    puts("PASS real MP3/FLAC/PCM/TJPGD interleaving, private contexts, partial replay, reset/cancel isolation, remote-only clients, continuous priority and background progress");
    return 0;
}
