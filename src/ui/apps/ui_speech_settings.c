/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_internal.h"
#include "platform/epd_keyboard.h"
#include "ui_speech_settings.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "app_files.h"
#include "speech/speech_settings.h"
#include "app_service.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "storage.h"

enum {
    S_ASR=-21000, S_TTS, S_IMPORT, S_RELOAD, S_ENABLED, S_APP_ID, S_SECRET_ID,
    S_SECRET, S_WORKSPACE, S_VOICE, S_RATE, S_SAVE, S_DELETE, S_CONFIRM,
    S_INPUT_SAVE, S_INPUT_CANCEL, S_DISCARD, S_TEST_ASR, S_TEST_TTS, S_FINISH,
    S_CANCEL, S_PAUSE, S_RESUME, S_LANGUAGE, S_STATUS, S_RESULT, S_DRAFT, S_LOAD_ERROR, S_RECORD_TIME, S_VOICE_NONE,
    S_PROVIDER=-21100, S_CHOOSE=-21200, S_VOICE_CHOOSE=-21300, S_RATE_CHOOSE=-21400
};
typedef enum { V_ROOT, V_ASR, V_TTS, V_ACCOUNT, V_VOICE, V_RATE } view_mode_t;
typedef struct {
    lv_obj_t *title, *body, *editor, *input, *keyboard, *save, *cancel;
    ui_settings_list_t *list;
    lv_timer_t *timer;
    speech_settings_snapshot_t snapshot;
    speech_account_t draft;
    speech_settings_job_t job;
    speech_settings_operation_t pending_operation;
    uint32_t ticket, candidate, card_session, confirmation_serial, locale, scale, test_revision;
    int confirmation, input_key;
    int32_t width, bottom;
    view_mode_t mode;
    epd_speech_language_t language;
    bool redraw, dirty, keep_credentials, import_pending;
    char path[SPEECH_SETTINGS_PATH_SIZE], rate[24], elapsed[48];
} view_t;

/* The test owner outlives pages; only the UI thread touches this state. */
L2_RET_BSS_SECT_BEGIN(speech_preview)
static struct {
    epd_service_t *owner;
    epd_speech_session_t session;
    epd_speech_snapshot_t snapshot;
    epd_speech_error_t error;
    epd_speech_kind_t kind;
    const char *text;
    size_t sent;
    bool finishing, cancelling;
    uint32_t revision;
    char final_text[512], draft[512];
} preview L2_RET_BSS_SECT(speech_preview);
L2_RET_BSS_SECT_END

/* Retained PSRAM needs explicit boot initialization, not a page reset:
 * the preview owner can remain active across page changes and sleep. */
static int speech_preview_init(void)
{
    memset(&preview, 0, sizeof(preview));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(speech_preview_init);

static view_t *page(void)
{ return current && current->id == UI_PAGE_SPEECH_SETTINGS ? current->feature : NULL; }

static const char *error_text(epd_speech_error_t error)
{
    switch (error) {
    case EPD_SPEECH_OK: return epd_get_str(speech_saved);
    case EPD_SPEECH_BUSY: return epd_get_str(speech_busy);
    case EPD_SPEECH_NOT_CONFIGURED: return epd_get_str(speech_not_configured);
    case EPD_SPEECH_AUTH: return epd_get_str(speech_auth_error);
    case EPD_SPEECH_QUOTA: return epd_get_str(speech_quota_error);
    case EPD_SPEECH_NO_NETWORK: case EPD_SPEECH_CONNECT: return epd_get_str(speech_network_error);
    case EPD_SPEECH_TLS: return epd_get_str(speech_tls_error);
    case EPD_SPEECH_TIMEOUT: case EPD_SPEECH_OUTPUT_TIMEOUT: return epd_get_str(speech_timeout);
    case EPD_SPEECH_STORAGE: return epd_get_str(speech_storage_error);
    case EPD_SPEECH_NO_MEMORY: return epd_get_str(platform_no_memory);
    case EPD_SPEECH_INPUT_UNAVAILABLE: return epd_get_str(speech_input_error);
    case EPD_SPEECH_INPUT_OVERRUN: return epd_get_str(speech_overrun);
    case EPD_SPEECH_OUTPUT_INTERRUPTED: return epd_get_str(speech_output_error);
    case EPD_SPEECH_CANCELLED: return epd_get_str(speech_cancelled);
    case EPD_SPEECH_INVALID: case EPD_SPEECH_UNSUPPORTED: return epd_get_str(speech_invalid);
    default: return epd_get_str(speech_service_error);
    }
}
static void show_error(epd_speech_error_t error) { popup_open(error_text(error), NULL); }
static const char *provider_name(speech_provider_t p)
{
    switch (p) {
    case SPEECH_TENCENT: return epd_get_str(speech_tencent);
    case SPEECH_ALI: return epd_get_str(speech_ali);
    case SPEECH_DOUBAO: return epd_get_str(speech_doubao);
    default: return epd_get_str(speech_none);
    }
}
static const char *voice_name(const speech_voice_t *v)
{ return !strcmp(epd_get_str(speech_language_tag), "zh") ? v->name_zh : v->name_en; }

static void copy_preview(char *out, size_t capacity, const char *text, size_t bytes)
{
    size_t n = bytes < capacity-1 ? bytes : capacity-1;
    if (n < bytes) while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    if (n) memcpy(out, text, n);
    out[n] = 0;
}

static bool preview_process(epd_service_t *service, bool stopping)
{
    /* A stopping service reaches this callback only after its retained speech
     * work has released the owner. Terminal events may already be discarded. */
    if (stopping) {
        preview.session = 0; preview.owner = NULL; preview.cancelling = true;
        preview.draft[0] = 0; ++preview.revision;
        return true;
    }
    if (preview.session) {
        epd_speech_snapshot_t snapshot;
        if (preview.kind == EPD_SPEECH_ASR && epd_speech_snapshot(service, preview.session, &snapshot) == EPD_SPEECH_OK) {
            uint32_t quantum = snapshot.elapsed_ms >= EPD_SPEECH_ASR_LIMIT_MS-10000u ? 1000u : 10000u;
            if (snapshot.elapsed_ms/quantum != preview.snapshot.elapsed_ms/quantum) ++preview.revision;
            preview.snapshot.elapsed_ms = snapshot.elapsed_ms;
        }
        if (preview.cancelling) (void)epd_speech_cancel(service, preview.session);
        else if (preview.kind == EPD_SPEECH_TTS && !preview.finishing) {
            size_t size = strlen(preview.text);
            int n = preview.sent < size ? epd_speech_append(service, preview.session,
                preview.text+preview.sent, size-preview.sent) : 0;
            if (n > 0) preview.sent += (size_t)n;
            else if (n < 0 && n != -EPD_SPEECH_BUSY) {
                preview.error = (epd_speech_error_t)-n; preview.cancelling = true; ++preview.revision;
            }
            if (!preview.cancelling && preview.sent == size)
                preview.finishing = epd_speech_finish(service, preview.session) == EPD_SPEECH_OK;
        }
        epd_speech_event_t event;
        for (unsigned i = 0; i < 8 && epd_speech_poll(service, &event) == EPD_SPEECH_OK; ++i) {
            if (event.snapshot.session == preview.session) {
                preview.snapshot = event.snapshot;
                if (event.kind == EPD_SPEECH_EVENT_DRAFT && event.text)
                    copy_preview(preview.draft, sizeof(preview.draft), event.text, event.text_bytes);
                if (event.kind == EPD_SPEECH_EVENT_FINAL_TEXT && event.text) {
                    copy_preview(preview.final_text, sizeof(preview.final_text), event.text, event.text_bytes);
                    preview.draft[0] = 0;
                }
                if (event.kind == EPD_SPEECH_EVENT_TERMINAL) {
                    if (!preview.error) preview.error = event.snapshot.error;
                    preview.session = 0; preview.draft[0] = 0;
                }
                ++preview.revision;
            }
            epd_speech_event_release(&event);
        }
    }
    if (!preview.session) {
        app_service_stop("sp_test");
    }
    return false;
}
static const epd_background_t preview_definition = {.ui_process=preview_process};

static void start_preview(view_t *v, epd_speech_kind_t kind)
{
    if (preview.owner) { show_error(EPD_SPEECH_BUSY); return; }
    epd_service_t *owner = app_service_start("sp_test", &preview_definition, NULL);
    if (!owner) { show_error(EPD_SPEECH_BUSY); return; }
    uint32_t revision = preview.revision+1;
    memset(&preview, 0, sizeof(preview)); preview.owner = owner; preview.kind = kind; preview.revision = revision;
    preview.text = v->language == EPD_SPEECH_ZH ? "你好，这是文字朗读测试。你今天过得好吗？" :
        v->language == EPD_SPEECH_EN ? "Hello. This is a reading test. How are you today?" :
        "你好，Hello。这是一段中英文朗读测试。How are you today?";
    epd_speech_request_t request = {.struct_size=sizeof(request), .version=EPD_SPEECH_VERSION, .language=v->language};
    snprintf(request.title, sizeof(request.title), "%s", epd_get_str(speech_test_title));
    preview.error = epd_speech_start(owner, kind, &request, &preview.session);
    preview.snapshot.state = EPD_SPEECH_PREPARING;
    if (preview.error) { app_service_stop("sp_test"); show_error(preview.error); }
    v->redraw = true;
}

static const char *preview_status(void)
{
    if (preview.error) return error_text(preview.error);
    switch (preview.snapshot.state) {
    case EPD_SPEECH_RECORDING: return epd_get_str(speech_recording);
    case EPD_SPEECH_FINALIZING: case EPD_SPEECH_DRAINING: return epd_get_str(speech_finishing);
    case EPD_SPEECH_BUFFERING: return epd_get_str(speech_buffering);
    case EPD_SPEECH_PLAYING: return epd_get_str(speech_playing);
    case EPD_SPEECH_PAUSED: return epd_get_str(speech_paused);
    case EPD_SPEECH_COMPLETED: return epd_get_str(speech_completed);
    case EPD_SPEECH_STOPPED: return epd_get_str(speech_cancelled);
    default: return epd_get_str(speech_busy);
    }
}

static void editor_layout(view_t *v)
{
    int32_t width = lv_obj_get_width(current->screen), top = SETTINGS_CONTENT_Y;
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int32_t h = LV_MAX(epd_ui_touch(70), ui_font_settings_body()->line_height+epd_ui_px(24));
    int32_t gap = epd_ui_px(16), bw = (width-2*MARGIN-gap)/2;
    lv_obj_set_pos(v->editor, 0, top); lv_obj_set_size(v->editor, width, bottom-top);
    lv_obj_set_pos(v->input, MARGIN, 0); lv_obj_set_size(v->input, width-2*MARGIN, h);
    lv_obj_set_style_text_font(v->input, ui_font_settings_body(), 0);
    lv_obj_t *buttons[] = {v->cancel, v->save};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_pos(buttons[i], MARGIN+(int32_t)i*(bw+gap), h+gap); lv_obj_set_size(buttons[i], bw, h);
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        text_update(label, i ? epd_get_str(speech_save) : epd_get_str(platform_cancel));
        lv_obj_set_style_text_font(label, ui_font_settings_value(), 0); lv_obj_center(label);
    }
    epd_keyboard_refresh(v->keyboard);
    int32_t available = LV_MAX(1, bottom-top-2*h-3*gap);
    int32_t kh = LV_MIN(epd_keyboard_preferred_height(v->keyboard), available);
    lv_obj_set_size(v->keyboard, width, kh); lv_obj_align(v->keyboard, LV_ALIGN_BOTTOM_MID, 0, -gap);
}

static void render(view_t *v)
{
    if (!v || current->popup || !lv_refreshing_done()) return;
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    int32_t width = lv_obj_get_width(current->screen), bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (!v->redraw && v->locale == locale && v->scale == scale && v->width == width && v->bottom == bottom) return;
    lv_obj_set_pos(v->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_width(v->title, width-SETTINGS_TITLE_X-MARGIN);
    lv_obj_set_style_text_font(v->title, ui_font_settings_title(), 0);
    const char *title = v->mode >= V_ACCOUNT ? provider_name(v->draft.provider) : epd_get_str(speech_title);
    if (v->editor) title = v->input_key == S_APP_ID ? "AppID" : v->input_key == S_SECRET_ID ? "SecretId" :
        v->input_key == S_WORKSPACE ? "WorkspaceId" : v->draft.provider == SPEECH_TENCENT ? "SecretKey" : "API Key";
    text_update(v->title, title);
    int32_t top = LV_MAX(SETTINGS_CONTENT_Y, SETTINGS_TITLE_Y+ui_font_settings_title()->line_height+epd_ui_px(32));
    lv_obj_set_pos(v->body, 0, top); lv_obj_set_size(v->body, width, LV_MAX(1, bottom-top));
    if (v->editor) editor_layout(v);
    else {
        ui_settings_list_row_t rows[22]; size_t n = 0;
        bool ready = v->snapshot.initialized && !v->ticket && !v->snapshot.stopping;
        #define ROW(k,t,value_,active_) rows[n++]=(ui_settings_list_row_t){.key=(k),.kind=UI_SETTINGS_ROW_LINK,.title=(t),.value=(value_),.enabled=(active_)}
        #define INFO(k,t,value_) rows[n++]=(ui_settings_list_row_t){.key=(k),.kind=UI_SETTINGS_ROW_INFO,.title=(t),.value=(value_)}
        if (v->mode == V_ROOT) {
            ROW(S_ASR, epd_get_str(speech_default_asr), provider_name(v->snapshot.asr), ready);
            ROW(S_TTS, epd_get_str(speech_default_tts), provider_name(v->snapshot.tts), ready);
            for (unsigned p = 1; p < SPEECH_PROVIDER_COUNT; ++p)
                ROW(S_PROVIDER+(int)p, provider_name((speech_provider_t)p), !v->snapshot.accounts[p].configured ? epd_get_str(speech_none) :
                    v->snapshot.accounts[p].enabled ? epd_get_str(speech_configured) : epd_get_str(speech_disabled), ready);
            ROW(S_IMPORT, epd_get_str(speech_import), NULL, !v->ticket && !v->snapshot.stopping);
            ROW(S_LANGUAGE, epd_get_str(speech_test_language), v->language == EPD_SPEECH_EN ? epd_get_str(speech_english) :
                v->language == EPD_SPEECH_ZH ? epd_get_str(speech_chinese) : epd_get_str(speech_mixed), !preview.owner);
            ROW(S_TEST_ASR, epd_get_str(speech_test_asr), NULL, ready && v->snapshot.asr && !preview.owner);
            ROW(S_TEST_TTS, epd_get_str(speech_test_tts), NULL, ready && v->snapshot.tts && !preview.owner);
            if (preview.revision) INFO(S_STATUS, epd_get_str(speech_test_title), preview_status());
            if (preview.session && preview.kind == EPD_SPEECH_ASR) {
                uint32_t seconds = preview.snapshot.elapsed_ms/1000u;
                if (seconds < 590u) seconds = seconds/10u*10u;
                snprintf(v->elapsed, sizeof(v->elapsed), "%lu:%02lu / 10:00", (unsigned long)(seconds/60u), (unsigned long)(seconds%60u));
                INFO(S_RECORD_TIME, epd_get_str(speech_recording_time), v->elapsed);
            }
            if (preview.final_text[0]) INFO(S_RESULT, epd_get_str(speech_recognized), preview.final_text);
            if (preview.draft[0]) INFO(S_DRAFT, epd_get_str(speech_draft), preview.draft);
            if (preview.session) {
                if (preview.kind == EPD_SPEECH_ASR) ROW(S_FINISH, epd_get_str(speech_finish), NULL, !preview.finishing);
                else if (preview.snapshot.state == EPD_SPEECH_PAUSED) ROW(S_RESUME, epd_get_str(speech_resume), NULL, true);
                else ROW(S_PAUSE, epd_get_str(speech_pause), NULL, !preview.cancelling);
                ROW(S_CANCEL, epd_get_str(speech_stop), NULL, !preview.cancelling);
            }
            if (!v->snapshot.initialized || v->snapshot.initialization_error != EPD_SPEECH_OK) {
                INFO(S_LOAD_ERROR, error_text(v->snapshot.initialization_error), NULL);
                ROW(S_RELOAD, epd_get_str(speech_reload), NULL,
                    !v->ticket && !v->snapshot.busy && !v->snapshot.stopping);
            }
        } else if (v->mode == V_ASR || v->mode == V_TTS) {
            epd_speech_kind_t kind = v->mode == V_ASR ? EPD_SPEECH_ASR : EPD_SPEECH_TTS;
            speech_provider_t chosen = kind == EPD_SPEECH_ASR ? v->snapshot.asr : v->snapshot.tts;
            for (unsigned p = 0; p < SPEECH_PROVIDER_COUNT; ++p) {
                if (p && !speech_provider_supports((speech_provider_t)p, kind)) continue;
                ROW(S_CHOOSE+(int)p, provider_name((speech_provider_t)p), NULL,
                    ready && (!p || (v->snapshot.accounts[p].configured && v->snapshot.accounts[p].enabled &&
                                    (kind != EPD_SPEECH_TTS || v->snapshot.accounts[p].voice[0]))));
                rows[n-1].kind = UI_SETTINGS_ROW_CHOICE; rows[n-1].checked = chosen == (speech_provider_t)p;
            }
        } else if (v->mode == V_ACCOUNT) {
            ROW(S_ENABLED, epd_get_str(speech_enabled), NULL, !v->ticket);
            rows[n-1].kind = UI_SETTINGS_ROW_SWITCH; rows[n-1].checked = v->draft.enabled;
            #define FIELD(k,label_,field_) ROW(k,label_,(v->keep_credentials || v->draft.field_[0]) ? epd_get_str(speech_secret_set) : epd_get_str(speech_none), !v->ticket)
            if (v->draft.provider == SPEECH_TENCENT) { FIELD(S_APP_ID, "AppID", app_id); FIELD(S_SECRET_ID, "SecretId", secret_id); }
            if (v->draft.provider == SPEECH_ALI) FIELD(S_WORKSPACE, "WorkspaceId", workspace);
            FIELD(S_SECRET, v->draft.provider == SPEECH_TENCENT ? "SecretKey" : "API Key", secret);
            #undef FIELD
            size_t count; const speech_voice_t *voices = speech_settings_voices(v->draft.provider, &count);
            const char *name = epd_get_str(speech_none);
            for (size_t i = 0; i < count; ++i) if (!strcmp(v->draft.voice, voices[i].id)) name = voice_name(&voices[i]);
            snprintf(v->rate, sizeof(v->rate), "%u%%", (unsigned)(v->draft.rate*100+0.5f));
            ROW(S_VOICE, epd_get_str(speech_voice), name, !v->ticket);
            ROW(S_RATE, epd_get_str(speech_rate), v->draft.voice[0] ? v->rate : epd_get_str(speech_none),
                !v->ticket && v->draft.voice[0]);
            ROW(S_SAVE, epd_get_str(speech_save), NULL, !v->ticket && v->dirty);
            ROW(S_DELETE, epd_get_str(speech_delete), NULL, !v->ticket && v->snapshot.accounts[v->draft.provider].configured);
        } else if (v->mode == V_VOICE) {
            size_t count; const speech_voice_t *voices = speech_settings_voices(v->draft.provider, &count);
            ROW(S_VOICE_NONE, epd_get_str(speech_none), NULL, true);
            rows[n-1].kind = UI_SETTINGS_ROW_CHOICE; rows[n-1].checked = !v->draft.voice[0];
            for (size_t i = 0; i < count; ++i) {
                ROW(S_VOICE_CHOOSE+(int)i, voice_name(&voices[i]), epd_get_str(speech_mixed), true);
                rows[n-1].kind = UI_SETTINGS_ROW_CHOICE; rows[n-1].checked = !strcmp(v->draft.voice, voices[i].id);
            }
        } else {
            static const char *const rates[] = {"50%", "75%", "100%", "125%", "150%", "175%", "200%"};
            for (unsigned i = 0; i < 7; ++i) {
                bool tencent_minimum = !i && v->draft.provider == SPEECH_TENCENT;
                ROW(S_RATE_CHOOSE+(int)i, tencent_minimum ? "60%" : rates[i], NULL, true);
                rows[n-1].kind = UI_SETTINGS_ROW_CHOICE;
                rows[n-1].checked = v->draft.rate == (tencent_minimum ? 0.6f : 0.5f+0.25f*i);
            }
        }
        #undef ROW
        #undef INFO
        ui_settings_list_refresh_fonts(v->list, ui_font_settings_body(), ui_font_settings_value());
        if (!ui_settings_list_set_rows(v->list, rows, n)) return;
        ui_settings_list_set_group(v->list, current->group);
    }
    v->locale = locale; v->scale = scale; v->width = width; v->bottom = bottom; v->redraw = false;
}

static bool submit(view_t *v, speech_settings_operation_t operation)
{
    v->job.operation = operation;
    epd_speech_error_t error = speech_settings_submit(&v->job, &v->ticket);
    if (!error) v->pending_operation = operation;
    speech_config_wipe(&v->job, sizeof(v->job));
    if (error) show_error(error);
    v->redraw = true; return error == EPD_SPEECH_OK;
}
static void confirm(view_t *v, int action, lv_ext_str_id_t message)
{
    v->confirmation = action; popup_confirm_i18n(message, NULL, S_CONFIRM);
    v->confirmation_serial = current->popup_serial;
}
static void discard_import(view_t *v)
{
    if (v->candidate) speech_settings_import_discard(v->candidate);
    v->candidate = 0;
}
static bool imported(const char *path, void *context)
{
    view_t *v = context;
    if (strlen(path) >= sizeof(v->path)) return false;
    strcpy(v->path, path); v->card_session = storage_card_session(); v->import_pending = true; return true;
}

static void process(view_t *v)
{
    if (!v || !current->foreground) return;
    speech_settings_snapshot_t incoming;
    if (speech_settings_snapshot(&incoming) == EPD_SPEECH_OK && memcmp(&v->snapshot, &incoming, sizeof(incoming))) {
        v->snapshot = incoming; v->redraw = true;
    }
    if (v->test_revision != preview.revision) { v->test_revision = preview.revision; v->redraw = true; }
    if (current->popup) return;
    if (v->candidate && v->confirmation == S_IMPORT) { discard_import(v); v->confirmation = 0; }
    if (v->import_pending && !v->ticket) {
        v->import_pending = false;
        strcpy(v->job.path, v->path); v->job.card_session = v->card_session;
        submit(v, SPEECH_SETTINGS_IMPORT_PREPARE);
    }
    if (v->ticket) {
        speech_settings_result_t result;
        epd_speech_error_t error = speech_settings_poll(v->ticket, &result);
        if (error != EPD_SPEECH_BUSY) {
            v->ticket = 0; v->redraw = true;
            if (!error) error = result.error;
            if (error) { show_error(error); return; }
            if (result.operation == SPEECH_SETTINGS_IMPORT_PREPARE) {
                v->candidate = result.candidate_id; confirm(v, S_IMPORT, LV_EXT_STR_ID(speech_import_confirm)); return;
            }
            if (result.operation == SPEECH_SETTINGS_SAVE_ACCOUNT || result.operation == SPEECH_SETTINGS_REMOVE ||
                result.operation == SPEECH_SETTINGS_IMPORT) {
                speech_config_wipe(&v->draft, sizeof(v->draft)); v->dirty = false; v->mode = V_ROOT;
            }
            (void)speech_settings_snapshot(&v->snapshot);
        }
    }
    render(v);
}
static void poll(lv_timer_t *timer)
{ if (current == lv_timer_get_user_data(timer)) process(page()); }
static void selected(int key, void *context)
{ if (current == context) ui_speech_settings_action(&key); }
static void input_event(lv_event_t *event)
{
    int key = (int)(intptr_t)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_READY) key = S_INPUT_SAVE;
    ui_speech_settings_action(&key);
}

static void close_editor(view_t *v)
{
    if (!v->editor) return;
    const char *text = lv_textarea_get_text(v->input);
    speech_config_wipe((void *)text, strlen(text)); lv_textarea_set_text(v->input, "");
    lv_obj_delete(v->editor); v->editor = v->input = v->keyboard = v->save = v->cancel = NULL;
    object_visible(v->body, true); v->redraw = true;
    ui_settings_list_set_group(v->list, current->group);
}
static char *field(view_t *v, size_t *capacity)
{
    switch (v->input_key) {
    case S_APP_ID: *capacity = sizeof(v->draft.app_id); return v->draft.app_id;
    case S_SECRET_ID: *capacity = sizeof(v->draft.secret_id); return v->draft.secret_id;
    case S_WORKSPACE: *capacity = sizeof(v->draft.workspace); return v->draft.workspace;
    default: *capacity = sizeof(v->draft.secret); return v->draft.secret;
    }
}
static void edit(view_t *v, int key)
{
    v->input_key = key;
    size_t capacity; (void)field(v, &capacity);
    ui_settings_list_set_group(v->list, NULL); object_visible(v->body, false);
    v->editor = lv_obj_create(current->screen); epd_obj_init(v->editor);
    v->input = lv_textarea_create(v->editor);
    lv_textarea_set_one_line(v->input, true); lv_textarea_set_max_length(v->input, (uint32_t)capacity-1);
    lv_textarea_set_password_mode(v->input, true); lv_textarea_set_password_show_time(v->input, 0);
    lv_textarea_set_text(v->input, "");
    lv_obj_set_style_text_font(v->input, ui_font_settings_body(), 0);
    lv_obj_set_style_border_width(v->input, 2, 0); lv_obj_set_style_border_color(v->input, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(v->input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(v->input, 0, LV_PART_CURSOR);
    lv_group_add_obj(current->group, v->input);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *b = lv_button_create(v->editor); epd_obj_init(b);
        lv_obj_set_style_border_width(b, 2, 0); lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, input_event, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? S_INPUT_SAVE : S_INPUT_CANCEL));
        label_create(b, i ? epd_get_str(speech_save) : epd_get_str(platform_cancel), 0, 0, 200, ui_font_settings_value());
        lv_group_add_obj(current->group, b);
        if (i) v->save = b; else v->cancel = b;
    }
    const epd_keyboard_config_t config = {.mode = EPD_KEYBOARD_PASSWORD};
    v->keyboard = epd_keyboard_create(v->editor, v->input, &config);
    lv_obj_add_event_cb(v->keyboard, input_event, LV_EVENT_READY, NULL);
    lv_group_focus_obj(v->input);
    v->redraw = true; editor_layout(v);
}

void ui_speech_settings_create(lv_obj_t *screen)
{
    view_t *v = epd_app_alloc(sizeof(*v), EPD_APP_PSRAM);
    if (!v) { centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_settings_body()); return; }
    memset(v, 0, sizeof(*v)); current->feature = v; v->redraw = true;
    v->title = page_title_create(screen, LV_EXT_STR_ID(speech_title), ui_font_settings_title());
    v->body = lv_obj_create(screen); epd_obj_init(v->body);
    v->list = ui_settings_list_create(v->body, ui_font_settings_body(), ui_font_settings_value(), selected, current);
    v->timer = lv_timer_create(poll, 250, current);
    speech_settings_process(); process(v);
}
void ui_speech_settings_restore_focus(void)
{
    view_t *v = page(); if (!v || current->popup) return;
    if (v->editor) {
        lv_group_add_obj(current->group, v->input); lv_group_add_obj(current->group, v->cancel); lv_group_add_obj(current->group, v->save);
    } else ui_settings_list_set_group(v->list, current->group);
}
void ui_speech_settings_resume(void)
{
    view_t *v = page(); if (!v) return;
    v->redraw = true; lv_timer_resume(v->timer); speech_settings_process(); process(v);
}
void ui_speech_settings_pause(void)
{
    view_t *v = page(); if (!v) return;
    preview.cancelling = true; discard_import(v);
    if (v->ticket && v->pending_operation == SPEECH_SETTINGS_IMPORT_PREPARE) {
        speech_settings_import_discard(v->ticket); v->ticket = 0;
    }
    v->import_pending = false;
    if (v->confirmation == S_IMPORT) v->confirmation = 0;
    lv_timer_pause(v->timer);
}
void ui_speech_settings_stop(void)
{
    view_t *v = page(); if (!v) return;
    ui_speech_settings_pause(); files_picker_cancel(current->screen);
    if (v->ticket) speech_settings_import_discard(v->ticket);
    lv_timer_delete(v->timer); close_editor(v); ui_settings_list_destroy(v->list);
    speech_config_wipe(v, sizeof(*v)); epd_app_free(v); current->feature = NULL;
}

bool ui_speech_settings_action(int *target)
{
    view_t *v = page(); if (!v || !target) return false;
    int key = *target;
    if (key == S_CONFIRM) {
        if (!current->popup || current->popup_serial != v->confirmation_serial) return true;
        int action = v->confirmation; v->confirmation = 0; popup_close();
        if (action == S_IMPORT) {
            v->job.candidate_id = v->candidate; v->job.card_session = v->card_session;
            if (submit(v, SPEECH_SETTINGS_IMPORT)) v->candidate = 0;
        } else if (action == S_DELETE) {
            v->job.provider = v->draft.provider; submit(v, SPEECH_SETTINGS_REMOVE);
        } else if (action == S_DISCARD) {
            speech_config_wipe(&v->draft, sizeof(v->draft)); v->dirty = false; v->mode = V_ROOT;
        }
        v->redraw = true; render(v); return true;
    }
    if (current->popup) return false;
    if (v->editor) {
        if (key == NAV_BACK || key == S_INPUT_CANCEL) close_editor(v);
        else if (key == S_INPUT_SAVE) {
            size_t capacity; char *dst = field(v, &capacity);
            const char *text = lv_textarea_get_text(v->input); size_t n = strlen(text);
            bool valid = (n || v->input_key == S_WORKSPACE) && n < capacity;
            for (size_t i = 0; valid && i < n; ++i) if ((unsigned char)text[i] < 33 || (unsigned char)text[i] > 126) valid = false;
            if (!valid) show_error(EPD_SPEECH_INVALID);
            else { memcpy(dst, text, n+1); v->dirty = true; close_editor(v); }
        }
        render(v); return true;
    }
    if (key == NAV_BACK) {
        if (v->mode == V_ROOT) return false;
        if (v->ticket) return true;
        if (v->mode == V_ACCOUNT && v->dirty) { confirm(v, S_DISCARD, LV_EXT_STR_ID(speech_discard_confirm)); return true; }
        v->mode = v->mode == V_RATE || v->mode == V_VOICE ? V_ACCOUNT : V_ROOT;
        v->redraw = true; render(v); return true;
    }
    if (v->ticket) return true;
    if (key > S_PROVIDER && key < S_PROVIDER+SPEECH_PROVIDER_COUNT && v->mode == V_ROOT) {
        speech_provider_t p = (speech_provider_t)(key-S_PROVIDER);
        speech_config_wipe(&v->draft, sizeof(v->draft)); v->draft.provider = p;
        v->keep_credentials = v->snapshot.accounts[p].configured;
        v->draft.enabled = v->keep_credentials ? v->snapshot.accounts[p].enabled : true;
        v->draft.rate = v->keep_credentials && v->snapshot.accounts[p].rate ? v->snapshot.accounts[p].rate : 1;
        if (v->keep_credentials) strcpy(v->draft.voice, v->snapshot.accounts[p].voice);
        v->mode = V_ACCOUNT; v->dirty = false;
    } else if (key >= S_CHOOSE && key < S_CHOOSE+SPEECH_PROVIDER_COUNT && (v->mode == V_ASR || v->mode == V_TTS)) {
        v->job.provider = (speech_provider_t)(key-S_CHOOSE); v->job.kind = v->mode == V_ASR ? EPD_SPEECH_ASR : EPD_SPEECH_TTS;
        submit(v, SPEECH_SETTINGS_DEFAULT);
    } else if (key >= S_VOICE_CHOOSE && key < S_VOICE_CHOOSE+8 && v->mode == V_VOICE) {
        size_t count; const speech_voice_t *voices = speech_settings_voices(v->draft.provider, &count);
        if ((size_t)(key-S_VOICE_CHOOSE) < count) { strcpy(v->draft.voice, voices[key-S_VOICE_CHOOSE].id); v->dirty = true; v->mode = V_ACCOUNT; }
    } else if (key >= S_RATE_CHOOSE && key < S_RATE_CHOOSE+7 && v->mode == V_RATE) {
        v->draft.rate = key == S_RATE_CHOOSE && v->draft.provider == SPEECH_TENCENT ? 0.6f : 0.5f+0.25f*(key-S_RATE_CHOOSE);
        v->dirty = true; v->mode = V_ACCOUNT;
    } else switch (key) {
    case S_ASR: v->mode = V_ASR; break;
    case S_TTS: v->mode = V_TTS; break;
    case S_VOICE_NONE: v->draft.voice[0] = 0; v->dirty = true; v->mode = V_ACCOUNT; break;
    case S_VOICE: v->mode = V_VOICE; break;
    case S_RATE: v->mode = V_RATE; break;
    case S_ENABLED: v->draft.enabled = !v->draft.enabled; v->dirty = true; break;
    case S_APP_ID: case S_SECRET_ID: case S_SECRET: case S_WORKSPACE: edit(v, key); break;
    case S_SAVE:
        v->job.account = v->draft;
        if (!v->job.account.voice[0]) v->job.account.rate = 0;
        v->job.keep_credentials = v->keep_credentials;
        submit(v, SPEECH_SETTINGS_SAVE_ACCOUNT); break;
    case S_DELETE: confirm(v, S_DELETE, LV_EXT_STR_ID(speech_delete_confirm)); break;
    case S_IMPORT: if (!files_picker_open_json(imported, v)) show_error(EPD_SPEECH_BUSY); return true;
    case S_RELOAD:
        if ((!v->snapshot.initialized || v->snapshot.initialization_error != EPD_SPEECH_OK) &&
            !v->snapshot.busy && !v->snapshot.stopping) submit(v, SPEECH_SETTINGS_RELOAD);
        break;
    case S_LANGUAGE: v->language = (epd_speech_language_t)((v->language+1)%3); break;
    case S_TEST_ASR: start_preview(v, EPD_SPEECH_ASR); break;
    case S_TEST_TTS: start_preview(v, EPD_SPEECH_TTS); break;
    case S_FINISH:
        if (preview.session) { epd_speech_error_t e = epd_speech_finish(preview.owner, preview.session); if (e) show_error(e); else preview.finishing = true; }
        break;
    case S_CANCEL: preview.cancelling = true; break;
    case S_PAUSE: case S_RESUME:
        if (preview.session) { epd_speech_error_t e = key == S_PAUSE ? epd_speech_pause(preview.owner, preview.session) : epd_speech_resume(preview.owner, preview.session); if (e) show_error(e); }
        break;
    default: return false;
    }
    v->redraw = true; render(v); return true;
}
