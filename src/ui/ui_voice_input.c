/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_voice_input.h"
#include "ui_shell.h"
#include "ui_font.h"
#include "icons/ui_icons.h"
#include "platform/epd_ui_scale.h"
#include "platform/epd_i18n.h"
#include "platform/epd_refresh.h"
#include "platform/epd_keyboard_internal.h"
#include "app_service.h"
#include "speech_input.h"
#include <platform/epd_app.h>
#include <stdio.h>
#include <string.h>

/* UI state lives on the GUI thread; speech workers only publish owned events. */
static struct {
    lv_obj_t *target, *screen, *panel, *status, *preview, *time;
    lv_obj_t *record, *record_icon, *keyboard, *close;
    epd_service_t *owner;
    speech_input_t input;
    char *draft, *committed;
    uint32_t target_id, scale, locale, last_paint;
    bool initialized, blocked, dirty, return_keyboard, rejected;
} voice;

static void render(bool force);
static void target_deleted(lv_event_t *event);

static bool displayed(lv_obj_t *object)
{
    for (; object; object = lv_obj_get_parent(object))
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

static bool enabled(lv_obj_t *object)
{
    for (; object; object = lv_obj_get_parent(object))
        if (lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    return true;
}

static bool editable(lv_obj_t *object)
{
    return object && lv_obj_check_type(object, &lv_textarea_class) &&
        lv_obj_get_screen(object) == lv_screen_active() && displayed(object) &&
        lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE) &&
        enabled(object) &&
        !lv_textarea_get_password_mode(object) && !lv_textarea_get_accepted_chars(object);
}

static void track(lv_obj_t *target)
{
    if (voice.target == target) return;
    if (voice.target) lv_obj_remove_event_cb(voice.target, target_deleted);
    voice.target = target;
    ++voice.target_id;
    if (target && !lv_obj_add_event_cb(target, target_deleted, LV_EVENT_DELETE, NULL))
        voice.target = NULL;
}

static void target_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) != voice.target) return;
    voice.target = NULL;
    ++voice.target_id;
    ui_voice_input_close();
}

static void pointer_target(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_param(event);
    while (object && !lv_obj_check_type(object, &lv_textarea_class))
        object = lv_obj_get_parent(object);
    if (!object || voice.blocked) return;
    if (object != voice.target) ui_voice_input_close();
    track(editable(object) ? object : NULL);
}

static lv_obj_t *bound_keyboard(lv_obj_t *root)
{
    if (!displayed(root)) return NULL;
    if (lv_obj_check_type(root, &lv_keyboard_class)) return root;
    for (int32_t i = (int32_t)lv_obj_get_child_count(root) - 1; i >= 0; --i) {
        lv_obj_t *found = bound_keyboard(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

bool ui_voice_input_available(void)
{
    if (voice.blocked) return false;
    /* Search only on activation, including editors opened without a pointer. */
    lv_obj_t *keyboard = bound_keyboard(lv_screen_active());
    if (keyboard) {
        lv_obj_t *target = lv_keyboard_get_textarea(keyboard);
        track(epd_keyboard_allows_voice(keyboard) && editable(target) ? target : NULL);
    }
    return editable(voice.target);
}

static uint32_t characters(const char *text, size_t bytes)
{
    uint32_t count = 0;
    for (size_t i = 0; i < bytes; ++i)
        if (((unsigned char)text[i] & 0xc0) != 0x80) ++count;
    return count;
}

static epd_speech_error_t commit(void *user, uint32_t target, const char *text, size_t bytes)
{
    (void)user;
    if (!voice.panel || voice.blocked || target != voice.target_id ||
        voice.screen != lv_screen_active() || !editable(voice.target)) return EPD_SPEECH_CANCELLED;
    uint32_t limit = lv_textarea_get_max_length(voice.target);
    const char *old = lv_textarea_get_text(voice.target);
    size_t old_bytes = strlen(old);
    if (limit && characters(old, old_bytes) + characters(text, bytes) > limit)
        return EPD_SPEECH_TEXT_TOO_LONG;
    if (bytes >= EPD_SPEECH_TEXT_BYTES) return EPD_SPEECH_TEXT_TOO_LONG;
    memcpy(voice.committed, text, bytes);
    voice.committed[bytes] = 0;
    /* Native insertion honors the cursor and delivers the editor's normal
     * VALUE_CHANGED notification. The callback may close/delete that editor. */
    if (!limit) lv_textarea_add_text(voice.target, voice.committed);
    else {
        /* LVGL inserts limited text character by character. Revalidate between
         * notifications because an editor callback may delete its own page. */
        for (size_t offset = 0; offset < bytes;) {
            if (!voice.panel || target != voice.target_id || !editable(voice.target))
                return EPD_SPEECH_CANCELLED;
            size_t length = 1;
            while (offset + length < bytes && ((unsigned char)text[offset + length] & 0xc0) == 0x80)
                ++length;
            if (length > 4) return EPD_SPEECH_PROTOCOL;
            char character[5];
            memcpy(character, text + offset, length); character[length] = 0;
            lv_textarea_add_text(voice.target, character);
            offset += length;
        }
    }
    if (target != voice.target_id || !voice.target || !voice.panel) return EPD_SPEECH_CANCELLED;
    if (strlen(lv_textarea_get_text(voice.target)) != old_bytes + bytes) {
        voice.rejected = true;
        return EPD_SPEECH_INVALID;
    }
    return EPD_SPEECH_OK;
}

static const char *error_text(epd_speech_error_t error)
{
    switch (error) {
    case EPD_SPEECH_BUSY: return epd_get_str(speech_busy);
    case EPD_SPEECH_NOT_CONFIGURED: return epd_get_str(speech_not_configured);
    case EPD_SPEECH_AUTH: return epd_get_str(speech_auth_error);
    case EPD_SPEECH_QUOTA: return epd_get_str(speech_quota_error);
    case EPD_SPEECH_NO_NETWORK: case EPD_SPEECH_CONNECT: return epd_get_str(speech_network_error);
    case EPD_SPEECH_TLS: return epd_get_str(speech_tls_error);
    case EPD_SPEECH_TIMEOUT: return epd_get_str(speech_timeout);
    case EPD_SPEECH_NO_MEMORY: return epd_get_str(platform_no_memory);
    case EPD_SPEECH_INPUT_UNAVAILABLE: return epd_get_str(speech_input_error);
    case EPD_SPEECH_INPUT_OVERRUN: return epd_get_str(voice_overrun);
    case EPD_SPEECH_TEXT_TOO_LONG: return epd_get_str(voice_full);
    default: return epd_get_str(speech_service_error);
    }
}

static void label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static lv_obj_t *flat(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_style_text_color(object, lv_color_black(), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    return object;
}

enum { CLOSE, RECORD, KEYBOARD };
static void control(lv_event_t *event)
{
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (action == CLOSE) { ui_voice_input_close(); return; }
    if (action == KEYBOARD) {
        if (voice.input.active && voice.input.accepting) {
            voice.return_keyboard = true;
            speech_input_finish(&voice.input);
        } else ui_voice_input_close();
    } else if (voice.input.active) {
        if (voice.input.accepting && voice.input.snapshot.state != EPD_SPEECH_FINALIZING)
            speech_input_finish(&voice.input);
    } else {
        epd_speech_request_t request = {0};
        request.struct_size = sizeof(request); request.version = EPD_SPEECH_VERSION;
        request.language = EPD_SPEECH_ZH_EN;
        snprintf(request.title, sizeof(request.title), "%s", epd_get_str(voice_title));
        voice.committed[0] = 0;
        voice.rejected = false;
        voice.return_keyboard = false;
        if (editable(voice.target) && !voice.blocked)
            speech_input_start(&voice.input, voice.owner, &request, voice.target_id);
    }
    voice.dirty = true;
    render(true);
}

static lv_obj_t *button(lv_obj_t *parent, const void *source, unsigned action)
{
    lv_obj_t *object = flat(parent);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, control, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    lv_obj_t *image = lv_image_create(object);
    lv_image_set_src(image, source);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    return object;
}

static void layout(void)
{
    int w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
    int nav = epd_ui_touch(84), padding = epd_ui_px(32);
    int ph = LV_MIN(epd_ui_px(460), h - nav - epd_ui_px(160));
    lv_obj_t *keyboard = bound_keyboard(lv_screen_active());
    if (keyboard) {
        lv_area_t area;
        lv_obj_get_coords(keyboard, &area);
        ph = LV_MAX(ph, h - nav - area.y1);
    }
    ph = LV_MIN(ph, h - nav - epd_ui_px(80));
    lv_obj_set_pos(voice.panel, 0, h - nav - ph);
    lv_obj_set_size(voice.panel, w, ph);
    lv_obj_set_style_text_font(voice.status, ui_font_body(), 0);
    lv_obj_set_style_text_font(voice.preview, ui_font_body(), 0);
    lv_obj_set_style_text_font(voice.time, ui_font_small(), 0);
    lv_obj_set_pos(voice.status, padding, epd_ui_px(28));
    lv_obj_set_width(voice.status, w - 2 * padding);
    lv_obj_set_pos(voice.preview, padding, epd_ui_px(96));
    lv_obj_set_size(voice.preview, w - 2 * padding, LV_MAX(epd_ui_px(60), ph - epd_ui_px(290)));
    lv_obj_set_pos(voice.time, padding, ph - epd_ui_px(180));
    lv_obj_set_width(voice.time, w - 2 * padding);
    int size = epd_ui_touch(104), side = epd_ui_touch(84), y = ph - size - epd_ui_px(24);
    lv_obj_set_size(voice.record, size, size);
    lv_obj_set_pos(voice.record, (w - size) / 2, y);
    lv_obj_set_size(voice.close, side, side);
    lv_obj_set_pos(voice.close, padding, y + (size - side) / 2);
    lv_obj_set_size(voice.keyboard, side, side);
    lv_obj_set_pos(voice.keyboard, w - padding - side, y + (size - side) / 2);
    lv_obj_t *buttons[] = {voice.close, voice.record, voice.keyboard};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *image = lv_obj_get_child(buttons[i], 0);
        lv_obj_set_size(image, epd_ui_px(i == 1 ? 52 : 40), epd_ui_px(i == 1 ? 52 : 40));
        lv_obj_center(image);
    }
    voice.scale = epd_ui_scale_revision();
}

static void render(bool force)
{
    if (!voice.panel) return;
    bool localized = voice.locale != epd_locale_revision();
    if (voice.scale != epd_ui_scale_revision()) { layout(); force = true; }
    if (!force && !localized && (!voice.dirty || lv_tick_elaps(voice.last_paint) < 1000)) return;
    const char *state = epd_get_str(voice_ready);
    const char *preview = voice.input.draft && *voice.input.draft ? voice.input.draft : voice.committed;
    bool error = voice.input.error != EPD_SPEECH_OK && voice.input.error != EPD_SPEECH_CANCELLED;
    if (!editable(voice.target)) state = epd_get_str(voice_choose_field);
    else if (error) state = voice.rejected ? epd_get_str(voice_rejected) : error_text(voice.input.error);
    else if (voice.input.active) {
        state = voice.input.snapshot.state == EPD_SPEECH_PREPARING ? epd_get_str(voice_connecting) :
            voice.input.snapshot.state == EPD_SPEECH_RECORDING ? epd_get_str(voice_listening) :
            epd_get_str(voice_finishing);
    } else if (voice.input.snapshot.session) state = epd_get_str(voice_done);
    label_text(voice.status, state);
    label_text(voice.preview, preview ? preview : "");
    lv_obj_update_layout(voice.status);
    int32_t preview_y = lv_obj_get_y(voice.status) + lv_obj_get_height(voice.status) + epd_ui_px(24);
    int32_t preview_h = LV_MAX(0, lv_obj_get_y(voice.time) - epd_ui_px(20) - preview_y);
    if (lv_obj_get_y(voice.preview) != preview_y) lv_obj_set_y(voice.preview, preview_y);
    if (lv_obj_get_height(voice.preview) != preview_h) lv_obj_set_height(voice.preview, preview_h);
    uint32_t elapsed = voice.input.snapshot.elapsed_ms / 1000u;
    /* Coarse elapsed updates avoid a refresh for every second of silence. */
    if (voice.input.active && elapsed < 590) elapsed = elapsed / 10 * 10;
    char time[32] = "";
    if (voice.input.snapshot.session)
        snprintf(time, sizeof(time), "%02lu:%02lu / 10:00", (unsigned long)(elapsed / 60), (unsigned long)(elapsed % 60));
    label_text(voice.time, time);
    const void *source = voice.input.active ? &ui_icon_stop : &ui_icon_microphone;
    if (lv_image_get_src(voice.record_icon) != source) lv_image_set_src(voice.record_icon, source);
    bool disabled = !editable(voice.target) ||
        (voice.input.active && (!voice.input.accepting || voice.input.snapshot.state == EPD_SPEECH_FINALIZING));
    if (disabled) lv_obj_add_state(voice.record, LV_STATE_DISABLED);
    else lv_obj_remove_state(voice.record, LV_STATE_DISABLED);
    lv_opa_t opa = disabled ? LV_OPA_40 : LV_OPA_COVER;
    if (lv_obj_get_style_opa(voice.record, 0) != opa) lv_obj_set_style_opa(voice.record, opa, 0);
    voice.locale = epd_locale_revision();
    voice.dirty = false;
    voice.last_paint = lv_tick_get();
}

static bool process_events(epd_service_t *service, bool stopping)
{
    if (stopping) {
        ui_voice_input_close();
        voice.input.active = false;
        voice.owner = NULL;
        return true;
    }
    /* Eligibility is rechecked before every commit, including queued finals. */
    if (voice.panel && (voice.blocked || voice.screen != lv_screen_active() ||
        (voice.target && !editable(voice.target)) || (voice.input.active && !voice.target)))
        ui_voice_input_close();
    epd_speech_event_t event;
    unsigned count = 0;
    while (count < 4 && epd_speech_poll(service, &event) == EPD_SPEECH_OK) {
        epd_speech_state_t previous = voice.input.snapshot.state;
        bool matched = speech_input_event(&voice.input, &event);
        bool state_changed = previous != voice.input.snapshot.state;
        if (matched && voice.panel && event.kind == EPD_SPEECH_EVENT_TERMINAL)
            lv_display_trigger_activity(NULL);
        epd_speech_event_release(&event);
        if (matched) { voice.dirty = true; render(state_changed || !voice.input.active); }
        ++count;
    }
    speech_input_refresh(&voice.input);
    if (voice.return_keyboard && !voice.input.active) ui_voice_input_close();
    epd_service_ui_request(service, count == 4 ? 0 : voice.input.active ? 1000 : 60000);
    return false;
}

static bool flush(void)
{
    ui_voice_input_close();
    speech_input_refresh(&voice.input);
    return !voice.input.active;
}

void ui_voice_input_init(void)
{
    if (voice.initialized) return;
    voice.initialized = true;
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
            lv_indev_add_event_cb(indev, pointer_target, LV_EVENT_PRESSED, NULL);
}

static void open_input(lv_obj_t *target, bool explicit_target)
{
    if (voice.blocked || voice.panel || ui_shell_locked()) return;
    ui_voice_input_init();
    if (explicit_target) track(editable(target) ? target : NULL);
    bool ready = explicit_target ? editable(voice.target) : ui_voice_input_available();
    if (!voice.owner) {
        static const epd_background_t definition = {.ui_process = process_events, .flush = flush};
        voice.owner = app_service_start("voicein", &definition, NULL);
    }
    if (!voice.draft) voice.draft = lv_malloc_zeroed(EPD_SPEECH_TEXT_BYTES);
    if (!voice.committed) voice.committed = lv_malloc_zeroed(EPD_SPEECH_TEXT_BYTES);
    if (!voice.draft || !voice.committed || !voice.owner) return;
    if (!voice.input.active) {
        speech_input_init(&voice.input, voice.draft, EPD_SPEECH_TEXT_BYTES, commit, NULL);
        voice.committed[0] = 0;
        voice.rejected = false;
    }
    voice.screen = lv_screen_active();
    voice.return_keyboard = false;
    voice.panel = flat(lv_layer_top());
    lv_obj_add_flag(voice.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(voice.panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(voice.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(voice.panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(voice.panel, 2, 0);
    lv_obj_set_style_border_color(voice.panel, lv_color_hex(0xaaaaaa), 0);
    voice.status = label(voice.panel);
    voice.preview = label(voice.panel);
    voice.time = label(voice.panel);
    lv_obj_set_style_text_color(voice.time, lv_color_hex(0x666666), 0);
    voice.close = button(voice.panel, &ui_icon_close, CLOSE);
    voice.record = button(voice.panel, &ui_icon_microphone, RECORD);
    voice.record_icon = lv_obj_get_child(voice.record, 0);
    lv_obj_set_style_radius(voice.record, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(voice.record, 2, 0);
    lv_obj_set_style_border_color(voice.record, lv_color_black(), 0);
    voice.keyboard = button(voice.panel, &ui_icon_keyboard, KEYBOARD);
    if (!epd_app_input_group(voice.panel)) { ui_voice_input_close(); return; }
    epd_app_input_back(voice.panel, voice.close);
    layout();
    epd_refresh_request_interaction();
    render(true);
    /* The navigation microphone is the explicit recording gesture. */
    if (ready && !voice.input.active) {
        lv_obj_send_event(voice.record, LV_EVENT_CLICKED, NULL);
    }
}

void ui_voice_input_open(void) { open_input(NULL, false); }
void ui_voice_input_open_for(lv_obj_t *target) { open_input(target, true); }

void ui_voice_input_close(void)
{
    if (voice.input.accepting) speech_input_cancel(&voice.input);
    voice.return_keyboard = false;
    if (voice.panel) {
        lv_obj_delete(voice.panel);
        voice.panel = NULL;
        epd_refresh_request_interaction();
    }
}

bool ui_voice_input_visible(void) { return voice.panel != NULL; }
bool ui_voice_input_recording(void) { return voice.panel && voice.input.active && voice.input.accepting; }

void ui_voice_input_process(bool blocked)
{
    voice.blocked = blocked;
    if (blocked || (voice.target && !editable(voice.target)) ||
        (voice.panel && voice.screen != lv_screen_active())) {
        ui_voice_input_close();
        track(NULL);
    }
    if (voice.panel) {
        speech_input_refresh(&voice.input);
        voice.dirty = true;
        render(false);
    }
}
